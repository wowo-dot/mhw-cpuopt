"""Replays traced job rounds (job_timing's "job_trace" events) in other job orders.

  python -B tools/job_sim.py <run> [--pid N] [--marker M] [--threshold-us 20] [--top 8]

Model, per traced round:
- the jobs and their measured durations, in list order;
- the threads that took part, each ready from its first job's start (a thread
  that took no job woke too late to count);
- the gap a thread needs between two jobs (the median measured gap);
- greedy list scheduling: the next job in the list goes to the thread that is
  free first, as the engine's shared job counter does;
- the round ends when its last job ends, plus the measured tail (return and
  spin exit).
Replaying the original order checks the model against the measured round.
Orders compared:
  original      the game's list
  oracle        longest job first by this round's own durations (the best a
                reorder could do; needs knowledge no one has in advance)
  class mean    longest first by each job type's mean duration over all traces
                of that round type (job function and object class)
  class front   job types whose mean is at least --threshold-us moved to the
                front, the rest in the game's order
  reversed      the list backwards
Times in microseconds (TSC MHz from the windows).
"""
import argparse
import collections
import heapq
import statistics
from pathlib import Path

from profile_report import GAME, Symbolizer
from run import newest_log, rows, run_folder

REPO = Path(__file__).resolve().parents[1]


def load_vtables():
    path = REPO / 'local' / 'ida' / 'vtables.tsv'
    out = {}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            rva, _, name = line.partition('\t')
            if name:
                out[int(rva, 16)] = name.replace('::vft', '')
    return out


class Round:
    def __init__(self, event):
        self.caller = event['caller_rva']
        self.duration = event['duration']
        # (key, duration, start, thread) in list order; key = (vtable, function)
        self.jobs = [((v, f), d, s, t) for v, f, s, d, t in event['jobs']]
        ran = [j for j in self.jobs if j[1] > 0]
        self.last_end = max((s + d for _, d, s, _ in ran), default=0)
        self.tail = max(0, self.duration - self.last_end)
        first = {}
        for _, d, s, t in ran:
            first[t] = min(first.get(t, s), s)
        self.ready = sorted(first.values())
        gaps = []
        by_thread = collections.defaultdict(list)
        for _, d, s, t in ran:
            by_thread[t].append((s, d))
        for runs in by_thread.values():
            runs.sort()
            gaps += [b[0] - (a[0] + a[1]) for a, b in zip(runs, runs[1:]) if b[0] >= a[0] + a[1]]
        self.gap = statistics.median(gaps) if gaps else 0

    def simulate(self, order):
        """Round time for the jobs taken in `order` (indices into self.jobs)."""
        if not self.ready:
            return self.duration
        free = [(r, k) for k, r in enumerate(self.ready)]
        heapq.heapify(free)
        end = 0
        for i in order:
            d = self.jobs[i][1]
            at, k = heapq.heappop(free)
            finish = at + d
            end = max(end, finish)
            heapq.heappush(free, (finish + self.gap, k))
        return end + self.tail


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run')
    p.add_argument('--pid', type=int)
    p.add_argument('--marker')
    p.add_argument('--threshold-us', type=float, default=20.0)
    p.add_argument('--top', type=int, default=8)
    a = p.parse_args()
    run = run_folder(a.run)
    log = run / f'cpuopt-{a.pid}.jsonl' if a.pid else newest_log(run)
    events = rows(log)
    paths, mhz, window = {}, 0, None
    rounds = collections.defaultdict(list)
    per_frame = collections.Counter()  # caller -> rounds, from the job_timing totals
    frames = 0
    for e in events:
        if e.get('event') == 'modules':
            for m in e['modules']:
                paths[m[0].rsplit(chr(92), 1)[-1].lower()] = m[0]
        elif e.get('event') == 'window':
            window = e
            mhz = mhz or e.get('tsc_mhz', 0)
        elif window and (a.marker is None or window['marker'] == a.marker):
            if e.get('event') == 'job_trace':
                rounds[e['caller_rva']].append(Round(e))
            elif e.get('event') == 'job_timing':
                frames += window['update']['frames']
                for row in e['callers']:
                    per_frame[row[0]] += row[1]
    if not rounds or not mhz:
        raise SystemExit('no job_trace events (job timing, toggle 4, v0.4.1+)')
    us = lambda cycles: cycles / mhz
    sym = Symbolizer(run, paths)
    vt = load_vtables()
    name = lambda rva: sym.label(GAME, rva).replace('game!', '') if rva else '(outside the exe)'
    print(f'{log.name}: {sum(len(v) for v in rounds.values())} traced rounds; {frames} game frames under job timing')
    for caller, traced in sorted(rounds.items(), key=lambda kv: -per_frame[kv[0]] * statistics.mean(r.duration for r in kv[1])):
        # Each job type's mean duration over every traced round of this type.
        sums = collections.defaultdict(lambda: [0, 0])
        for r in traced:
            for key, d, _, _ in r.jobs:
                if d:
                    sums[key][0] += d
                    sums[key][1] += 1
        mean = {k: s / n for k, (s, n) in sums.items()}
        threshold = a.threshold_us * mhz
        results = collections.defaultdict(list)
        fidelity = []
        for r in traced:
            n = len(r.jobs)
            original = list(range(n))
            sim_original = r.simulate(original)
            fidelity.append(sim_original - r.duration)
            results['measured'].append(r.duration)
            results['original'].append(sim_original)
            results['oracle'].append(r.simulate(sorted(original, key=lambda i: -r.jobs[i][1])))
            results['class mean'].append(r.simulate(sorted(original, key=lambda i: -mean.get(r.jobs[i][0], 0))))
            front = [i for i in original if mean.get(r.jobs[i][0], 0) >= threshold]
            results['class front'].append(r.simulate(front + [i for i in original if mean.get(r.jobs[i][0], 0) < threshold]))
            results['reversed'].append(r.simulate(original[::-1]))
        rpf = per_frame[caller] / frames if frames else 0
        base = statistics.mean(results['original'])
        print(f'\n== {name(caller)}: {len(traced)} traced rounds, {rpf:.2f} per frame, {statistics.mean(len(r.jobs) for r in traced):.0f} jobs, '
              f'{statistics.mean(len(r.ready) for r in traced):.1f} threads, gap {us(statistics.mean(r.gap for r in traced)):.2f} us')
        print(f'   model check: replayed original vs measured, mean {us(statistics.mean(fidelity)):+.1f} us, '
              f'median |error| {us(statistics.median(abs(x) for x in fidelity)):.1f} us')
        for label in ('measured', 'original', 'oracle', 'class mean', 'class front', 'reversed'):
            m = statistics.mean(results[label])
            gain = base - m if label not in ('measured', 'original') else 0
            print(f'   {label:12s} {us(m):8.1f} us/round' + (f'   gain {us(gain):7.1f} us/round, {us(gain) * rpf:7.1f} us/frame' if gain else ''))
        expensive = sorted(((m, k) for k, m in mean.items() if m >= threshold), reverse=True)[:a.top]
        if expensive:
            print(f'   job types at or over {a.threshold_us:.0f} us (mean):')
            for m, (v, f) in expensive:
                print(f'     {us(m):7.1f} us  {name(f)} on {vt.get(v, hex(v) if v else "-")}')


if __name__ == '__main__':
    main()
