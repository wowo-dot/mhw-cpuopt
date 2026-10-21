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

### Spinning job threads: slower (al-13, v0.7.1)

spin_workers (128) on top of v0.6's switches, Ancient Forest at a camp,
30 s steps, each window pair against 120 next to it:

| spin_us | with spinning | 120 beside it | CPU per frame (with) |
|---|---|---|---|
| 300 | 174.6 fps | 222.0, 217.4 fps | 104.0 ms |
| 100 | 205.0, 202.6 | 217.1, 215.1 | 70 ms |
| 50 | 213.1, 208.0 | 221.4, 217.4 | 58 ms |
| 20 | 205.8, 204.4 | 219.8, 218.7 | 45-46 ms |

The change worked as built (at 300 us, about 474 permits a frame were taken
by spinning threads and 134 waits blocked; no round hung), and every setting
was slower: -4% to -21%. Spinning threads cost far more CPU (23 ms a frame
without, 45-104 ms with) than the late wake costs the rounds. Hypothesis
(late-waking threads hold rounds back) rejected for this machine; the
switch stays as research, off. v0.7.0's first try (al-12) never applied:
the jump stub was made when the switch went on, when no page within 2 GB of
the exe was free any more; v0.7.1 makes it at PRE_MAIN (al-13: 2,047 MB
below the wait site).

### The game thread outside the job rounds (al-11, Ancient Forest, v0.6 on)

51% of the game thread's samples are outside executeJob:

- Present (cD3DRender::present) 8.9%, cD3DRender::endFrame 3.9%;
- sMhUnit's passes walking lines alone: the move pass (sub_14221ACE0)
  8.4%, the second pass (sub_14221B400, its serial loop at +0x1AF) 5.0%, the
  third (sub_14221C120) 2.1%. A line flagged parallel goes to the job
  threads only if it took more than sMhUnit +0x4A58 last frame, which the
  constructor sets to 60 us (v0.7.2's unit_threshold sets it);
- sCamera::draw outside its rounds 5.4%; collision (sCollision::Collider::move)
  4.3%;
- the session poll (sub_1422ADAE0) 3.8%, in which
  SteamAPI_IsSteamRunning's registry work (open, query, close through MO2's
  usvfs NtClose hook) is about 1.5% (v0.7.2's cache_steam_running); the
  Steam overlay's ClipCursor hook 0.4%; our own detour's VirtualQuery on
  timed rounds 0.3%.

### unit_threshold and cache_steam_running (al-15, v0.7.3)

Ancient Forest at a camp, v0.6's switches on in every step (120), each
variant a 30 s step between two 120 steps (the first window of each left
out). The 120 steps drifted from 237 to 217 fps over the session (in-game
time of day), so each variant is compared with its two neighbours.

| Variant | first pass | second pass | third / fourth |
|---|---|---|---|
| cache_steam_running (632) | -0.4% | +0.8% | |
| unit_us 1 | -6.7% | -7.4% | |
| unit_us 20 | -0.9% | -1.1% | |
| unit_us 30 | +1.8% | +0.7% | |
| **unit_us 40** | **+2.8%** | **+3.3%** | **+2.8%, +2.8%** |
| unit_us 50 | +2.8% | +2.6% | |
| unit_us 120 | -10.2% | -11.7% | |

- **unit_threshold: +2.8% at 40 us** (four of four), the same at 50;
  lower costs more in round overhead than it moves off the game thread,
  higher keeps more on it. The game's 60 us is not the best on this CPU.
  With 40 us the unit move round runs 12.6 times a frame (10.4 at 60) and
  the third pass's round (sub_14221B5A8) 5.9 times (2.4).
- **cache_steam_running: no measurable gain.** It worked (about 3,400
  answers reused for every 5 Steam calls in 5 s); the registry work it
  saves is smaller than the profile's 1.5% suggested, or hidden in noise.
- Next, from job timing with the best set (376) on: the camera round still
  loses 95 us a round (379 us a frame) against its bound; its work is
  SpeedTree draws (uBindSpeedTree, 294 a frame, 1.8 ms of work), base model
  instances (733 a frame, 1.6 ms) and effects (0.8 ms). No single hot spot
  stands out in the profile any more.

### Smaller CPUs, the cache die, and where the rounds wait (al-17, al-19; v0.8)

- **smaller_cpu (al-17, Ancient Forest):** the best set (376) against no
  switch, four pairs each: six cores (12 logical, CCD0) 176.4 to 236.9 fps
  (+34%), four cores (8) 160.2 to 212.8 (+33%). Even on 12 logical
  processors the game thread is the limit (95% busy; the 12 processors
  35-74% busy).
- **cache_ccd (al-17, al-19):** the same session, all switches of the best
  set on: CCD0's 16 (the 96 MB L3) against all 32 logical processors: al-17
  241.4 against 230.2 fps (three rounds), al-19 +4.1%, +3.8%, +3.0%, +3.4%
  (four pairs). Process CPU per frame 21.2 ms against 24.8.
- **Stock against everything (al-19, Ancient Forest camp):** 181.8 and
  180.0 fps against 245.2 and 242.6 (+35%); mean frame 5.53 to 4.10 ms.
- **What the rounds wait for (al-19 job timing, everything on):** the
  player's move (uPlayer, cUnit::genericMove) takes 192 us once a frame and
  ended its unit round in 10,494 of about 10,500 frames; monsters 104-121 us
  (uEm100, uEm109, uEm100_01). The unit pass runs each line as its own round
  (11.7 a frame), so the player's line holds the game thread about 190 us
  while the other job threads are idle.
- **Job-thread focus (sample_jobs 16, 60 s):** about 1,000 busy job-thread
  samples: unit moves 39% (uMhEffect::move 11%, the character update
  sub_141FBCCD0 12%), SpeedTree draws 12%, model draws 9%, effect draws 7.5%.
  On this CPU the job threads are idle about 88% of the time.

### Fights and the unit lines (al-20, v0.8.2; 2026-10-16)

Solo, the user fighting normally; everything off (0) against everything on
(2424: v0.6's switches, unit_threshold 40, cache_ccd), 15 s steps, eight
pairs, 3 s windows, the first window of each step left out.

| Fight | off | on | gain | on steps vs off steps |
|---|---|---|---|---|
| Arch-Tempered Velkhana | 158.5 fps, p99 8.12 ms | 233.1 fps, p99 5.97 ms | +47.1% | 222-241 against 152-167 |
| Furious Rajang | 204.3 | 267.4 | +30.9% | 252-281 against 198-209 |
| Fatalis (small arena) | 233.9 | 247.6 | +5.9% | overlapping |
| Alatreon | 268.8 | 279.7 | +4.1% | near the 300 fps cap |

- **Fatalis's nova:** a 6-minute watch (45 s blocks off and on, 1 s
  windows, sampler on) caught one slow stretch with everything off: 58-62
  fps for about five seconds, process CPU 46-48 ms a frame (16 normally).
  In those seconds sub_142570A40, the prim record loop plain_prim_stores
  fixes, held 35.3% of all busy samples (5.7% in normal seconds of the same
  fight). With everything on, the slowest second of the watch was 205 fps.
- **What the rounds wait for in fights:** the monster's own move (uEm124
  Velkhana 195 us, uEm023_05 Rajang 180, uEm013 Fatalis 263 in the line
  dump, uEm050 Alatreon 206) and the player's (165-193 us) each end their
  round in every frame.
