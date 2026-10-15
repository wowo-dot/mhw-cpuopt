"""Frame-time and CPU summary of one run, grouped by marker and switch state.

  python -B tools/report.py <run> [--pid N] [--skip 1] [--threads 6]

Per group (marker, toggles):
- game frames (Astera Loader's al.frame, exact time stamps): frame rate, mean
  frame time, the median over windows of each window's p50/p95/p99 frame
  interval, the worst interval and frames over 25 ms;
- the game update itself (al.frame BEGIN to END) and the Present calls
  (al.d3d12: time inside Present; long calls mean the GPU holds the CPU back);
- sRender's frames against game frames (should match);
- the busiest threads' CPU time per frame (cycle counted), the game and
  present threads named.
The first `skip` windows after each switch or marker change are left out.
"""
import argparse
import collections
import statistics

from run import newest_log, rows, run_folder


def weighted_mean(ws, path):
    count = sum(w[path[0]][path[1]]['count'] for w in ws)
    total = sum(w[path[0]][path[1]]['mean'] * w[path[0]][path[1]]['count'] for w in ws)
    return total / count if count else 0.0


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
    loader = start.get('loader', {})
    print(f"{log.name}: v{start['version']} {start['commit']}  exe tested={start['exe']['tested_build']}  {start['cpu']['brand']}")
    print(f"  loader {loader.get('name')} API {loader.get('api')} game {loader.get('game_version')}  affinity {start['cpu']['process_affinity']}  "
          f"config.ini [CPU] {start['game_config']['config_cpu']}")
    print(f"  {cfg.get('ScreenMode')} {cfg.get('Resolution')} scaling={cfg.get('ResolutionScaling')} dlss={cfg.get('NVIDIA DLSS')} "
          f"frame counter={clock.get('reason')} ({clock.get('scanner')})")
    modules = [m[0] for e in events if e.get('event') == 'modules' for m in e['modules']]
    extra = sorted({m.rsplit('\\', 1)[-1] for m in modules if 'windows' not in m.lower()})
    print(f'  non-Windows modules: {", ".join(extra)}')

    groups = collections.OrderedDict()
    last_key, settled = None, 0
    for w in (e for e in events if e.get('event') == 'window'):
        order = w.get('order', {}).get('mode', 0)
        key = (w['marker'], w['toggles'] | (order << 8), w.get('job', {}).get('applied'))
        settled = settled + 1 if key == last_key else 0
        last_key = key
        if settled < a.skip or not w['update']['frames']:
            continue
        groups.setdefault(key, []).append(w)
    for (marker, toggles, jobs), ws in groups.items():
        frames = sum(w['update']['frames'] for w in ws)
        seconds = sum(w['ms'] for w in ws) / 1000
        med = lambda f: statistics.median(f(w) for w in ws)
        iv = lambda k: med(lambda w: w['update']['interval'][k])
        state = ws[-1].get('job', {}).get('state', '-')
        order = toggles >> 8
        toggles &= 0xFF
        if order:
            o = ws[-1]['order']
            state += f', job order {("reversed", "learned")[order - 1]}: {sum(w["order"]["rounds"] for w in ws)} rounds reordered, '                      f'{sum(w["order"]["moved"] for w in ws)} jobs moved, {o["classes"]} classes known'
            if not sum(w['order']['rounds'] for w in ws):
                state += ' (NOTHING REORDERED)' 
        print(f'\n== marker {marker or "-"}  toggles {toggles}  job threads {jobs} ({state}): {len(ws)} windows, '
              f'{seconds:.0f} s, {frames} game frames')
        print(f'  game frames {frames / seconds:.1f} fps (windows {min(w["update"]["fps"] for w in ws):.1f}..'
              f'{max(w["update"]["fps"] for w in ws):.1f})  mean {weighted_mean(ws, ("update", "interval")):.3f} ms  '
              f'median of window p50 {iv("p50"):.2f} p95 {iv("p95"):.2f} p99 {iv("p99"):.2f} ms  '
              f'worst {max(w["update"]["interval"]["max"] for w in ws):.1f} ms  over 25 ms: '
              f'{sum(w["update"]["interval"]["over_25ms"] for w in ws)}')
        print(f'  game update {weighted_mean(ws, ("update", "work")):.3f} ms mean (median p95 '
              f'{med(lambda w: w["update"]["work"]["p95"]):.2f})  Present {sum(w["present"]["count"] for w in ws) / seconds:.1f}/s, '
              f'inside Present {weighted_mean(ws, ("present", "call")):.3f} ms mean (median p95 {med(lambda w: w["present"]["call"]["p95"]):.2f})  '
              f'sRender frames {sum(w["render"]["frames"] for w in ws)}')
        print(f'  process CPU {sum(w["process_cpu_ms"] for w in ws) / frames:.2f} ms/frame; threads {med(lambda w: w["thread_count"])}')
        per_thread, label = collections.defaultdict(float), {}
        for w in ws:
            roles = {w['update']['thread']: 'GAME', w['present']['thread']: 'PRESENT'}
            for t in w['threads']:
                per_thread[t[0]] += t[1]
                label[t[0]] = ' '.join(x for x in (roles.get(t[0], ''), t[5] or t[4], '[plugin]' if 'own' in t[6] else '') if x)
        for tid, cpu_ms in sorted(per_thread.items(), key=lambda kv: -kv[1])[:a.threads]:
            print(f'    {cpu_ms / frames:7.3f} ms/frame  {100 * cpu_ms / (seconds * 1000):5.1f}% of a CPU  tid {tid}  {label[tid]}')
        busy = [statistics.mean(w['cpu_busy'][i] for w in ws) for i in range(len(ws[0]['cpu_busy']))]
        print('  CPU busy %: ' + ' '.join(f'{b:.0f}' for b in busy))


if __name__ == '__main__':
    main()
