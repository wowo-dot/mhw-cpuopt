"""Frame-time and CPU summary of one run, grouped by marker and switch state.

  python -B tools/report.py <run> [--pid N] [--skip 1] [--threads 6]

Per group (marker, toggles): window count, frame rate and mean frame time
(exact: counter advance over wall time), the median of the windows' poll-timed
percentiles (p50/p95/p99, about +-1 ms per frame), and the busiest threads'
CPU time per frame (cycle counted). The first `skip` windows after each
switch or marker change are left out (settling after a change).
"""
import argparse
import collections
import statistics

from run import newest_log, rows, run_folder


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run')
    p.add_argument('--pid', type=int)
    p.add_argument('--skip', type=int, default=1)
    p.add_argument('--threads', type=int, default=6)
    a = p.parse_args()
    run = run_folder(a.run)
    log = run / f'cpuopt-{a.pid}.jsonl' if a.pid else newest_log(run)
    events = rows(log)
    start = next(e for e in events if e.get('event') == 'start')
    clock = next((e for e in events if e.get('event') == 'frame_clock'), {})
    cfg = start['game_config']['graphics_option']
    print(f"{log.name}: v{start['version']} {start['commit']}  exe tested={start['exe']['tested_build']}  {start['cpu']['brand']}")
    print(f"  affinity {start['cpu']['process_affinity']}  config.ini [CPU] {start['game_config']['config_cpu']}")
    print(f"  {cfg.get('ScreenMode')} {cfg.get('Resolution')} scaling={cfg.get('ResolutionScaling')} dlss={cfg.get('NVIDIA DLSS')} "
          f"frame_clock={clock.get('reason')}")
    modules = [m[0] for e in events if e.get('event') == 'modules' for m in e['modules']]
    extra = sorted({m.rsplit('\\', 1)[-1] for m in modules if 'windows' not in m.lower()})
    print(f'  non-Windows modules: {", ".join(extra)}')

    groups = collections.OrderedDict()
    last_key, settled = None, 0
    for w in (e for e in events if e.get('event') == 'window'):
        key = (w['marker'], w['toggles'])
        settled = settled + 1 if key == last_key else 0
        last_key = key
        if settled < a.skip or not w['frames']:
            continue
        groups.setdefault(key, []).append(w)
    for (marker, toggles), ws in groups.items():
        frames = sum(w['frames'] for w in ws)
        seconds = sum(w['ms'] for w in ws) / 1000
        med = lambda f: statistics.median(f(w) for w in ws)
        print(f'\n== marker {marker or "-"}  toggles {toggles}: {len(ws)} windows, {seconds:.0f} s, {frames} frames')
        print(f'  fps {frames / seconds:.1f} (windows {min(w["fps"] for w in ws):.1f}..{max(w["fps"] for w in ws):.1f})  '
              f'mean frame {1000 * seconds / frames:.3f} ms  median of window p50 {med(lambda w: w["intervals"]["p50"]):.2f} '
              f'p95 {med(lambda w: w["intervals"]["p95"]):.2f} p99 {med(lambda w: w["intervals"]["p99"]):.2f} ms  '
              f'worst max {max(w["intervals"]["max"] for w in ws):.1f} ms  over 25 ms: {sum(w["intervals"]["over_25ms"] for w in ws)}')
        print(f'  process CPU {sum(w["process_cpu_ms"] for w in ws) / frames:.2f} ms/frame; threads {med(lambda w: w["thread_count"])}; '
              f'poll gap {med(lambda w: w["poll_gap_avg_us"])} us')
        per_thread = collections.defaultdict(float)
        label = {}
        for w in ws:
            for t in w['threads']:
                per_thread[t[0]] += t[1]
                label[t[0]] = f'{t[5] or t[4]}' + (' [plugin]' if 'own' in t[6] else '')
        for tid, cpu_ms in sorted(per_thread.items(), key=lambda kv: -kv[1])[:a.threads]:
            print(f'    {cpu_ms / frames:7.3f} ms/frame  {100 * cpu_ms / (seconds * 1000):5.1f}% of a CPU  tid {tid}  {label[tid]}')
        busy = [statistics.mean(w['cpu_busy'][i] for w in ws) for i in range(len(ws[0]['cpu_busy']))]
        print('  CPU busy %: ' + ' '.join(f'{b:.0f}' for b in busy))


if __name__ == '__main__':
    main()