- **The unit lines (tools/unit_lines.py, read-only):** the monster is in
  line 8 with its scar model, the player in line 10 with the bound
  environment models (uBindBaseModelInstance, uBindSpeedTree), animals and
  NPCs in 12, gimmicks in 14-15, armour and faces in 16, effects in 19-21,
  lights and fog in 22. Lines run in order 0-31 in each pass; the order
  looks like dependencies (the player and world, what moves with them,
  effects bound to what moved, GUI and lights last), so overlapping lines,
  even the monster's and the player's, would change what each sees.
- Single long frames with everything on (Rajang, one of 71 ms) came a few
  seconds after a switch flip; flipping applies code changes with the
  game's threads paused and moves the process between the CPU dies, which
  normal play never does.

### The CPU frame (al-21, v0.9.0; 2026-10-16)

Measuring switches on in both halves: budget (4096) and uncap (8192,
max_fps 1000); everything off against everything on (2424), four pairs of
30 s, 5 s windows, the first window of each step left out. CPU frame = the
frame interval minus the limiter's wait and the GPU wait (the fence wait in
cD3DRender::present, outside the swapchain call); "CPU-bound fps" is 1000
over it.

| Scene | fps off / on | CPU frame off / on | rounds | serial | GPU wait | process CPU per frame |
|---|---|---|---|---|---|---|
| Title screen (the normal one) | 370.2 / 367.4 | 1.843 / 1.428 ms (-22.5%; 543 / 700 CPU-bound fps) | 0.728 / 0.356 | 0.754 / 0.711 | 0.858 / 1.293 | 4.55 / 3.57 ms |
| Hub (Seliana) | 239.8 / 281.8 (+17.5%) | 4.157 / 2.830 ms (-31.9%; 241 / 353) | 2.640 / 1.450 | 1.255 / 0.997 | 0.013 / 0.718 | 17.75 / 13.55 ms |
| Ancient Forest | 167.2 / 240.4 (+43.8%) | 5.775 / 4.095 ms (-29.1%; 173 / 244) | 3.686 / 2.259 | 1.791 / 1.525 | 0.205 / 0.064 | 29.50 / 21.76 ms |

