"""Hotspot report from the stack sampler's "profile" events.

  python -B tools/profile_report.py <run> [--pid N] [--marker M] [--toggles T]
         [--threads 4] [--top 30] [--folded out.txt] [--leaves 40] [--wc 30]

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

--leaves N lists the game's N hottest instructions (samples whose innermost
frame is that exact instruction, from v0.6 on), as function+offset and VA,
with the game thread's share: which loop of a function the time goes to.

--wc N (windows sampled with control.ini wc_scan=1, from v0.15.1) lists the
game instructions sampled most often with a register pointing into
write-combined memory (GPU upload heaps: every CPU read there bypasses the
caches, the effect loop's cost), with the registers, the share of that
instruction's samples that had such a pointer, and the call paths of the
functions involved. A hit is a candidate, not a finding: the disassembly
says whether the instruction reads that memory or only writes it.
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
REGISTERS = ('rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15')


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
        maps = sorted(run.glob('cpuopt.map'))
        self.own = MapSymbols(maps[0]) if maps else None
        self.game_names = {}
        names = REPO / 'local' / 'ida' / 'names.tsv'
        if names.exists():
            for line in names.read_text(encoding='utf-8').splitlines():
                rva, _, name = line.partition('\t')
                if name:
                    self.game_names[int(rva, 16)] = name

    def nearest(self, rva):
        """The named game function at or below `rva`, as name+0xoffset."""
        if not hasattr(self, 'starts'):
            self.starts = sorted(self.game_names)
        i = bisect.bisect_right(self.starts, rva) - 1
        if i < 0:
            return f'sub_{GAME_BASE + rva:X}'
        start = self.starts[i]
        return f'{self.game_names[start]}+{rva - start:#x}'

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
        if module.lower() == 'cpuopt.dll' and self.own:
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
    p.add_argument('--leaves', type=int, default=0, help="list the game's N hottest instructions")
    p.add_argument('--followed', action='store_true', help='only the follow mode\'s samples (flag 4), all threads as one')
    p.add_argument('--wc', type=int, default=0, help='the write-combined scan: the N most flagged game instructions')
    a = p.parse_args()
    run = run_folder(a.run)
    log = run / f'cpuopt-{a.pid}.jsonl' if a.pid else newest_log(run)
    events = rows(log)
    paths, thread_info, roles = {}, {}, {}
    for e in events:
        if e.get('event') == 'modules':
            for m in e['modules']:
                paths[Path(m[0]).name.lower()] = m[0]
        elif e.get('event') == 'window':
            for t in e['threads']:
                thread_info[t[0]] = (t[4], t[5])
            for role, label in (('update', 'GAME THREAD'), ('present', 'PRESENT THREAD')):
                if e.get(role, {}).get('thread'):
                    roles.setdefault(e[role]['thread'], label)  # the game thread may present too
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
        if a.followed:
            f = e.get('follow', {})
            failed['FOLLOWED'] += f.get('dropped', 0)
            for tid, count, flags, stack in e['stacks']:
                if flags & 4:
                    samples['FOLLOWED'] += count
                    per_thread['FOLLOWED'][tuple(frames[i] for i in stack)] += count
            continue
        for tid, n, bad in e['threads']:
            samples[tid] += n
            failed[tid] += bad
        for tid, count, _, stack in e['stacks']:
            per_thread[tid][tuple(frames[i] for i in stack)] += count
    print(f'{log.name}: {len(profiles)} profile windows, {ms / 1000:.1f} s, markers {sorted({e["marker"] for e in profiles})}, '
          f'toggles {sorted({e["toggles"] for e in profiles})}')
    if a.followed:
        jobs = sum(e.get('follow', {}).get('jobs', 0) for e in profiles)
        print(f'followed: {jobs} jobs, {samples["FOLLOWED"]} samples ({samples["FOLLOWED"] / max(jobs, 1):.1f} a job), '
              f'{failed["FOLLOWED"]} dropped (the job had ended), '
              f'{sum(e.get("follow", {}).get("missing", 0) for e in profiles)} times its thread was unknown')
        roles['FOLLOWED'] = 'FOLLOWED JOB'
    folded = []
    # The game and present threads first, then the most sampled.
    order = sorted(samples, key=lambda t: (t not in roles, roles.get(t, ""), -samples[t]))[:a.threads]
    for tid, total in ((t, samples[t]) for t in order):
        start, name = thread_info.get(tid, ('', ''))
        stacks = per_thread[tid]
        print(f'\n=== thread {tid} {roles.get(tid, "")} {name or ""} start {start}: {total} samples ({failed[tid]} failed)')
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
    if a.leaves:
        game_total = sum(samples[t] for t, r in roles.items() if r == 'GAME THREAD')
        every = sum(samples.values())
        leaf_all, leaf_game = collections.Counter(), collections.Counter()
        for e in profiles:
            for rva, game, total in e.get('leaves', []):
                leaf_all[rva] += total
                leaf_game[rva] += game
        print(f'\n=== hottest game instructions: {sum(leaf_all.values())} of {every} samples in the game exe '
              f'({sum(leaf_game.values())} of {game_total} on the game thread)')
        print('   all %   game %   samples  instruction')
        for rva, n in leaf_all.most_common(a.leaves):
            print(f'{100 * n / max(every, 1):7.2f}  {100 * leaf_game[rva] / max(game_total, 1):7.2f}  {n:8d}  '
                  f'{sym.nearest(rva)}  ({GAME_BASE + rva:#x})')
    if a.wc:
        wc_report(profiles, sym, a.wc)
    if a.folded:
        merged = collections.Counter()
        for line, n in folded:
            merged[line] += n
        a.folded.write_text(''.join(f'{line} {n}\n' for line, n in merged.items()), encoding='utf-8')
        print(f'\nfolded stacks: {a.folded}')


def wc_report(profiles, sym, top):
    scanned = [e for e in profiles if 'wc' in e]
    if not scanned:
        print('\n=== write-combined scan: not on in these windows (control.ini wc_scan=1, v0.15.1 on)')
        return
    total = sum(e['wc']['scanned'] for e in scanned)
    flagged = sum(e['wc']['samples'] for e in scanned)
    game = sum(e['wc']['game_thread'] for e in scanned)
    rebuilds = sorted(e['wc']['refresh_us'] for e in scanned)
    print(f'\n=== write-combined scan: {flagged} of {total} samples ({100 * flagged / max(total, 1):.2f}%) had a register '
          f'pointing into write-combined memory, {game} of them on the game thread; '
          f'{max(e["wc"]["regions"] for e in scanned)} regions, {max(e["wc"]["bytes"] for e in scanned) / 2**20:.0f} MB, '
          f'{sum(e["wc"]["dropped"] for e in scanned)} past the list; region list rebuilt in {rebuilds[len(rebuilds) // 2]} us '
          f'(worst {rebuilds[-1]})')
    modules = collections.Counter()
    for e in scanned:
        names = e['modules']
        for m, n in e['wc']['modules']:
            modules[names[m] if m != UNKNOWN and m < len(names) else '(unknown)'] += n
    print('-- module of the executing code')
    for name, n in modules.most_common(10):
        print(f'{100 * n / max(flagged, 1):6.1f}%  {n:7d}  {name}')
    wc, wc_game, regs, every = (collections.Counter() for _ in range(4))
    for e in scanned:
        for rva, n, g, mask, a in e.get('wc_leaves', []):
            wc[rva] += n
            wc_game[rva] += g
            regs[rva] |= mask
            every[rva] += a
    print("-- game instructions with such a pointer (of all: the share of that instruction's samples)")
    print('  flagged  game  of all  registers        instruction')
    for rva, n in wc.most_common(top):
        mask = regs[rva]
        names = ','.join(r for i, r in enumerate(REGISTERS) if mask >> i & 1)
        print(f'{n:9d} {wc_game[rva]:5d} {100 * n / max(every[rva], 1):6.0f}%  {names:15s}  {sym.nearest(rva)}  ({GAME_BASE + rva:#x})')
    # Call paths of the flagged samples, by the function executing.
    paths = collections.defaultdict(collections.Counter)
    for e in scanned:
        names = e['modules']
        frames = [(names[m] if m != UNKNOWN and m < len(names) else None, r) for m, r in e['frames']]
        for tid, count, flags, stack in e['stacks']:
            if flags & 8 and stack:
                chain = [frames[i] for i in stack]
                paths[sym.label(*chain[0])][' <- '.join(sym.label(*f) for f in chain[1:6])] += count
    print('-- call paths of the flagged samples (executing function, then up to five callers)')
    for function, counter in sorted(paths.items(), key=lambda kv: -sum(kv[1].values()))[:12]:
        print(f'{sum(counter.values()):7d}  {function}')
        for path, n in counter.most_common(2):
            print(f'           {n:6d}  <- {path}')


if __name__ == '__main__':
    main()
