# Engine notes for CPU work

Executable SHA-256 `c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`
(PE timestamp `0x66D13CB6`), checked 2026-10-08 against the game folder.

## Confirmed (static, decoded image)

First found on the old decoded image (`ai\glavenus`, SHA-256 `d7861173…884c`),
which was dumped with mods loaded and differs from the original code in 29
places. Re-checked on 2026-10-11 on the clean image (SHA-256
`a78ea69b…1d80`, CMFM `docs/handoff-clean-image-20261011.md`): the three
code sites below have the same bytes on both images, their functions
(0x239223A, 0x2293280, 0x2290880) are byte-identical on both, and the
nearest of the 29 places is 0xC75E bytes from the signature. Nothing here
depended on modified bytes.

- Sections: `.text` 0x1000 (0x2E1102C bytes, encrypted on disk and decrypted
  at start by the Steam stub in `.bind`), `.rdata`, `.data` 0x3E78000,
  `.pdata` 0x5216000 (0x34A7C0 bytes: about 287,000 unwind entries, so most
  functions have boundaries for the sampler).
- Frame counter: the signature
  `48 8B 05 ?? ?? ?? ?? 49 B9 AB AA AA AA AA AA AA AA 4C 8B 80 ?? ?? ?? ??`
  (renderer pointer load, the multiply-by-1/3 constant for frame % 3, the
  counter load) matches once in `.text`, at 0x23922C2; its operands give the
  renderer singleton at 0x51C4480 and the counter at +0x1F210 (the DLSS
  project's values).
- The counter is incremented in one place: `inc qword [rbx+1F210h]` at
  0x22932DB, and written at 0x2290E78 (`mov [rsi+1F210h], rdi`), which is in
  `sRender::ctor` (Fexty's name, 0x2290880): the singleton at 0x51C4480 is
  the engine's `sRender`. The function containing 0x22932DB (0x2293280,
  unnamed, one caller 0x1AF2B60) runs once per frame; which thread runs it
  is not yet observed.

## Game configuration on the test PC (read only)

- `graphics_option.ini`: windowed 2560x1440, no frame limit, V-Sync off,
  manual preset, ResolutionScaling High, LODBias High, MaxLODLevel No Limit,
  volume rendering Highest, DLSS off, AA off.
- `config.ini` `[CPU]`: `NumCPUCore=32`, `NumProcessAffinity=0`,
  `RenderingThread=ON`. The user restricts the game to CCD0 (16 logical
  processors) with Process Lasso, so the game may size thread pools for 32
  logical processors while running on 16.

## Hypotheses to test (not results)

- H1: the render thread's submission is the limit (handoff: hub render
  submission 2.6-3.2 ms of a 4.8-5.3 ms frame with the DLSS mod). Test: render
  thread cycles per frame against frame time, vanilla.
- H2: worker pools sized for 32 logical processors on 16 cause extra context
  switching. Test: thread count and switches per frame; A/B
  `NumCPUCore=16` (a user-side setting, no code).
- H3: a few functions dominate the render thread. Test: sampler, hub and
  training grounds, with and without the DLSS mod.

## The job system and the main thread's wait (2026-10-15)

### Measured (runs al-01, al-02; hub start, standing still; Astera Loader 0.2.3, cpuopt v0.2.0)

| Run | Scheduling | fps | p50 / p95 / p99 ms | Game thread busy | Self time in the executeJob spin |
|---|---|---|---|---|---|
| al-01 | Process Lasso → cache CCD (affinity 0xffff); a third-party DLL hooking timing functions was injected | 253.3 | 3.75 / 5.60 / 6.40 | 94.1% | 41.2% of 37,285 samples |
| al-02 | stock: BIOS CPPC Auto, Game Bar + AMD driver (frequency CCD parked, 0% busy); nothing injected | 258.5 | 3.85 / 4.40 / 4.70 | 93.5% | 39.3% of 37,436 samples |

- The game thread (start `0x22E4AE0`) runs Main:Update and calls Present
  itself: al.frame's game thread and al.d3d12's present thread are the same
  thread. Update (BEGIN to END) takes 99.6% of each frame interval.
- Time inside Present: 0.22-0.23 ms mean. The GPU is not the limit.
- The process ran 158-165 threads; 30 job threads (start `0x225F2B0`) each
  used about 0.37-0.40 ms of CPU per frame with about 14 context switches per
  frame; process CPU 16.9-17.6 ms per frame.
- The two runs differ in more than scheduling (the injected timing DLL), so they do not
  rank the two scheduling setups.