- The swapchain call is 0.25-0.37 ms a frame in every scene, on or off.
- Uncapped, the title screen is GPU-bound at about 370 fps: the CPU time
  the switches free (0.41 ms) becomes GPU wait (+0.43 ms). The hub with
  everything on already waits 0.72 ms a frame for the GPU, so its fps gain
  (+17.5%) understates the CPU gain (+47% CPU-bound fps). The forest stays
  CPU-bound (GPU wait 0.06 ms): rounds 55% of its CPU frame, serial
  game-thread work 37%, the swapchain call 7%.
- At the GPU-bound title screen, everything on was 1.4-3.5 fps slower in
  all four pairs (371.8/368.3, 370.2/367.1, 368.8/367.4, 370.2/366.9; about
  0.02 ms a frame). Not chased (under 1%). Unverified suspect: cache_ccd
  also confines the driver's threads in the process to the 8 cores of the
  cache die.
- The forest's p99 frame time: 15.38 ms off, 5.47 on.
- The budget's own cost: at the hub, uncapped, 8192 against 8192|4096
  (two pairs) 245.8 against 248.0 fps, process CPU 17.55 against 17.43 ms
  a frame; at the capped title screen 299.9 fps both. None measurable.

### The frame limiter (sub_14225F9F0; v0.10.0, al-22)

- sMain's frame timing ends each frame's wait: remaining ms = 1000 /
  mMaxFps (+0x5C) minus the elapsed time, from a running sum of QPC ticks
  times the game rate (+0x58, 60) at +0x70, the frame's reference at +0x78
  and the last QPC reading at +0x80. If 2 ms or more remain it calls
  Sleep((int)remaining - 1), then spins on QueryPerformanceCounter until
  the frame is due. A byte at +0x60 selects a fixed-step branch that spins
  with SwitchToThread instead. sAppExt::run calls timeBeginPeriod(1).
- With the switches on, a 300 fps cap leaves about 1.9 ms a frame at the
  title screen: under 2 ms, so all of it is spun on the game thread's core.
- Switch sleep_limiter (16384) waits on a high-resolution waitable timer in
  the frame-timing detour, before the game's function, until margin_us
  (500) before the deadline computed from those fields; the function then
  runs unchanged and spins the rest. In the unit test the timer woke 0.18
  ms late on average (p99 0.24 ms).
- The sampler's follow mode (control.ini follow, a vtable RVA) samples the
  thread running one class's job every 20 us while it runs: uPlayer's move
  (vtable 0x34a6d18) is next.

### The limiter in game (al-22, v0.10.0; 2026-10-16)

Title screen (the slow one: with everything on it holds the 300 fps cap
anyway), everything on + budget (6520) against the same + sleep_limiter,
30 s steps, the first 5 s window of each left out.

| Cap, margin | fps off / on | game thread CPU a frame off / on | process CPU a frame | wakes past the margin |
|---|---|---|---|---|
| 300, 500 us (four pairs) | 299.7 / 299.0 | 2.823 / 1.753 ms (-38%) | 5.73 / 4.61 ms | 9.1% |
| 144, 500 us (two pairs) | 144.0 / 143.9 | 2.454 / 1.620 ms (-34%) | 5.28 / 4.45 ms | 6.8% |
| 300, 500 us (margin run) | 299.7 / 298.9 | 2.798 / 1.750 ms | 5.70 / 4.63 ms | 9.4% |
| 300, 1000 us (margin run) | 299.7 / 299.7 | 2.798 / 2.213 ms (-21%) | 5.70 / 5.11 ms | 0.2% |

- Frame pacing unchanged at both caps (p50 3.35, p95 4.10 ms at 300).
- In game the high-resolution timer woke up to about 0.85 ms late at p99
  (0.24 ms in the unit test), so a 500 us margin left 9% of frames up to
  ~0.3 ms late: -0.3% fps at the cap. With 1000 us the cap holds exactly.
  Default margin_us is 1000 from v0.11.0; a margin learned from the
  measured lateness would get most of the 500 us saving without the loss.

### Following the critical jobs (al-22, sampler follow mode)

Everything on, time_jobs and the sampler on, follow = a class's vtable
RVA, 60 s each; sampling pauses the followed job, so these are profiles,
not timings.

