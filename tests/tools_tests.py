"""Pure logic in the Python tools: the job round simulator (tools/job_sim.py).

  python -B tests/tools_tests.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from job_sim import Round  # noqa: E402

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

print('tools_tests: ok' if not failures else f'tools_tests: {failures} failure(s)')
sys.exit(1 if failures else 0)
