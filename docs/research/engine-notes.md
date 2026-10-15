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

## v0.6 targets, from the al-05 profile and al-07's job timing (2026-10-15)

Measured (al-05: hub start, 60 s of sampling, 37,159 game-thread samples;
al-07: 23,617 frames of job timing). The fixes are switches in v0.6, not yet
measured in game.

### Steam's server time: 4.3% of the game thread

- Two game functions call `ISteamUtils::GetServerRealTime` (vtable slot 3 of
  the interface `SteamInternal_ContextInit` returns): `sub_142215230` (a
  session object's slot 0xC0; 2.7% of game-thread samples are under it) and
  `sub_141B008E0` (a date struct through `_gmtime64`; 1.6%). Both run several
  times a frame from the per-frame checks `sub_141976900/A60/D20/DE0`
  (under `sub_140257A00`).
- Inside `steamclient64.dll` every call takes a lock
  (`CThreadFullMutex::TryLock`); the samples sit there. All Steam frames
  together were 5.7% of the game thread; the rest is `SteamAPI_RunCallbacks`
  (0.4%) and P2P polling `sub_142207A70` (0.9%, left alone: online path).
- Switch 32 keeps one read for time_ms and adds the whole seconds since.
  The server time is a whole-second clock; a reused value lags by under a
  second. `sub_142215230`'s session state (slot 0x60, `sub_142215220`: a
  flag at +0x974C, 2 when set) is still asked on every call.

### sPrimitive's record loop: 6.0% of the game thread, the hottest game code outside the spin

- `sub_142570A40` had 5,793 samples in al-05 (2,173 on the game thread,
  6.0%); the next game function outside executeJob's spin had 359. It runs
  inside the camera round's effect draw jobs
  (`sub_141FAF590` → `sub_1422F7C20` → `sub_1425D0140` → `sub_14262DA80` →
  `sub_142630000`), the jobs that end camera rounds.
- Confirmed (machine code): its loop (0x2570C20) writes one 16-byte record
  per primitive into a ring buffer taken from sPrimitive (+0x138 the mapped
  pointer, +0x144 the atomic fill offset). The first two dwords are bit
  fields written with eight read-modify-write steps on memory; their final
  values do not depend on the old contents.
- Hypothesis: the ring is GPU upload memory (sPrimitive's constructor
  `sub_1424EE8E0` makes its buffers through `sub_14239E230` with usage 2), so
  write-combined, and every read there is uncached. v0.6 logs the ring's page
  protection once (`prim_buffer`, read-only).
- Switch 16 (al.patch, 56 bytes from 0x2570C1A): the loop's preheader
  completes both dwords once and the loop stores them; the same bytes are
  written. `plugin_tests` runs the game's loop and the replacement as
  machine code on 48 random inputs (1 to 4,096 records): same records, same
  live registers.
- Open: the game re-checks parts of its own code; whether this function is
  among them is not known. Watch for crashes or odd behaviour minutes after
  the switch goes on.

### Rounds with little work (al-07)

- executeJob wakes min(jobs, participants) - 1 threads and spins until all
  have reported back. Four rounds a frame held 11-20 us of work and took
  21-31 us: `sub_1422E248F` (314 jobs, 19.5 us of work, 31.4 us),
  `sub_1426D9328` (62 jobs, 13.0 us, 27.6 us), `sub_1411DA84C` (13 jobs,
  11.0 us, 21.1 us), `sub_1423D29E1` (5 jobs, 17.1 us, 22.5 us).
- Switch 64 runs a call site's rounds with participants 1 (executeJob's own
  single-thread path) while the site's mean work is under inline_us (25),
  learned from timed rounds. Expected gain about 50 us a frame (1.3%) if
  inline work matches the measured work; jobs that ran on many threads may
  run faster or slower on one.
- The unit move round (`sub_14221B2CA`, 6.3 a frame) does not gain from
  ordering: the replay's oracle saves 5.8 us a round. Its long jobs
  (`uPlayer` 161 us, `uNpcVillage` 95 us) are the floor.

### Hottest instructions (v0.6)

The sampler now also counts the exact innermost instruction in the game exe
(`profile_report.py --leaves N`), so the next profile can show which loop of
a function (`sub_142570A40`, the effect jobs) the time goes to.

### v0.6 in game (al-09, al-10; 2026-10-15)

**al-09:** the first v0.6 launch froze on a black screen right after the
title (game_ready at 15.0 s; no frames from about 16 s), with no switch on.
The game thread waited in `cD3DRender::present12` on the frame fence; the
GPU was idle (10%, 77 W), all threads were waiting, and there was no TDR.
The only module differing from al-08 was cpuopt.dll, which at that point had
done only read-only work (`local/runs/al-09/notes.md`). No cause found; the
relaunch (al-10) and its clean exit did not repeat it.

**al-10:** hub start, stock scheduling; 30 s steps, each new switch on top of
order_jobs (8), the list run twice (the first window of each step left out):