- **uPlayer, Ancient Forest:** 66,000 jobs, 842,000 samples (12.8 a job).
  Per frame the player has one move job (cUnit::genericMove, 77% of its
  samples), four draw jobs in the camera rounds (22%) and one more (1%).
  The game thread ran the player's job itself in 62% of the samples. Inside
  the move (sub_141FBCCD0, the character update with the "On checking
  mQuat before/after Animation" checks): uPlayer's own update
  (sub_1420397F0, actions, stamina, targeting) about half; world collision
  queries (cStageAdjustCollision, then capsule-vs-BVH: sub_14231E090 ->
  sub_1425D1E20 -> sub_1425B0EC0 -> sub_1425AED40, pointer-chasing AABB
  tests, and the narrow phase sub_1422C7E00/sub_1422CB250) 22-24%;
  animation-event effect lookups (sub_142485020 -> sub_142363680: a hash
  table and list walk in rEffectProvider, then its parents) 11-14%. The
  hottest single function is 5%: no single waste.
- **uEm124 (Arch-Tempered Velkhana), in the fight:** 58,000 jobs, 1.08
  million samples. Move 66%, draw 34%. The same world collision chain
  takes 24% of its job time (sub_1425D1E20 inclusive). Flat otherwise.
- **uMhEffect, the same fight:** 3.6 million jobs (about 250 a frame:
  move and three draw rounds), 49,000 samples (most jobs end before a
  sample). 87% uEffect::move -> sub_1426342E0 -> sub_142633C60 (emitter
  updates), mostly math (powf, atan2f, sinf): flat.

### The Velkhana fight's frame (al-22, everything on)

- Budget (60 s, time_jobs on): CPU frame 3.553 ms (281 fps CPU-bound) at
  242 fps: rounds 1.969 ms, serial 1.212, swapchain 0.357, and 0.568 ms of
  GPU wait: at 1440p this fight is partly GPU-bound with everything on.
- Job types per frame (job timing): uMhEffect moves 1,641 us of work (122
  jobs, max 759 us; they ended their round 2.6 times a frame), uMhEffect
  draws 1,184 us (119), base model instance draws 903 us (543), the
  monster's move 201 us and the player's 189 us (one each, each ending
  its round every frame), the HUD radar's draw 121 us (one).
- Replay (job_sim.py): the unit rounds (9.1 a frame) gain at most 32 us a
  frame from any order (they wait on single long jobs; about 14 us a
  round is waking and the tail); the camera rounds (3 a frame) up to 142
  us with a perfect order, 75 us sorted by class means (job order mode
  3), nothing more from mode 2 (already on).
- The game thread (sampler, 60 s): unit passes 33% (rounds 27.5%, lines
  run alone 5.7%), Present path 27% (present12 22%, endFrame12 5% in
  D3D12Core), sMhRender::move 18% (camera rounds 10%, serial setup
  sub_1423C3790 5%), **sCollision::Collider::move 4.2%**, a second unit
  pass sub_14221B400 3.5%, sub_141B05440 3.1%. Job threads waited for work
  96% of the time.

### sCollision runs serially for the whole session (al-22, al-23; v0.11.0)

- sCollision::NodeList::executeContact has two paths: with the collider's
  flag +0x3D set it queues executeTargetActiveNodeContact per active node
  on sMain's job list and runs one executeJob round; without it the game
  thread calls them one by one. movePhase1_runSetupObjectCollisionSystem
  does the same for its node lists with +0x3C.
- sub_14248A160 sets both flags to "sMain +0xCC != 1" (more than one job
  participant). It runs in sMhMain's constructor (sMhCollision built there,
  stored at sMain +0xAF8E8, collider at +0x84E8), before the job threads
  exist. Read from the running game (read-only, al-22): participants 16,
  flags 0 and 0. Area loads do not run the setup again (al-23: the flag
  stayed as set through a quest end and the return to Seliana).
- **parallel_contacts (32768), Velkhana, al-23:** six rounds of 15 s steps
  (serial, contact pass, both passes): the game thread's serial work fell in
  every round (1.30 to 1.19 ms; 1.17 with both); the fight's own swings
  (+-200-450 us between rounds) hide the total (CPU frame -47 us, -32 us
  with both, on average). Job timing (45 s each): the contact round, 97
  jobs, waits 45.1 us (longest job 28.5 us) for 183 us of work the game
  thread did alone: about 140 us a frame back in this fight. The setup
  pass adds nothing: contacts 1 is the default.
- Hits looked and felt normal to the user (damage numbers, hits both ways,
  part breaks, clutch claw); stable through the quest end, a loading
  screen, alt-tab and menus; the loading screens' long frames (165-300 ms)
  only. Kept, on in every session from now so it sees more fights.

### cache_steam_running with three sites, job order 3 in a fight (al-24, v0.12.0)

- Ancient Forest, the kept set + budget, four pairs of 30 s: IsSteamRunning
  went from about 5 calls a frame to 98 in 100 s (131,898 answers reused);
  CPU frame 3.681 against 3.679 ms, serial game-thread work 1.356 against
  1.338 ms. About 18 us a frame: no measurable gain on the CPU frame either.
  The sampler had put the three calls at 1.5% of the game thread; its own
  thread suspensions likely make registry calls look dearer. Optional.
- Arch-Tempered Velkhana, job order 2 against 3 with time_jobs, six pairs of
  15 s: the camera round 167.0 against 160.8 us, but the CPU frame +73 us on
  average (worse in four of six pairs; reordering all ~540 jobs costs the
  game thread more than the round saves). Mode 2 stays.

### With the DLSS mod (al-25, v0.12.0; DLSS 0.28.5, quality, no frame generation, no Reflex)

