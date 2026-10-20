"""Pure logic in the Python tools: the job round simulator (tools/job_sim.py)
and the packager's import check (tools/package.py).

  python -B tests/tools_tests.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from job_sim import Round  # noqa: E402
from package import WINE_ABSENT, imported_names  # noqa: E402

failures = 0


def check(condition, what):
    global failures
    if not condition:
        print(f'FAIL: {what}')
        failures += 1


# Three jobs on two threads: job 0 takes 300 on thread 0 (ready at 10); jobs 1
# (100) and 2 (680) run on thread 1 (ready at 20), 10 cycles apart; the round
# returns 150 after its last job ends.
r = Round({'caller_rva': 1, 'duration': 960, 'jobs': [[0, 1, 10, 300, 0], [0, 1, 20, 100, 1], [0, 1, 130, 680, 1]]})
check(r.ready == [10, 20] and r.gap == 10 and r.tail == 150 and r.last_end == 810, 'round read from the trace')
check(r.simulate([0, 1, 2]) == 960, 'the original order replays the measured round')
check(r.simulate([2, 0, 1]) == 840, 'longest first: job 2 starts at 10 and ends at 690')
check(r.simulate([1, 0, 2]) == 950, 'job 2 third: the thread that ran job 1 (free at 120) takes it')

# A job that never ran (duration 0) does not set a thread's ready time.
q = Round({'caller_rva': 1, 'duration': 100, 'jobs': [[0, 1, 0, 0, 3], [0, 1, 5, 50, 0]]})
check(q.ready == [5] and q.simulate([0, 1]) == q.simulate([1, 0]), 'jobs that never ran')

# No job ran: the measured duration stands.
check(Round({'caller_rva': 1, 'duration': 77, 'jobs': [[0, 1, 0, 0, 0]]}).simulate([0]) == 77, 'nothing ran')


# The packager's import check, on a minimal 64-bit PE: one section (RVA 0x1000,
# file 0x200) with one import descriptor for KERNEL32.dll: two functions by
# name and one by ordinal.
def tiny_pe(with_imports=True):
    import struct
    d = bytearray(0x400)
    struct.pack_into('<I', d, 0x3C, 0x40)
    d[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HH12xHH', d, 0x44, 0x8664, 1, 0xF0, 0x2022)  # file header
    struct.pack_into('<H', d, 0x58, 0x20B)                           # optional header: PE32+
    if with_imports:
        struct.pack_into('<II', d, 0x58 + 120, 0x1000, 40)           # import directory
    struct.pack_into('<8sIIII', d, 0x58 + 0xF0, b'.idata', 0x200, 0x1000, 0x200, 0x200)
    def put(rva, fmt, *v):
        struct.pack_into(fmt, d, rva - 0x1000 + 0x200, *v)
    put(0x1000, '<5I', 0x1040, 0, 0, 0x1030, 0x1080)  # descriptor; the next one is zero
    put(0x1030, '13s', b'KERNEL32.dll')
    for table in (0x1040, 0x1080):
        put(table, '<4Q', 0x10C0, 0x10E0, (1 << 63) | 5, 0)
    put(0x10C0, '<H25s', 0, b'GetProcessDefaultCpuSets')
    put(0x10E0, '<H13s', 0, b'CreateThread')
    return bytes(d)


names = imported_names(tiny_pe())
check(names == {'GetProcessDefaultCpuSets', 'CreateThread'}, 'imports by name; the ordinal one left out')
check(names & WINE_ABSENT == {'GetProcessDefaultCpuSets'}, 'a function Wine lacks is caught')
check(imported_names(tiny_pe(with_imports=False)) == set(), 'no import directory')

print('tools_tests: ok' if not failures else f'tools_tests: {failures} failure(s)')
sys.exit(1 if failures else 0)