| Step | fps | mean frame | process CPU per frame |
|---|---|---|---|
| 8 (baseline), eight steps | 258.2-263.4 (260.9 pooled) | 3.832 ms | 16.8-17.2 ms |
| 24 (+ plain_prim_stores) | 287.7, 287.0 | 3.476, 3.485 ms | 13.6 ms |
| 40 (+ reuse_server_time) | 267.4, 270.2 | 3.740, 3.702 ms | 16.9 ms |
| 72 (+ inline_rounds) | 264.1, 263.6 | 3.786, 3.793 ms | 16.5-16.8 ms |
| 120 (all) | 287.7, 285.6 | 3.476, 3.502 ms | 13.7-13.9 ms |

- **plain_prim_stores: +10.1%** (3.83 to 3.48 ms a frame), every step above
  every baseline step; the process's CPU time per frame fell by 3.4 ms
  (-20%). The ring is write-combined (`prim_buffer`: protect 0x404,
  MEM_PRIVATE), as supposed. Profile (toggles 9 against 25): the loop's
  read-modify-write instructions were the hottest game instructions after the
  spin (each ~0.75% of the game thread); with the change, samples under the
  effect draw job (`sub_141FAF590`) fell from 3,653 to 361 and those in
  `sub_142570A40` from 3,063 to 9; the game thread's spin fell from 35.8% to
  18.8% of its samples. Applied and removed four times while the game ran;
  the game exited cleanly with it on.
- **reuse_server_time: +3.0%**, both steps above every baseline step; 48
  reads answered by Steam against 80,088 reused.
- **inline_rounds: +1.2%**, inside the baseline's spread: not shown. Six of
  nine sites ran alone (about 4 rounds a frame).
- **All together** equals plain_prim_stores alone: with it on, the median
  frame is 3.40 ms against the engine's 300 fps cap (3.33 ms), and the game
  thread spends 9.3% waiting for the frame fence of two frames back and 3.1%
  in the frame limiter's QPC loop (`sub_14225F9F0+0x416`). The hub cannot
  show further CPU gains; heavier scenes (fights, many effects) can.

### Candidates after v0.6 (not yet changed)

- **Waking the job threads** (`NtReleaseSemaphore`, 5.8% of the game thread
  with the prim fix on). A stand-alone test on the test PC (30 threads asleep
  on a semaphore, 400 rounds each): releasing 30 costs the caller 70 us
  (p50), 15 cost 40 us, 7 cost 19 us, 1 cost 6 us; a fan-out (release 3,
  each woken thread releases 3 more) costs the caller 10 us and wakes all 30
  in the same 67 us. In the game, threads start their jobs while the call is
  still waking the rest, so the call may be off the critical path; H2's null
  result points the same way.
- **The render threads' descriptor cache** (`sub_1423A9200` →
  `sub_142624870`): each lookup hashes the descriptors with a byte-wise
  CRC32 (table at 0x34FFDA0) and scans an append-only array under a shared
  SRW lock; the 8 `sRender::renderingSub` threads contend on that lock (44%
  of the samples under `sub_1423A9200` are in `RtlAcquireSRWLockShared`,
  12% in the release). About 10% of the render threads' busy time at the
  hub, where they are 88% idle.
- **Per-unit timers**: `sub_142C76350`/`sub_142C764F0` call QPC in every
  unit move (4% of the samples under `cUnit::genericMove`, roughly 10 us a
  frame).

### Heavier scenes (al-11, v0.6.0; 2026-10-15)

Every switch of v0.6 on (120) against every switch off (0), 30 s steps,
four pairs each (first window of each step left out):

| Scene | off: fps, mean frame, CPU/frame | on: fps, mean frame, CPU/frame |
|---|---|---|
| title screen (the slow variant) | 114.0-116.4, 8.71 ms, 18.6 ms | 295.1-297.9, 3.38 ms, 6.1 ms |
| Ancient Forest, standing at a camp | 154.1-161.5, 6.32 ms, 30.2 ms | 212.3-221.7, 4.61 ms, 23.5 ms |

- **Title screen: 2.6x.** plain_prim_stores alone does it (0 against 16, two
  pairs: 115.0/115.1 against 295.3/294.8 fps, CPU 18.6 to 6.2 ms a frame);
  with it on, the title reaches the 300 fps cap. Switched off, the game
  thread spends 78% of its samples in executeJob's spin while the job
  threads run the record loop: the slow title screen is that loop.
- **Ancient Forest: +37%** (6.32 to 4.61 ms a frame), every on step above
  every off step; the scene stays under the cap.
- Job timing in the forest with everything on: the unit move round runs
  10.4 times a frame (115 us each; its floor is the longest job, uPlayer's
  move at 222 us once a frame), the camera round 4 times (181 us each,
  bound 91 us); only 14.2 of 31 participants take a job in either. The
  "gain" (round minus its bound) is 362 us a frame in the camera round and
  254 us in the unit round. Most job threads wake after the round's jobs are
  taken: the target of v0.7's spin_workers.