- Ancient Forest, cpuopt off (4096|8192) against the kept set, four pairs of
  30 s: 168.9 against 238.4 fps (+41%), CPU frame 5.916 against 4.185 ms, p99
  7.30 against 5.30 ms. nvidia-smi: GPU 58% busy (317 W) against 79% (401
  W). The forest is CPU-bound with DLSS too (GPU wait under 0.01 ms in both
  halves); DLSS alone gives nothing there (al-21 without it: 167 fps).
- Both mods together: no errors, nothing changed in either's hooks.

### Windows Game Mode parks the frequency die (al-25, al-26)

- With the game focused it ran 234 fps, unfocused 265 (1 s windows, the
  user focusing for 30 s): focused frames had +0.40 ms of job rounds, +0.11
  ms of serial work and +2.5 ms of process CPU. typeperf (read-only,
  Processor Information % Processor Time and Parking Status): focused, CCD1
  was parked 90% of the time (4% busy, 18% unfocused) and CCD0 went from
  42% to 56% busy: everything else on the PC was crowded onto MHW's die.
- Cause on the test PC: Windows Game Mode on (AutoGameModeEnabled 1), the
  amd3dvcache driver running, the Balanced plan letting class-1 cores park
  to 0%. The user turned Game Mode off and set the BIOS's CPPC preferred
  cores to frequency (al-26): CCD1 parked 0.00 with the game focused.
- Runs before al-26 ran with the game focused under this parking (the A/B
  halves alike), so their numbers are a little low against a clean setup.

### cache_ccd on a clean setup, and stock against everything (al-26, v0.12.0)

Ancient Forest, DLSS quality, Game Mode off, BIOS preferring the frequency
die, no Process Lasso rule for MHW; four pairs of 30 s each.

| | fps | CPU frame | p99 | process CPU a frame | CCD0 / CCD1 busy |
|---|---|---|---|---|---|
| kept set without cache_ccd | 188.1 | 5.295 ms | 6.25 ms | 41.6 ms | 29% / 47% |
| kept set | 262.7 (+40%) | 3.776 ms | 4.53 ms | 21.9 ms | 39% / 18% |
| everything off (stock) | 146.6 | 6.814 ms | 7.95 ms | 46.5 ms | |
| kept set | 264.5 (+80%) | 3.747 ms | 4.50 ms | 21.8 ms | |

- Without a pin, Windows put MHW's busiest threads on the frequency die (32
  MB L3) and spread the rest over both dies: cross-die traffic and a third
  of the cache. cache_ccd's +3.6-4.9% in al-17/19 was measured while Game
  Mode parked CCD1 for the focused game, which put MHW on CCD0 anyway.
- Gains by setup, Ancient Forest: Game Mode parking (Windows' default on
  X3D) 169 to 238 fps (+41%, al-25); no parking, BIOS preferring frequency,
  no pin 147 to 265 (+80%, al-26). A setup with no preference at all may
  scatter the game across the dies more and gain more; not measured.
- v0.12.1: cpu_limit's switch-off path set the mask read at startup, which
  would undo a Process Lasso rule applied after it; it now restores only an
  affinity it set itself.

### With a Process Lasso rule, and the fight with DLSS (al-27, v0.12.1)

DLSS quality, Game Mode off, BIOS preferring the frequency die, and the
user's Process Lasso rule pinning MHW to CCD0 (the game stayed on 0xFFFF
in the stock steps, where cpuopt holds no mask).

