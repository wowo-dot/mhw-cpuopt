"""Read-only dump of sMhUnit's move lines in the running game.

  python -B tools/unit_lines.py <run> [--pid N] [--top 6]

Opens the game with PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ only
(no writes, no suspension) and, for each of sMhUnit's 32 move lines, prints
its flags (P = may run on the job threads, off = skipped), last frame's
times of the move and second passes (us, as the passes measured them), and
the classes of the units in it, from local/ida/vtables.tsv.

Layout (tested exe c2ebbbd2...; engine-notes.md): sMain global at RVA
0x51C4148, sMhUnit at sMain +0xAF8D0, lines from sMhUnit +0x38, 248 bytes
each; relative to sMhUnit + 248 * line: flags +0x98, unit list head +0x80,
times +0xA8 and +0xAC; units linked through +0x30.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import ntpath
import struct
from pathlib import Path

from run import newest_log, rows, run_folder

REPO = Path(__file__).resolve().parents[1]
SMAIN_RVA, UNIT_OFFSET, LINES, STRIDE = 0x51C4148, 0xAF8D0, 32, 0xF8
PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_VM_READ = 0x1000, 0x0010

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class Reader:
    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            raise SystemExit(f'cannot open pid {pid} for reading ({ctypes.get_last_error()})')
        # The pid may have been reused: read only the game.
        name = ctypes.create_unicode_buffer(1024)
        size = wt.DWORD(len(name))
        if not k32.QueryFullProcessImageNameW(self.h, 0, name, ctypes.byref(size)) or                 ntpath.basename(name.value).lower() != 'monsterhunterworld.exe':
            k32.CloseHandle(self.h)
            raise SystemExit(f'pid {pid} is not MonsterHunterWorld.exe ({name.value or "unknown"}); is the game running?')

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return buf.raw

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack('<Q', b)[0] if b else None

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if b else None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run')
    p.add_argument('--pid', type=int)
    p.add_argument('--top', type=int, default=6)
    a = p.parse_args()
    events = rows(newest_log(run_folder(a.run)))
    start = next(e for e in events if e.get('event') == 'start')
    pid = a.pid or start['pid']
    base = None
    for e in events:
        if e.get('event') == 'modules':
            for m in e['modules']:
                if ntpath.basename(m[0]).lower() == 'monsterhunterworld.exe':
                    base = int(m[1], 16)
    names = {}
    tsv = REPO / 'local' / 'ida' / 'vtables.tsv'
    if tsv.exists():
        for line in tsv.read_text(encoding='utf-8').splitlines():
            rva, _, name = line.partition('\t')
            names[int(rva, 16)] = name.replace('::vft', '')
    r = Reader(pid)
    smain = r.u64(base + SMAIN_RVA)
    unit = r.u64(smain + UNIT_OFFSET) if smain else None
    if not unit:
        raise SystemExit('no sMhUnit yet')
    threshold = r.u32(unit + 0x4A58)
    print(f'pid {pid}: sMhUnit {unit:#x}, threshold {threshold} us')
    print(' line flags      units  move us  step us  classes (count)')
    for i in range(LINES):
        at = unit + STRIDE * i
        flags = r.u32(at + 0x98)
        head = r.u64(at + 0x80)
        move_us, step_us = r.u32(at + 0xA8), r.u32(at + 0xAC)
        classes = collections.Counter()
        n, u, seen = 0, head, set()
        while u and u not in seen and n < 20000:
            seen.add(u)
            vt = r.u64(u)
            classes[names.get(vt - base, f'vt+{vt - base:#x}') if vt else '?'] += 1
            n += 1
            u = r.u64(u + 0x30)
        if not n and not (flags & 1):
            continue
        tag = ('P' if flags & 1 else '-') + (' off' if flags & 8 else '')
        top = ', '.join(f'{c} {k}' for c, k in classes.most_common(a.top))
        more = f' +{len(classes) - a.top} more' if len(classes) > a.top else ''
        fmt = lambda v: f'{v:7d}' if v is not None and v != 0xFFFFFFFF else '      -'
        print(f'{i:5d} {tag:9s} {n:6d}  {fmt(move_us)}  {fmt(step_us)}  {top}{more}')


if __name__ == '__main__':
    main()