### Confirmed (decompiler, clean database)

- `sMain::executeJob` (0x225E840, Fexty's name) is the fork-join step. It
  releases `participants - 1` waiters on one semaphore (`[+9F1F0h]`), runs
  jobs itself (`sub_14225F620`), then spins (`pause`, 0x225EAD1) until the
  done counter `[+9F1FCh]` equals the number released.
- Jobs come from one shared counter (`lock inc [sMain+0C0h]`, compared with
  the job count `[+0B0h]`): every participant takes the next job until none
  are left. The worker loop (`sub_14225F2F0`) waits on the semaphore, runs
  jobs the same way, then increments the done counter, also when it found no
  job. So the main thread waits for every released thread to wake.
- Participants: `sub_14225BE20` sets `[+0D0h]` = min(dwNumberOfProcessors from
  GetSystemInfo, 32), minus one from 4 on; 31 on a 32-thread CPU, whatever
  the process affinity. `sub_14225F9F0` copies `[+0D0h]` into `[+0CCh]`
  every frame. Only `sub_14225BE20` writes `[+0D0h]`.
- config.ini `NumCPUCore` does not change this: it is stored at
  `[+0AF5F4h]` and only used by the `NumProcessAffinity` setter
  (`sub_1422602D0`), which may call SetProcessAffinityMask.

### Hypothesis under test

- H2 (refined): waking 30 threads per job round, most of which find little
  or no work, makes the main thread wait for the slowest wake-up. cpuopt
  v0.3's `job_thread_cap` switch (toggle 2, `job_threads=N`) sets `[+0D0h]`
  and `[+0CCh]` to N on the game thread at al.frame BEGIN; measured on and off
  in one session.

### The job rounds the game thread waits on (al-02, tools/job_report.py)

Game thread, hub start, 37,436 samples over 60 s (stock scheduling):

| Job round started by | Share of the game thread | Spinning | Running jobs itself | Called from |
|---|---|---|---|---|
| `sub_14221A050` | 39.5% | 26.9% | 11.6% | `sCamera::draw` ← `sMhRender::move` ← the frame (`sub_141AF2B60`) |
| `sub_14221ACE0` | 20.4% | 10.1% | 8.4% | `sub_14221B360` ← the frame (`sub_141AF2B60`) |
| seven others | 4.7% | | | |

- Jobs the game thread runs itself: `cUnit::genericMove` 8.3% (the
  `sub_14221ACE0` round), `sub_141FAF590` 6.9% and smaller ones (the
  `sub_14221A050` round).
- The 30 job threads waited for work 92.4% of their samples; when busy they
  ran the same two job functions.
- `sub_14221A050` builds its rounds two ways: a prepared job list with a
  125,680-byte buffer per thread slot (32 slots), and one job per object of a
  linked object list (type and flag filter) running `sub_142228DE0`.
- Job thread cap sweep (al-03, 31/15/11/7 threads per round, 25-125 s each,
  interleaved with native): 256.5 / 259.8 / 255.3 / 246.9 fps. Fewer
  threads per round did not shorten the rounds, and 7 lengthened them: the
  game thread waits for job work, not for thread wake-ups. H2 (refined) is
  rejected as the main cost; process CPU fell from 17.0 to 16.3 ms per frame
  at 15.
- Next (v0.3.1): the sampler reads sMain's job slots whenever it catches the
  game thread in the spin (`src/plugin/spin_probe.hpp`), to see whether one
  long job (load imbalance) or many jobs hold each round.
- al-04 (v0.3.1): the slot reading reported 30-31 "busy" slots in 94% of
  spin samples, impossible with job threads idle 92% of the time. Each slot
  keeps its last job after the thread finishes (the runner's loop can end on
  its condition without clearing it), so slot contents do not show running
  jobs. v0.3.2 reads the round's own counters instead: released (the spinning
  thread's edi), started `[+9F1F8h]` and done `[+9F1FCh]`, and marks the job
  threads' samples taken in the same sampler tick as a spin.
- al-04 repeated al-02's split closely: executeJob 64.2% of the game thread
  (spin 37.2%, own jobs 21.6%); `sub_14221A050` 39.3%, `sub_14221ACE0` 20.2%.
  `sub_14221ACE0` is the unit move pass (`sUnit`): one job per unit of a move
  line, each `cUnit::genericMove(unit)`, timed per line by the game's own
  MtPerformanceCounter.

### The spin, read from the round's counters (al-05, v0.3.2)

14,461 spin samples, hub start (the game thread's time: executeJob 63.5%, of
which spin 39.0%, own jobs 20.2%):

| Round from | Share of spins | Released | Not started (mean; samples with any) | Still running (mean; exactly one; two or more) |
|---|---|---|---|---|
| `sub_14221A050` (camera draw) | 68.6% | 30.0 | 0.06; 1.1% | 1.38; 58.8%; 32.6% |
| `sub_14221ACE0` (unit move) | 25.1% | 29.4 | 0.69; 7.8% | 0.69; 53.4%; 6.4% |

- Wake-up is not the cost: the threads of a round have nearly all started
  when the game thread spins. It waits for one or two threads still running
  a job while the other 28-29 are idle (job threads idle 94.9% in the ticks
  the game thread spun).
- The camera draw round's straggler job is mostly `sub_141FAF590`, a
  virtual per-object draw (flag check, a visibility test `sub_1422E1F60`, then
  `sub_1422F7C20` records the object's draws into the thread's buffer). The
  round runs one job per object.
- Hypothesis H5: the rounds end on a few expensive objects whose jobs start
  late in the list; ordering the list so expensive jobs start first (longest
  first) would shorten the rounds. Test: v0.4 times every job of every round
  (`src/plugin/job_timing.hpp`, toggle 4).

### Job timing and replay (al-06, al-07; v0.4.0, v0.4.1)

- Job timing costs about 3% while on (al-06: 260.6 to 252.7 fps; p99 4.62
  to 4.80 ms); the executeJob detour ran without trouble in game.
- Per round type (al-06, 16,503 frames): the camera draw round
  (`sub_14221A050`'s prepared-list call at 0x221A39D, 3.7 per frame, about
  470 jobs) took 383 us against a bound of 295 us; its last job sat on
  average 94% of the way down the list, in the last tenth in 95% of rounds.
  The unit move round (`sub_14221ACE0`, 6.1 per frame, about 140 jobs) took
  112 us against 83 us; its last job sat at 36%.
- Costliest job types per frame: effects (`uMhEffect` 1.9 ms, 31 jobs, 60 us
  each, peaks of 2.8 ms; `uBindMhEffect` 1.65 ms), model instances
  (`uBindBaseModelInstance` 1.1 ms, 748 jobs); in the move pass the player
  (`uPlayer`, 144-161 us per update) and village NPCs (`uNpcVillage`, 94 us).
- tools/job_sim.py replays traced rounds with greedy list scheduling. On 749
  traced rounds (al-07) replaying the game's order matched the measured
  rounds within 0.1 us on average. Predicted for the camera round (420.4 us
  measured): longest first by the round's own durations 348.9 us; longest
  first by job-type mean 352.6 us; job types of 20 us or more moved to the
  front 357.1 us; the list reversed 353.2 us: 239-270 us per frame back.
  The unit move round gains 13-37 us per frame at most; reversing it loses
  57 us. Other rounds: under 3 us per frame.
- H5 (expensive camera jobs start late) is supported by the replay; v0.5's
  switch order_jobs (toggle 8) tests it in game: job_order 1 reverses the
  camera round's list, job_order 2 learns each class's job time from one
  camera round in 16 and moves classes at or over order_us (20 us) to the
  front.

### The first fix: camera round order (al-08, v0.5.0)

Hub start, stock scheduling, nine interleaved 30 s steps (the first window
of each step left out):

| Step | fps | mean frame |
|---|---|---|
| game's order | 244.7 | 4.087 ms |
| reversed | 255.5 | 3.914 ms |
| game's order | 245.9 | 4.067 ms |
| learned | 254.6 | 3.928 ms |
| game's order | 244.1 | 4.097 ms |
| reversed | 256.8 | 3.894 ms |
| game's order | 244.5 | 4.090 ms |
| learned | 257.9 | 3.878 ms |
| game's order | 243.6 | 4.105 ms |

- Every reordered step beat every step in the game's order: 244.5 to 256.2
  fps over all windows (+4.8%), mean frame 4.089 to 3.903 ms (-0.19 ms), p95
  4.75 to 4.53-4.55 ms, p99 5.25 to 4.95-5.03 ms. Both modes the same here.
- The replay (al-07) predicted 0.24-0.26 ms; the difference is the switch's
  own cost (the list copied out and back; learned mode also times one round
  in 16).
- About 102,000 camera rounds reordered and restored; the user saw no visual
  change. H5 confirmed for the hub.