- Ancient Forest, four pairs of 30 s: stock 181.9 fps (p99 16.68 ms: the
  effect loop's spikes), CPU frame 5.242 ms; the kept set 268.6 fps (+48%),
  3.694 ms, p99 4.40 ms. With cpuopt the Lasso rule makes no difference
  (al-26 without it: 264.5); without cpuopt it is worth +24% (146.6 to
  181.9).
- Arch-Tempered Velkhana, eight pairs of 15 s: stock 175.2 fps (GPU 62%
  busy, 345 W), the kept set 285.6 fps (+63%, every pair +54% to +76%; GPU
  97% busy, 482 W). CPU frame 5.704 to 3.154 ms (317 fps CPU-bound); with
  cpuopt the fight is GPU-bound at 1440p with DLSS quality on an RTX 5090.

### Player mode in game (al-28 v0.13.0, al-29 v0.13.1)

No run folder: cpuopt.ini next to the DLL, cpuopt.log (1 MB limit), CMFM
0.1.0's Mods page, DLSS 0.28.5 quality, the same setup as al-27.

- al-28 (12 min): the page showed next to the DLSS mod's; the hotkey set on
  the page (Ctrl+P) flipped everything about five times; remove_fps_cap
  raised the cap to 1000 (385 fps in Seliana) and set it back to 300 when
  switched off. 13 changes, 13 saves of the ini, 0 failed switches in 11
  status checks. The status line counted only 3 of 7 optimizations: in
  menu.cpp the page's row names (an enum in the same namespace) shadowed
  four switches' toggle values, so the count loop read 4, 5, 6 and 7 as
  toggles. v0.13.1 names the rows row_*, qualifies the toggles and checks
  the table with a static_assert and the smoke test.
- al-29 (8.5 min): the status line reads all 7 on. Every switch was
  turned off and on one by one from the page, and everything with the
  hotkey: 55 changes, 54 saves, 0 failed in 13 status checks, the cap held
  at 1000 throughout.

### The Mods page's benchmark in game (al-30 v0.14.0, al-31 v0.14.1)

Ancient Forest camp, DLSS quality, the Lasso rule, a Discord stream running
alongside (al-27's setup otherwise). One benchmark, 8 pairs of 15 s:
171.7 to 270.5 fps (+58%, every pair +55% to +60%), game thread CPU time
per frame 5.03 to 3.44 ms (-32%), p99 16.65 to 4.30 ms, all threads' CPU
per frame 28.3 to 21.1 ms. The "on" side matches al-27's manual A/B (269
fps, 3.69 ms); the "off" side is 10 fps lower, likely the stream (stock MHW
is the more CPU-starved). The page's result line was squeezed into its
value box (about 30 characters fit); v0.14.1 splits it into two rows, and
al-31 showed every benchmark line fitting. This build is the closed beta's
package (cpuopt-0.14.1-beta.zip, DLL f553bad4).

The logs of al-29 to al-31 end without stopping/stopped/shutdown/exit: the
user ends the game with the taskbar's End task, which terminates the
process without EXITING or DLL detach. Nothing is lost: the changes live in
the process's memory, and cpuopt.ini is saved within a second of a change.

### Closed beta, first results (2026-10-16/17, v0.14.1 and v0.14.3)

Logs and reports in `local/beta/`. Both testers ran the game build cpuopt
was made on; every signature matched once and every switch applied, with
many other mods loaded (Stracker plugins through Astera Loader's
compatibility layer, an old CRC bypass, HunterPie, the Discord overlay, the
DLSS mod with Streamline). No crash or conflict was reported.

- **i7-13700K (8 P + 8 E cores), RTX 3080 Ti, 1440p, DLSS performance,
  Reflex off, all 24 threads, normal priority (v0.14.3, Elder's Recess;
  the user corrected the scene on 2026-10-19):** GPU-limited at 159 fps in both states (GPU 99%). The game
  thread's CPU time per frame 5.68 to 5.15 ms (-9.4%; busy 90% to 82%); job
  rounds 3.01 to 2.77 ms, its own work 2.23 to 1.91 ms, Present (the GPU
  wait) 1.02 to 1.58 ms; all threads' CPU per frame 31.8 to 32.3 ms (the
  windows vary about 5%). The off windows had no frame-time spikes (p99
  8.4 ms) though the effect ring is write-combined there too.
- **i5-12600KF, RTX 3060 Ti, 1440p, Reflex on (v0.14.1, Elder's
  Recess):** 116 fps in both states, GPU 99% even at DLSS performance.
  Something on the PC limited MHW to logical processors 0, 2, 4, 6, 8 and
  10 (one thread per P-core) at High priority from the start, while the
  engine still starts 15 job threads. frame_budget's "CPU" came out at 7.7
  of 8.6 ms: Reflex's wait sits outside Present and the limiter, so that
  measure counts it. v0.14.3 adds the game thread's cycle count (no sleep
  inflates it), frame_budget's parts, the DLSS mod's settings and the
  affinity and priority set outside cpuopt to the report.
- Against the 9950X3D (al-26: the kept set without cache_ccd cut the CPU
  frame 22%, with it 45%; al-30: off p99 16.65 ms, on 4.30 ms), the
  13700K's gain is a third of that and its off windows show none of the
  effect loop's spikes, in Elder's Recess, the effect-heavy area where the
  third tester's 9950X3D gained 56%. Hypothesis, not shown: reading
  write-combined memory costs far less on these Intel CPUs than on Zen, so
  plain_prim_stores, the biggest switch, is mostly a Ryzen fix. Wanted: a
  non-X3D Ryzen, and the 13700K CPU-limited (the game's resolution scaling
  low).
