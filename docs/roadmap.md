# Where cpuopt stands and what comes next

Updated 2026-10-16 (after al-20). Numbers and methods: `research/engine-notes.md`;
how a run is done: `runtime-protocol.md`.

## Kept: switches that help (measured on and off in one session)

| Toggle | Switch | Result |
|---|---|---|
| 16 | plain_prim_stores | the effect vertex loop stops reading GPU upload memory: hub +10%, Ancient Forest and Elder's Recess most of their +35-66%, one title screen 2.6x, Fatalis's nova dip (about 60 fps) gone |
| 8 | order_jobs (mode 2) | the camera round's expensive jobs first: hub +4.8% |
| 32 | reuse_server_time | Steam's server time read once a second: hub +3.0% |
| 256 | unit_threshold (unit_us 40) | sMhUnit hands more lines to the job threads: Ancient Forest +2.8% |
| 2048 | cache_ccd | the game on the 3D V-Cache die of a dual-die X3D CPU: +3.6% |

All of them (2424, with inline_rounds 64 which costs nothing measurable):
Ancient Forest 181 to 244 fps (+35%), AT Velkhana 158 to 233 (+47%),
Furious Rajang 204 to 267 (+31%); on six- and four-core settings +33-34%.

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

1. **Measuring and the frame limiter** (now): frame_budget and uncap are
   built (v0.9.0); next, re-measure the kept switches on the CPU frame at
   the hub and the Ancient Forest. The limiter (sMain mMaxFps, +0x5C) Sleeps
   only when 2 ms or more remain and otherwise spins on
   QueryPerformanceCounter (about 30% of the game thread at a capped title
   screen): a "sleep instead of spin" switch would keep the same cap
   without burning a core.
2. **The player's and the monster's moves:** a sampler mode that follows the
   thread running one chosen job (the player's, a monster's) and samples it
   every tick; then look for waste inside those ~200 us.
3. **The DLSS mod together with cpuopt,** with the GPU's utilization logged
   (nvidia-smi, read-only): coexistence, and whether the GPU is now the
   limit at 1440p and 4K.
4. **Player mode** (when the research settles): the kept switches on without
   a run folder, no logs, and a short guide.
5. **Parked** (engine restructuring with real risk): overlapping camera
   rounds with the next camera's setup, Present and frame submission off
   the game thread, overlapping unit lines.

Pushing to GitHub happens when the user asks.
