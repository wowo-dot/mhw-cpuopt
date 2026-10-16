"""Run folders for in-game measurements of the cpuopt plugin.

  prepare <name> [--dll D] [--toggles N] [--marker M] [--note TEXT] [--player]
      New folder local/runs/<name>; installs the DLL into the MO2 slot if it
      differs (previous slot DLL backed up byte for byte), points the slot's
      .output-dir sidecar at the folder and writes a receipt. --player: no
      sidecar (moved into the folder if there was one), so the plugin starts
      in player mode with the slot's cpuopt.ini (copied in as it was).
  collect <run>
      After a player-mode launch: copies the slot's cpuopt-session.log (or
      cpuopt.log before 0.14.2), cpuopt.ini
      and cpuopt-benchmark.txt into the run folder (then seal it).
  control <run> [--toggles N] [--marker M] [--window-ms N] [--job-threads N]
          [--job-order 1|2] [--order-us N] [--inline-us N] [--time-ms N]
      Changes control.ini while the game runs; the plugin closes the current
      window and logs the change within about 300 ms.
  sweep <run> --marker M --steps 0:45,15:45,0:45,11:45 [--profile]
      job_thread_cap A/B: each step is job threads (0 = the game's own count,
      switch off) and seconds; the sampler stays off unless --profile.
  steps <run> --marker M --steps 0:30,4:60,8/1:30,8/2:30 [--repeat N]
      Any switch states in order: each step is toggles[/job_order] and
      seconds, the list run N times (interleaved A/B); then "M-done" with
      every switch off.
  sequence <run> --marker M [--baseline 60] [--profile 60]
      One scene: marker M with every switch off, then with the sampler on,
      then "M-done".
  status <run> [--last N]
      The last windows of the newest log: fps, frame times, busiest threads.
  seal <run>
      Hash manifest of the finished folder (tools/evidence.py).

The slot path comes from local/machine.json ("slot"), never from defaults in
code. Refuses to install while MonsterHunterWorld.exe runs. The game
directory is never written.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from evidence import game_running, seal

REPO = Path(__file__).resolve().parents[1]
RUNS = REPO / 'local' / 'runs'
BUILD_DLL = REPO / 'build' / 'bin' / 'Release' / 'cpuopt.dll'


def machine():
    path = REPO / 'local' / 'machine.json'
    if not path.exists():
        raise SystemExit(f'missing {path} (machine-local paths: "slot")')
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git_identity():
    def git(*args):
        return subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, text=True, check=True).stdout.strip()
    return dict(commit=git('rev-parse', 'HEAD'), dirty=bool(git('status', '--porcelain', '--untracked-files=no')))


def run_folder(name):
    path = Path(name)
    return path.resolve() if path.is_absolute() or path.exists() else (RUNS / name).resolve()


def read_control(run):
    values = {}
    path = run / 'control.ini'
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            if '=' in line and not line.lstrip().startswith((';', '#', '[')):
                key, value = line.split('=', 1)
                values[key.strip()] = value.strip()
    return values


def write_control(run, values):
    text = '[control]\n' + ''.join(f'{k}={v}\n' for k, v in values.items())
    tmp = run / 'control.ini.tmp'
    tmp.write_text(text, encoding='utf-8', newline='\n')
    os.replace(tmp, run / 'control.ini')  # the plugin never sees a half-written file


def prepare(a):
    if game_running():
        raise SystemExit('close MonsterHunterWorld.exe first')
    slot = Path(machine()['slot'])
    dll = Path(a.dll).resolve() if a.dll else BUILD_DLL
    run = (RUNS / a.name).resolve()
    run.mkdir(parents=True, exist_ok=False)
    slot.parent.mkdir(parents=True, exist_ok=True)
    previous = None
    if slot.exists() and sha(slot) != sha(dll):
        backup = run / 'previous-slot'
        backup.mkdir()
        shutil.copyfile(slot, backup / slot.name)
        previous = dict(path=str(slot), sha256=sha(slot))
    shutil.copyfile(dll, run / dll.name)
    if dll.with_suffix('.map').exists():  # symbols for tools/profile_report.py
        shutil.copyfile(dll.with_suffix('.map'), run / dll.with_suffix('.map').name)
    if not slot.exists() or sha(slot) != sha(dll):
        shutil.copyfile(dll, slot)
    if sha(slot) != sha(dll):
        raise SystemExit('installed DLL does not match')
    sidecar = Path(str(slot) + '.output-dir')
    if a.player:
        if sidecar.exists():
            shutil.move(str(sidecar), str(run / 'previous-sidecar.txt'))
        ini = slot.parent / 'cpuopt.ini'
        if ini.exists():
            shutil.copyfile(ini, run / 'cpuopt.ini.before')
    else:
        sidecar.write_text(str(run) + '\n', encoding='utf-8')
    control = {}
    if a.toggles is not None:
        control['toggles'] = str(a.toggles)
    if a.marker:
        control['marker'] = a.marker
    if control and not a.player:
        write_control(run, control)
    receipt = dict(schema=1, prepared_utc=datetime.now(timezone.utc).isoformat(), run=str(run), slot=str(slot),
                   dll=dll.name, dll_sha256=sha(dll), previous_slot=previous, git=git_identity(), note=a.note,
                   control=control, mode='player' if a.player else 'research')
    with (run / 'run-receipt.json').open('x', encoding='utf-8') as f:
        json.dump(receipt, f, indent=2)
    return receipt


def collect(a):
    if game_running():
        raise SystemExit('close MonsterHunterWorld.exe first')
    run = run_folder(a.run)
    folder = Path(machine()['slot']).parent
    copied = []
    for name in ('cpuopt-session.log', 'cpuopt.log', 'cpuopt.ini', 'cpuopt-benchmark.txt'):
        if (folder / name).exists():
            shutil.copyfile(folder / name, run / name)
            copied.append(name)
    return dict(run=str(run), copied=copied)


def control(a):
    run = run_folder(a.run)
    if not run.is_dir():
        raise SystemExit(f'no run folder {run}')
    values = read_control(run)
    if a.toggles is not None:
        values['toggles'] = str(a.toggles)
    if a.marker is not None:
        values['marker'] = a.marker
    if a.window_ms is not None:
        values['window_ms'] = str(a.window_ms)
    if a.job_threads is not None:
        values['job_threads'] = str(a.job_threads)
    for key in ('job_order', 'order_us', 'inline_us', 'time_ms'):
        if getattr(a, key) is not None:
            values[key] = str(getattr(a, key))
    write_control(run, values)
    return dict(run=str(run), control=values)


def sequence(a):
    """One scene: baseline windows with every switch off, then the sampler."""
    import time
    run = run_folder(a.run)
    steps = [(dict(marker=a.marker, toggles='0'), a.baseline), (dict(marker=a.marker, toggles='1'), a.profile),
             (dict(marker=f'{a.marker}-done', toggles='0'), 0)]
    for values, seconds in steps:
        current = read_control(run)
        current.update(values)
        write_control(run, current)
        print(f'{time.strftime("%H:%M:%S")} {values} for {seconds} s', flush=True)
        time.sleep(seconds)


JOB_THREAD_CAP = 2  # toggles.hpp


def sweep(a):
    """job_thread_cap A/B in one scene, step by step."""
    import time
    run = run_folder(a.run)
    steps = []
    for item in a.steps.split(','):
        threads, seconds = item.split(':')
        steps.append((int(threads), int(seconds)))
    sampler = 1 if a.profile else 0
    for threads, seconds in steps:
        values = read_control(run)
        values['marker'] = a.marker
        if threads:
            values['toggles'] = str(sampler | JOB_THREAD_CAP)
            values['job_threads'] = str(threads)
        else:
            values['toggles'] = str(sampler)
        write_control(run, values)
        print(f'{time.strftime("%H:%M:%S")} job threads {threads or "native"} for {seconds} s', flush=True)
        time.sleep(seconds)
    values = read_control(run)
    values.update(marker=f'{a.marker}-done', toggles='0')
    write_control(run, values)
    print(f'{time.strftime("%H:%M:%S")} done: switch off', flush=True)


def steps(a):
    """Switch states in order, one step each."""
    import time
    run = run_folder(a.run)
    for item in a.steps.split(',') * a.repeat:
        state, seconds = item.split(':')
        toggles, _, order = state.partition('/')  # "8/1": toggles 8 with job_order 1
        values = read_control(run)
        values.update(marker=a.marker, toggles=str(int(toggles, 0)))
        if order:
            values['job_order'] = order
        write_control(run, values)
        print(f'{time.strftime("%H:%M:%S")} toggles {toggles}{" job_order " + order if order else ""} for {seconds} s', flush=True)
        seconds = int(seconds)
        time.sleep(seconds)
    values = read_control(run)
    values.update(marker=f'{a.marker}-done', toggles='0')
    write_control(run, values)
    print(f'{time.strftime("%H:%M:%S")} done: every switch off', flush=True)


def newest_log(run):
    logs = sorted(run.glob('cpuopt-*.jsonl'), key=lambda p: p.stat().st_mtime)
    if not logs:
        raise SystemExit(f'no cpuopt-*.jsonl in {run}')
    return logs[-1]


def rows(path):
    out = []
    with path.open(encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line.startswith('{') and line.endswith('}'):
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass  # a line being written
    return out


def status(a):
    run = run_folder(a.run)
    log = newest_log(run)
    events = rows(log)
    start = next((e for e in events if e.get('event') == 'start'), {})
    clock = next((e for e in events if e.get('event') == 'frame_clock'), {})
    print(f"{log.name}: {len(events)} events; exe tested_build={start.get('exe', {}).get('tested_build')} "
          f"cpu={start.get('cpu', {}).get('brand')} frame_clock={clock.get('reason')}")
    for e in events:
        if e.get('event') in ('control', 'exit', 'stopped', 'log_full'):
            print('  ', json.dumps(e))
    for w in [e for e in events if e.get('event') == 'window'][-a.last:]:
        u, p = w['update'], w['present']
        frames = max(u['frames'], 1)
        roles = {u['thread']: 'game', p['thread']: 'present'}
        top = ', '.join(f"{roles.get(t[0], t[0])}:{t[1] / frames:.2f}" for t in w['threads'][:4])
        job = w.get('job', {})
        print(f"#{w['seq']:4d} t={w['t_ms'] / 1000:7.1f}s {w['marker'] or '-':14s} sw={w['toggles']} "
              f"jobs={job.get('applied', '-')}({job.get('state', '-')}) "
              f"fps={u['fps']:6.1f} p50={u['interval']['p50']:.2f} p95={u['interval']['p95']:.2f} p99={u['interval']['p99']:.2f} "
              f"max={u['interval']['max']:.1f} | update {u['work']['mean']:.2f} ms | present {p['fps']:6.1f}/s "
              f"call {p['call']['mean']:.2f} ms | render {w['render']['frames']} | thr={w['thread_count']} | ms/frame {top}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='action', required=True)
    s = sub.add_parser('prepare')
    s.add_argument('name')
    s.add_argument('--dll')
    s.add_argument('--toggles', type=lambda v: int(v, 0))
    s.add_argument('--marker')
    s.add_argument('--note', default='')
    s.add_argument('--player', action='store_true')
    s = sub.add_parser('collect')
    s.add_argument('run')
    s = sub.add_parser('control')
    s.add_argument('run')
    s.add_argument('--toggles', type=lambda v: int(v, 0))
    s.add_argument('--marker')
    s.add_argument('--window-ms', type=int)
    s.add_argument('--job-threads', type=int)
    s.add_argument('--job-order', type=int, choices=(1, 2, 3))
    s.add_argument('--order-us', type=int)
    s.add_argument('--inline-us', type=int)
    s.add_argument('--time-ms', type=int)
    s = sub.add_parser('sweep')
    s.add_argument('run')
    s.add_argument('--marker', required=True)
    s.add_argument('--steps', required=True)
    s.add_argument('--profile', action='store_true')
    s = sub.add_parser('steps')
    s.add_argument('run')
    s.add_argument('--marker', required=True)
    s.add_argument('--steps', required=True)
    s.add_argument('--repeat', type=int, default=1)
    s = sub.add_parser('sequence')
    s.add_argument('run')
    s.add_argument('--marker', required=True)
    s.add_argument('--baseline', type=int, default=60)
    s.add_argument('--profile', type=int, default=60)
    s = sub.add_parser('status')
    s.add_argument('run')
    s.add_argument('--last', type=int, default=12)
    s = sub.add_parser('seal')
    s.add_argument('run')
    a = p.parse_args()
    if a.action == 'prepare':
        print(json.dumps(prepare(a), indent=2))
    elif a.action == 'collect':
        print(json.dumps(collect(a)))
    elif a.action == 'control':
        print(json.dumps(control(a)))
    elif a.action == 'sequence':
        sequence(a)
    elif a.action == 'sweep':
        sweep(a)
    elif a.action == 'steps':
        steps(a)
    elif a.action == 'status':
        status(a)
    else:
        print(json.dumps(seal(run_folder(a.run))))


if __name__ == '__main__':
    sys.exit(main())