- **Ryzen 9 9950X3D, RTX 5080, 1440p Borderless, DLSS mod performance,
  Reflex and frame generation off, no frame rate limit, the 300 fps cap
  removed, all 32 logical processors, no affinity tool, no parked cores
  (v0.15.0, third tester, 2026-10-17, Elder's Recess):** 137.2 to 257.8
  fps, +88% (pairs +79 to +93%); p99 8.62 to 5.75 ms. The game thread's CPU
  time per frame 7.20 to 3.19 ms (-56%; busy 99% to 82%), all threads'
  33.9 to 16.7 ms (-51%). Job rounds 4.52 to 1.57 ms, its own work 2.29 to
  1.30 ms, Present 0.44 to 0.98 ms. Off CPU-limited, on held by the
  graphics card (the CPU alone about 313 fps). The off half ran over both
  dies (nothing pinned it), so this is the README's "clean setup" case
  (al-26: +80% on the test PC) reproduced on a second 9950X3D with a
  different graphics card and set of mods; it says nothing new about
  non-X3D Ryzen. Every signature matched, no failures, clean restore at
  exit; the master switch's hotkey was used several times.
- **Core i5-12400F (6 cores, no E-cores, one L3), RTX 3060, 1920x1080
  Borderless, V-Sync on (a 144 Hz FreeSync monitor), no frame rate limit,
  AO off and low shadows, the DLSS mod loaded (its settings are not in
  v0.14.1's report), the old CRC bypass and Stracker plugins (v0.14.1,
  fifth tester's report forwarded on 2026-10-19, scene not stated; 8 pairs
  of 15 s):** 82.3 to 83.3 fps (+1.1%, pairs -1.3 to +3.6%), p99 18.1 to
  18.2 ms; the game thread's CPU time per frame (v0.14.1's frame_budget
  measure) 6.90 to 6.38 ms (-7.5%), all threads 37.6 to 37.1 ms. That
  measure is 55-59% of the frame, the rest the wait in Present: Reflex
  sleeping would have been counted and brought it near the whole frame (as
  on the 12600KF), so the cut is most likely real game-thread work.
  GPU- or V-Sync-limited in both halves; the 13700K's -9% again, so the
  small Intel figure holds without E-cores too. Update threads was
  "waiting" until the benchmark re-requested it (the player-mode bug fixed
  in v0.16.0); the report's list of on switches named "stay on the cache
  die" (0.14.1's bug, fixed in 0.14.2). Frame-time spikes of 20-64 ms in
  both halves alike.
- Most players with mid-range graphics cards at 1440p are GPU-limited, so
  their fps cannot rise. The README now says who gains fps; a live "Limited
  by" line on the Mods page is proposed (roadmap).

### Parallel hit detection: the race, and the contact guard (2026-10-21, v0.17.0)

