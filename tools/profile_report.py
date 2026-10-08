"""Hotspot report from the stack sampler's "profile" events.

  python -B tools/profile_report.py <run> [--pid N] [--marker M] [--toggles T]
         [--threads 4] [--top 30] [--folded out.txt]

Per sampled thread (most samples first): where its time goes by module, the
functions it was executing (self), the functions on its stack (inclusive),
and which game function was innermost when the time went to another module
(the driver, the OS waits). Counts are wall-clock samples: a waiting thread
is sampled too, so a thread's shares add up to its whole wall time.

Symbols: game frames are function starts from the executable's exception
table, printed IDA-style (sub_1423922C0 = image base 0x140000000 + RVA), named
from local/ida/names.tsv ("rva<TAB>name") when it exists; the plugin from the
run's linker map; other DLLs by nearest export. Frames without an unwind entry
are instruction addresses ("@").
"""
import argparse
import bisect
import collections
import json
import re
from pathlib import Path

from run import newest_log, rows, run_folder

REPO = Path(__file__).resolve().parents[1]
RAW = 0x80000000
UNKNOWN = 0xFFFF
GAME = 'MonsterHunterWorld.exe'
GAME_BASE = 0x140000000


class MapSymbols:
    """Function starts from an MSVC linker map (publics and static symbols)."""

    def __init__(self, path):
        text = Path(path).read_text(encoding='utf-8', errors='replace')
        base = int(re.search(r'Preferred load address is ([0-9a-fA-F]+)', text).group(1), 16)
        found = {}
        for m in re.finditer(r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\s', text, re.M):
            rva = int(m.group(2), 16) - base
            if rva > 0:
                found.setdefault(rva, m.group(1))
        self.starts = sorted(found)
        self.names = [found[r] for r in self.starts]

    def name(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        return self.names[i] if i >= 0 else None


class ExportSymbols:
    """Nearest named export at or below an RVA, from a DLL's export table."""

    def __init__(self, path):
        data = Path(path).read_bytes()
        u16 = lambda o: int.from_bytes(data[o:o + 2], 'little')
        u32 = lambda o: int.from_bytes(data[o:o + 4], 'little')
        pe = u32(0x3c)
        sections = [(u32(s + 12), u32(s + 8), u32(s + 20)) for s in
                    (pe + 24 + u16(pe + 20) + 40 * i for i in range(u16(pe + 6)))]

        def offset(rva):
            for va, size, raw in sections:
                if va <= rva < va + max(size, 1):
                    return raw + rva - va
            raise ValueError(rva)
        found = {}
        export_rva = u32(pe + 24 + 112)
        if export_rva:
            e = offset(export_rva)
            functions, names, ordinals = offset(u32(e + 28)), offset(u32(e + 32)), offset(u32(e + 36))
            for i in range(u32(e + 24)):
                at = offset(u32(names + 4 * i))
                name = data[at:data.index(bytes(1), at)].decode('ascii', 'replace')
                found.setdefault(u32(functions + 4 * u16(ordinals + 2 * i)), name)
        self.starts = sorted(found)
        self.names = [found[r] for r in self.starts]

    def name(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i < 0:
            return None
        return self.names[i] + (f'+{rva - self.starts[i]:#x}' if rva != self.starts[i] else '')


class Symbolizer:
    def __init__(self, run, module_paths):
        self.paths = module_paths  # lower-case file name -> full path
        self.cache = {}
        maps = sorted(run.glob('mhw_cpuopt.map'))
        self.own = MapSymbols(maps[0]) if maps else None
        self.game_names = {}
        names = REPO / 'local' / 'ida' / 'names.tsv'
        if names.exists():
            for line in names.read_text(encoding='utf-8').splitlines():
                rva, _, name = line.partition('\t')
                if name:
                    self.game_names[int(rva, 16)] = name

    def resolver(self, module):
        key = module.lower()
        if key not in self.cache:
            path = self.paths.get(key)
            try:
                self.cache[key] = ExportSymbols(path) if path and key.endswith('.dll') else None
            except (OSError, ValueError, IndexError):
                self.cache[key] = None
        return self.cache[key]

    def label(self, module, rva):
        raw = bool(rva & RAW)
        rva &= ~RAW
        at = '@' if raw else ''
        if module is None:
            return '(unknown)'
        if module == GAME:
            name = self.game_names.get(rva)
            return f'game!{at}{name}' if name else f'game!{at}sub_{GAME_BASE + rva:X}'
        if module.lower() == 'mhw_cpuopt.dll' and self.own:
            return f'cpuopt!{self.own.name(rva) or hex(rva)}'
        r = self.resolver(module)
        sym = r.name(rva) if r else None
        return f'{module}!{at}{sym}' if sym else f'{module}+{at}{rva:#x}'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run')
    p.add_argument('--pid', type=int)
    p.add_argument('--marker')
    p.add_argument('--toggles', type=int)
    p.add_argument('--threads', type=int, default=4)
    p.add_argument('--top', type=int, default=30)
    p.add_argument('--folded', type=Path, help='write folded stacks (flame graph input) for the shown threads')
    a = p.parse_args()
    run = run_folder(a.run)
    log = run / f'cpuopt-{a.pid}.jsonl' if a.pid else newest_log(run)
    events = rows(log)
    paths, thread_info = {}, {}
    for e in events:
        if e.get('event') == 'modules':
            for m in e['modules']:
                paths[Path(m[0]).name.lower()] = m[0]
        elif e.get('event') == 'window':
            for t in e['threads']:
                thread_info[t[0]] = (t[4], t[5])
    profiles = [e for e in events if e.get('event') == 'profile' and (a.marker is None or e['marker'] == a.marker)
                and (a.toggles is None or e['toggles'] == a.toggles)]
    if not profiles:
        raise SystemExit('no matching profile events')
    sym = Symbolizer(run, paths)

    # Stacks per thread as tuples of (module name, rva), innermost first.
    per_thread = collections.defaultdict(collections.Counter)
    samples, failed = collections.Counter(), collections.Counter()
    ms = 0.0
    for e in profiles:
        ms += e['ms']
        names = e['modules']
        frames = [(names[m] if m != UNKNOWN and m < len(names) else None, r) for m, r in e['frames']]
        for tid, n, bad in e['threads']:
            samples[tid] += n
            failed[tid] += bad
        for tid, count, _, stack in e['stacks']:
            per_thread[tid][tuple(frames[i] for i in stack)] += count
    print(f'{log.name}: {len(profiles)} profile windows, {ms / 1000:.1f} s, markers {sorted({e["marker"] for e in profiles})}, '
          f'toggles {sorted({e["toggles"] for e in profiles})}')
    folded = []
    for tid, total in samples.most_common(a.threads):
        start, name = thread_info.get(tid, ('', ''))
        stacks = per_thread[tid]
        print(f'\n=== thread {tid} {name or ""} start {start}: {total} samples ({failed[tid]} failed)')
        by_module, self_time, inclusive, inner_game = (collections.Counter() for _ in range(4))
        for stack, n in stacks.items():
            leaf_module = stack[0][0] if stack else None
            by_module[leaf_module or '(unknown)'] += n
            self_time[sym.label(*stack[0])] += n
            for label in {sym.label(*f) for f in stack}:
                inclusive[label] += n
            game = next((f for f in stack if f[0] == GAME), None)
            inner_game[(sym.label(*game) if game else '(no game frame)', leaf_module or '(unknown)')] += n
            if a.folded:
                folded.append((f'{tid};' + ';'.join(sym.label(*f) for f in reversed(stack)), n))
        sections = (('module of the executing code', by_module.most_common(12)),
                    ('self (executing function)', self_time.most_common(a.top)),
                    ('inclusive (function on the stack)', inclusive.most_common(a.top)),
                    ('innermost game function -> module executing', [(f'{g}  ->  {m}', n) for (g, m), n in inner_game.most_common(a.top)]))
        for title, items in sections:
            print(f'-- {title}')
            for label, n in items:
                print(f'{100 * n / total:6.1f}%  {n:7d}  {label}')
    if a.folded:
        merged = collections.Counter()
        for line, n in folded:
            merged[line] += n
        a.folded.write_text(''.join(f'{line} {n}\n' for line, n in merged.items()), encoding='utf-8')
        print(f'\nfolded stacks: {a.folded}')


if __name__ == '__main__':
    main()
