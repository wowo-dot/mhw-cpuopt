# Where cpuopt stands and what comes next

Updated 2026-10-17 (after al-32 and the first beta reports). Numbers and methods: `research/engine-notes.md`;
how a run is done: `runtime-protocol.md`.

## Kept: switches that help (measured on and off in one session)

| Toggle | Switch | Result |
|---|---|---|
| 16 | plain_prim_stores | the effect vertex loop stops reading GPU upload memory: hub +10%, Ancient Forest and Elder's Recess most of their +35-66%, one title screen 2.6x, Fatalis's nova dip (about 60 fps) gone |
| 8 | order_jobs (mode 2) | the camera round's expensive jobs first: hub +4.8% |
| 32 | reuse_server_time | Steam's server time read once a second: hub +3.0% |
| 256 | unit_threshold (unit_us 40) | sMhUnit hands more lines to the job threads: Ancient Forest +2.8% |
| 2048 | cache_ccd | the game on the 3D V-Cache die of a dual-die X3D CPU: +3.6% with Windows Game Mode parking the other die (al-19), +40% without it (al-26) |
| 32768 | parallel_contacts (contacts 1) | sCollision's contact pass as a job round, the engine's own path left off by an init-order slip: about 140 us a frame back in the Velkhana fight (al-23) |
| 16384 | sleep_limiter (for capped play) | the frame limiter sleeps instead of spinning: game thread -21% at a 300 cap with margin 1000, same fps and pacing (al-22) |

All of them (2424, with inline_rounds 64 which costs nothing measurable):
Ancient Forest 181 to 244 fps (+35%), AT Velkhana 158 to 233 (+47%),
Furious Rajang 204 to 267 (+31%); on six- and four-core settings +33-34%.

On the CPU frame (al-21, uncapped): Ancient Forest 5.78 to 4.10 ms (-29%),
hub 4.16 to 2.83 ms (-32%), title screen 1.84 to 1.43 ms (-22%); process
CPU per frame -21 to -26%.

## Rejected or not shown

spin_workers (128, slower everywhere), job_thread_cap (2), inline_rounds'
own effect (64), cache_steam_running (512), job order mode 3, spinning
wake-ups in any form. Their notes say why.

## Research tools

Stack sampler with exact hot instructions and a job-thread focus
(sample_jobs), job timing and round traces with an offline replay
(job_sim.py), the smaller_cpu switch (1024), a read-only dump of the unit
lines (unit_lines.py), reports for frames, profiles and jobs.

## What limits the frame now

On the test PC (9950X3D, RTX 5090, 1440p) the game thread is the limit
(about 93% busy). It waits for each job round's longest job: the monster's
move (165-263 us) and the player's (165-193 us) end their rounds every
frame in fights. Light scenes reach the engine's 300 fps cap, and at the
hub the game thread already waits on the GPU's frame fence.

## How gains are measured (from v0.9)

fps alone is a weak research measure: the engine's 300 fps cap and the GPU
flatten it, and it does not say which part of a frame changed. Results are
reported as:

1. **CPU frame** (toggle 4096, frame_budget): per frame, the interval minus
   the limiter's wait and the GPU wait; what a frame costs the CPU with no
   cap and an infinitely fast GPU. With its parts: limiter, job rounds,
   Present path (GPU wait, swapchain call), serial game-thread work.
2. **Process CPU per frame:** all threads' work.
3. **Frame-time percentiles** (p95, p99, worst) for smoothness and hitches.
4. fps, secondary; with uncap (8192) on in both halves of an A/B, capped
   scenes measure again.

Both measuring switches stay on in both halves of every A/B.

## Plan

1. **Measuring and the frame limiter (done, al-21, al-22):** the CPU frame
   is the measure; sleep_limiter keeps capped play's pacing with a fifth to
   a third less game-thread CPU. Left: a margin learned from the timer's
   measured lateness.
2. **The critical jobs (done, al-22):** the player's, Velkhana's and the
   effects' jobs are flat profiles (world collision about a quarter of the
   first two); no single waste. The fight breakdown found sCollision's
   serial contacts instead (parallel_contacts, al-23).
2b. **The game thread's serial work, next:** in the Velkhana fight 1.2 ms
   a frame: unit lines run alone (~240 us), the second unit pass (~146
   us), sCamera::draw's serial setup (~220 us, sub_1423C3790), sub_141B05440
   (~130 us), endFrame12 (~220 us, D3D12). Also job order mode 3 in fights
   (replay: 75 us a frame).
3. **The DLSS mod together with cpuopt (al-25, al-26):** no conflicts; the
   forest stays CPU-bound with DLSS quality (GPU 58% busy stock, 79% with
   cpuopt); stock against the kept set +41% (Game Mode parking) to +80%
   (clean setup, no pin). The Velkhana fight with DLSS +63%, GPU-bound
   (al-27); with cpuopt the Lasso rule changes nothing. Left: frame
   generation.
4. **Player mode (done, al-28, al-29; v0.13.1):** without a run folder the
   kept switches are on from cpuopt.ini, with a CMFM page (a switch and a
   line of help per optimization, everything on/off with a hotkey, a live
   status line, the experimental 300 fps cap removal) and a small log.
5. **Release (closed beta running, v0.14.3):** the player guide, tester
   notes, the Mods page's benchmark (8 off/on pairs, a report testers send
   back) and `tools/package.py` are done. Two testers so far, both Intel and
   GPU-limited (engine notes, "Closed beta, first results"). Wanted: a
   non-X3D Ryzen, a CPU-limited Intel run. Then the public release (fresh
   repo history, noreply author, licence check).
   - Proposed: a live "Limited by" line on the Mods page (CPU, graphics
     card or frame rate limit, from the game thread's cycle count against
     the frame time), and the benchmark's page line saying "GPU-limited"
     when it was; the cutoff needs a CPU-limited reading with the cycle
     count from the test PC.
   - Ideas from the testers: "Stay on the performance cores" for Intel
     hybrid CPUs (the cache_ccd pattern by efficiency class), and job rounds
     sized to an affinity set outside cpuopt (the 12600KF's 6 of 16); both
     need measuring first.
6. **The GPU side (starting):** most players are GPU-limited. Measuring
   first, on the test PC with the GPU clock locked by the user (nvidia-smi
   -lgc) and no DLSS mod: the cpuopt benchmark as the GPU-limited baseline
   (fps then measures the GPU), each graphics option's cost, then per-pass
   GPU timings from PIX captures (v0.14.4 loads PIX's capture layer for
   that; `AGENTS.md`). Candidates come from what the captures show: passes
   done twice, effects at a resolution nobody would miss, off-screen work.
   Also open: whether the game thread's wait for the GPU in Present spins.
7. **Parked** (engine restructuring with real risk): overlapping camera
   rounds with the next camera's setup, Present and frame submission off
   the game thread, overlapping unit lines.

Pushing to GitHub happens when the user asks.