A player on cpuopt 0.16.x (Intel 12th-gen laptop, P and E cores) crashed
twice in the Arch-Tempered Velkhana fight at the same instruction:
`subss xmm3, [r12+240h]` at exe+0x12D61E3 in `sub_1412D5E90`, r12 = 0, on
the game thread. With parallel_contacts off the crashes stopped (the DLSS
project's crash analysis, 2026-10-20). Checked in this project's clean
database:

- **The race the crash came from.** `sub_1412D5370` writes the hit records of
  the object that was hit (112 bytes each at +0x17F0, at most 128, the count
  at +0x4FF0, a critical section at +0x5750). It takes a slot with
  `lock xadd` on the count, makes two virtual calls, then fills the record
  without a lock, the partner pointer (+0x40) among the last fields. Only
  when the list is full does it take the critical section, and then it
  compacts the list in place, copying records another thread may still be
  writing (a null or stale +0x40) and zeroing the tail. The reader
  (`sub_1412D5E90`, `sub_1412DEF10`) checks most fields but loads +0x40 and
  reads +0x240 from it without a check. With one thread (the shipped game)
  none of this can happen.
- **The whole family is unsafe.** In the parallel pass,
  `executeTargetActiveNodeContact` runs one job per active node on all job
  threads; the geometry (BVH queries, `cCollisionNodeObject::hitCheck`)
  takes a stack per thread and is built for it, but `hitCheckCallback` calls
  the active node's callback with code 0 for every contact and then the hit
  node's own callback (node +0x80 / +0xD0, code 1, any object's reaction),
  and the job calls the active node's callback with codes 4 and 2. The four
  callbacks `enumContact` registers all write shared state without a lock
  for code 0: the hit callback `sub_1412D6D20` (the records above and the
  attack history at target +0xA40); `sub_142396960` (a check-then-write
  nearest contact); `sub_1425109C0` (`array[count++]`, clamped from 9 to 7
  after the store, slot 8 being the count itself: a second thread can read a
  pointer as the count and write out of bounds); `sub_1426A16C0`,
  uGeometry2Collider (an allocation appended to a shared `MtArray`, Reserve
  and ++ without a lock). Only enumContact and `sub_142489260` (a one-off
  check on its caller's thread) create active nodes, but enumContact_0 is
  also referenced from data.
- **Why the 9950X3D never crashed:** the overflow has to land while another
  thread is between its slot and its +0x40 store; slow efficiency cores
  widen that window.

**The contact guard (contact_guard.hpp, v0.17.0):** one recursive lock
(SRW, a per-thread depth) around `hitCheckCallback` whole (codes 0 and 1),
the four known callbacks (codes 4 and 2), and any job whose callback is none
of the four (whole job, counted); the geometry stays parallel. The lock is
taken only while the contact flag is set: contacts.cpp switches it on before
it sets the flag and off after it clears it, at BEGIN on the game thread.
The flag is set only once all seven hooks are active (fail closed: "no
guard"). A counting detour on the record writer gives the overflows; in
research mode the guard also times waits and holds and counts same-object
overlaps (two threads in the guarded code for one object: the races it
prevents). plugin_tests: four threads never meet inside the guarded fakes
while locking (up to four do without it), nested calls do not lock twice,
unknown jobs run serialised. Not measured in game yet: the lock's cost
against al-23's 140 us a frame, and whether the overlaps and overflows show
on the 9950X3D.

### The GPU side, first look (al-34 to al-39, v0.14.4 to v0.14.6; 2026-10-17)

Setup: the RTX 5090's clock locked at 1200 MHz by the user (`nvidia-smi
-lgc 1200,1200`) so the GPU sets the frame rate; no DLSS mod; Ancient Forest
camp, 2560x1440, the game's own AA, the user's options (AO High, Volume
Rendering Highest, Shadows High, SSR, SSS, Z-Prepass on). NVIDIA Broadcast
running unless noted.

**The Mods page's benchmark, GPU-limited (cpuopt on half, fps):**

| Change from the baseline | fps | GPU frame | |
|---|---|---|---|
| baseline, Windowed (start / end of al-37) | 156.9 / 154.0 | 6.37 / 6.49 ms | drift about 2% (time of day, monsters) |
| Z-Prepass off | 150.9 | 6.63 ms | -4%: the prepass pays for itself |
| Ambient occlusion off | 177.5 | 5.63 ms | AO High costs 0.74 ms |
| Ambient occlusion Mid | 172.8 | 5.79 ms | AO Mid costs 0.16 ms |
| NVIDIA Broadcast closed | 160.6 | 6.23 ms | about 2%, near the noise |
| Borderless (al-38) | 158.5 | 6.31 ms | within the noise of Windowed |
| nine heavy options off (al-34) | 229.7 | 4.35 ms | about a third of the GPU frame |

- With the GPU limiting, cpuopt still cuts the game thread's CPU time per
  frame 34-37% on the 9950X3D (busy 82-91% to 53-63%); the saved time
  becomes GPU wait in Present. With the heavy options off the off half
  turns CPU-limited and cpuopt gains +25% fps (183 to 230).
- Frame-time spikes of 16-19 ms: in Windowed runs the cpuopt-off windows
  always have them (the effect loop: al-30's on windows had none, so they
  follow the switch, not the window); with cpuopt on they come back only
  when the GPU is maxed. The user sees separate hitches from the window tool
  Borderless Gaming when the window changes (menus); on 2026-10-19 the
  user confirmed that app as the cause of those hitches (not cpuopt, not
  the game) and replaced it with a small borderless tool of their own;
  runs from then on are without Borderless Gaming.

**One frame through PIX (`local/pix/`, captures stay private):** captured
in game through cpuopt's switches (PIX's layer loaded at PRE_MAIN,
`CaptureNextFrame` on request; pixtool cannot attach for GPU captures to a
process it did not launch), replayed with pixtool at the same 1200 MHz;
timing needs the Performance Log Users group (or admin). The main queue:
276 passes, 3.79 ms (per-pass medians over seven replays; a few compute
passes overlap differently from replay to replay). The largest: the
G-buffer 0.51 ms (three passes), AO 0.42 ms (one full-screen draw), the
Z-prepass 0.41 ms (its alpha-tested foliage runs 13.8 M pixel-shader
invocations for depth), indirect and compute passes without images 0.88 ms,
full-screen passes about 0.4 ms. No single waste stands out. In game the
GPU frame is 6.4 ms against the replay's 3.8: work on the second queue
(the frame waits on it), contention and the replay's idealisation are the
candidates; not explained yet (a PIX timing capture would show it).

**AO High against Mid (captures of one spot with High, Mid, Off):** not a
bug. Both use `ApproximateSSAO` with Capsule AO; High runs it at full
resolution (3.69 M pixels, 0.416 ms), Mid at half resolution (0.92 M
pixels, 0.089 ms) and scales it up; a full-screen pass that combines the
sun's screen-space shadow mask with the AO runs at every setting (0.03-0.05
ms). In the code: `sub_142269EB0` draws the AO pass from a settings object
(`+0x110` float: the AO resolution scale; `+0x11C`: the algorithm,
0 ApproximateSSAO, 1 a path over three rotating buffers (temporal, by the look), 2/3 HDAO and HDAO2,
4/5 HBAO and HBAO+); `sub_14227DBD0(full, x4, capsule, hiz, compute)` picks
the shader variant (`[CS]ApproximateSSAO[Full][x4][WithCapsuleAO][_HIZ]`),
`full` from `sub_142594CE0`'s level. The buffers look alike, Mid softer.
For players: AO Mid gets back most of High's cost.
