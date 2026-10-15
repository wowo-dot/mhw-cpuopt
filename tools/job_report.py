"""Where the game thread's time in sMain::executeJob goes, and what the job threads run.

  python -B tools/job_report.py <run> [--pid N] [--marker M] [--top 20]

From the stack sampler's "profile" events:
- game thread: samples inside executeJob (its tail chunk, 0x225E9F0, which
  holds the release, the main thread's own share and the spin), split into
  spinning (waiting for job threads), running jobs itself (the job runner
  0x225F620 and below) and the rest (semaphore release and the like), per
  caller: the game function that started the job round;
- the jobs the game thread ran itself, by job function;
- job threads (start 0x225F2B0): waiting for work against running jobs, by
  job function (the frame the worker loop 0x225F2F0 called).
Counts are wall-clock samples; shares are of each thread's own samples.
"""
import argparse
import collections

from profile_report import GAME, RAW, UNKNOWN, Symbolizer
from run import newest_log, rows, run_folder

# IDA function ranges (clean database). The exe's unwind table splits them into
# several entries, which the sampler records separately, so frames are matched
# by range, not by start.
EXECUTE_JOB = (0x225E840, 0x225EAFE)  # release, own share, spin (the spin's entry starts at 0x225E9F0)
JOB_RUNNER = (0x225F620, 0x225F8D4)   # the main thread's own share
WORKER_LOOP = (0x225F2F0, 0x225F614)  # a job thread's loop
JOB_THREAD_START = '+0x225f2b0'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run')
    p.add_argument('--pid', type=int)
    p.add_argument('--marker')
    p.add_argument('--top', type=int, default=20)
    a = p.parse_args()
    run = run_folder(a.run)
    log = run / f'cpuopt-{a.pid}.jsonl' if a.pid else newest_log(run)
    events = rows(log)
    paths, starts, game_threads = {}, {}, set()
    for e in events:
        if e.get('event') == 'modules':
            for m in e['modules']:
                paths[m[0].rsplit('\\', 1)[-1].lower()] = m[0]
        elif e.get('event') == 'window':
            for t in e['threads']:
                starts[t[0]] = t[4]
            if e.get('update', {}).get('thread'):
                game_threads.add(e['update']['thread'])
    profiles = [e for e in events if e.get('event') == 'profile' and (a.marker is None or e['marker'] == a.marker)]
    if not profiles:
        raise SystemExit('no matching profile events')
    sym = Symbolizer(run, paths)
    label = lambda f: sym.label(*f) if f else '(none)'
    is_game = lambda f, span: bool(f) and f[0] == GAME and span[0] <= (f[1] & ~RAW) < span[1]

    game_total = 0
    split = collections.Counter()           # (caller, kind) -> samples
    caller_total = collections.Counter()
    own_jobs = collections.Counter()
    worker_total, worker_wait = 0, 0
    worker_jobs = collections.Counter()
    worker_threads = set()
    spin_samples = spin_unreadable = 0
    spin_busy = collections.Counter()
    spin_jobs = collections.Counter()  # (caller label, job label) -> samples it was still running
    for e in profiles:
        names = e['modules']
        frames = [(names[m] if m != UNKNOWN and m < len(names) else None, r) for m, r in e['frames']]
        probe = e.get('spin_probe')
        if probe:
            spin_samples += probe['samples']
            spin_unreadable += probe['unreadable']
            for busy, n in enumerate(probe['busy']):
                spin_busy[busy] += n
            for caller, in_exe, function, n in e.get('spin_jobs', []):
                job = label((GAME, function)) if in_exe else f'{function:#x} (outside the exe)'
                spin_jobs[(label(frames[caller]) if caller < len(frames) else '?', job)] += n
        for tid, count, _, stack in e['stacks']:
            st = [frames[i] for i in stack]
            if tid in game_threads:
                game_total += count
                i = next((k for k, f in enumerate(st) if is_game(f, EXECUTE_JOB)), None)
                if i is None:
                    continue
                caller = label(st[i + 1]) if i + 1 < len(st) else '(none)'
                if i == 0:
                    kind = 'spin'
                elif any(is_game(f, JOB_RUNNER) for f in st[:i]):
                    kind = 'own jobs'
                    k = next(k for k, f in enumerate(st[:i]) if is_game(f, JOB_RUNNER))
                    own_jobs[label(st[k - 1]) if k > 0 else '(runner itself)'] += count
                else:
                    kind = 'release/other'
                split[(caller, kind)] += count
                caller_total[caller] += count
            elif starts.get(tid, '').endswith(JOB_THREAD_START):
                worker_threads.add(tid)
                worker_total += count
                k = next((k for k, f in enumerate(st) if is_game(f, WORKER_LOOP)), None)
                if k is None:
                    continue
                if k == 0 or not st[k - 1][0] or st[k - 1][0] != GAME:
                    worker_wait += count  # in the loop itself or the semaphore wait below it
                    continue
                worker_jobs[label(st[k - 1])] += count

    in_jobs = sum(caller_total.values())
    print(f'{log.name}: {len(profiles)} profile windows, markers {sorted({e["marker"] for e in profiles})}')
    print(f'\n== game thread: {game_total} samples; inside executeJob {in_jobs} ({100 * in_jobs / max(game_total, 1):.1f}%)')
    kinds = collections.Counter()
    for (caller, kind), n in split.items():
        kinds[kind] += n
    print('   ' + ', '.join(f'{k} {100 * n / max(game_total, 1):.1f}%' for k, n in kinds.most_common()))
    print(f'-- by caller (the game function that started the job round), % of the game thread')
    print(f'   {"total":>6} {"spin":>6} {"own":>6} {"other":>6}  caller')
    for caller, n in caller_total.most_common(a.top):
        pct = lambda kind: 100 * split[(caller, kind)] / max(game_total, 1)
        print(f'   {100 * n / max(game_total, 1):5.1f}% {pct("spin"):5.1f}% {pct("own jobs"):5.1f}% {pct("release/other"):5.1f}%  {caller}')
    print(f'-- jobs the game thread ran itself, % of the game thread')
    for job, n in own_jobs.most_common(a.top):
        print(f'   {100 * n / max(game_total, 1):5.1f}%  {job}')
    print(f'\n== spin probe: {spin_samples} samples of the game thread spinning ({spin_unreadable} unreadable)')
    if spin_samples:
        print('   busy job slots while it spun: ' + ', '.join(
            f'{k}: {100 * n / spin_samples:.1f}%' for k, n in sorted(spin_busy.items()) if n))
        print('-- still running while it spun, by job round (caller) and job function, % of spin samples')
        for (caller, job), n in spin_jobs.most_common(a.top):
            print(f'   {100 * n / spin_samples:5.1f}%  {job}   [round from {caller}]')
    print(f'\n== job threads: {len(worker_threads)} sampled, {worker_total} samples; waiting for work '
          f'{100 * worker_wait / max(worker_total, 1):.1f}%')
    print('-- jobs they ran, % of their samples')
    for job, n in worker_jobs.most_common(a.top):
        print(f'   {100 * n / max(worker_total, 1):5.1f}%  {n:7d}  {job}')


if __name__ == '__main__':
    main()
