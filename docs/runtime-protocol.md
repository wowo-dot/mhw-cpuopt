# In-game run protocol

## Installation (Astera Loader plugin, since v0.2)

- MO2 mod folder `cpuoptTEST` (path in `local/machine.json` → `slot`):
  `AsteraLoader\plugins\cpuopt\cpuopt.dll` plus the sidecar
  `cpuopt.dll.output-dir` naming the current run folder. Astera Loader loads
  only `<name>.dll` from a plugin folder; the sidecar is the plugin's own file.
- Without the sidecar, or if the run folder is not writable, the plugin stays
  idle: it subscribes to nothing (so Astera Loader hooks nothing for it) and
  its status is DEGRADED with the reason.
- The game is started with MO2's "Astera Loader" executable
  (`AsteraLoaderNew\AsteraLoader.exe`, release 0.2.3, API 1.8).
- A vanilla measurement uses a profile with only Astera Loader and
  `cpuoptTEST` enabled: no DLSS mod, no CMFM, no Stracker plugins. The log's
  `modules` event lists what actually loaded.
- v0.1 was a Stracker's Loader plugin (`nativePC\plugins\mhw_cpuopt.dll`); it
  was never run in game and was removed from `cpuoptTEST` on 2026-10-15, so
  Astera Loader's Stracker compatibility does not load a second copy.

## One run

1. `python -B tools/run.py prepare <name> --note "<scene, config>"` (game
   closed). Installs the DLL only if it changed, backs up the previous one,
   writes `run-receipt.json` and the sidecar.
2. The user launches through MO2 ("Astera Loader") and goes to the scene.
3. While the game runs:
   - `python -B tools/run.py sequence <name> --marker hub-start`: 60 s with
     every switch off, then 60 s with the stack sampler, then
     `hub-start-done`;
   - or by hand: `run.py control <name> --marker <m> --toggles 0|1`.
   - `run.py sweep <name> --marker hub-cap --steps 0:45,15:45,0:45,11:45`:
     the job_thread_cap A/B, one step per job-thread count (0 = the game's own
     count, switch off), sampler off unless `--profile`;
   - `run.py steps <name> --marker hub-time --steps 0:20,4:90,0:20`: any
     switch states in order (toggles:seconds); `--repeat N` runs the list N
     times, for interleaved A/B (`--steps 8:30,24:30 --repeat 4`);
   - or by hand: `run.py control <name> --toggles 2 --job-threads 15`;
     `--job-order`, `--order-us`, `--inline-us` and `--time-ms` set the
     switches' parameters (they stay in control.ini for later steps).
   Each change closes the current statistics window first, so no window mixes
   two states.
4. `python -B tools/run.py status <name>` shows the last windows live.
5. After the game has closed: `python -B tools/run.py seal <name>`.
6. Reports: `tools/report.py <name>` (frame times and CPU per scene and
   switch state), `tools/profile_report.py <name>` (hotspots, the game and
   present threads first).

## What the log holds (`cpuopt-<pid>.jsonl`)

- `start`: version and commit, loader (name, API version, MHW build string,
  its exe hash), executable and DLL SHA-256 (and whether the executable is the
  tested build), CPU brand, caches with their processor masks, CPU sets
  (core, last-level cache, parked), process affinity, OS build,
  `graphics_option.ini`, `config.ini` `[CPU]` and `[GRAPHICS]`.
- `modules`: every loaded module with base, size and SHA-256 (outside the
  Windows folder); logged again when the set changes.
- `frame_clock`: the sRender frame-counter signature search at PRE_MAIN
  (scanner used, matches, derived singleton RVA and counter offset, whether
  they equal the tested build's).
- `al_frame_subscribed`, `al_d3d12_subscribed`, `game_ready` (title screen,
  with Astera Loader's frame index).
- `control`: switch and marker changes (or a rejected file).
- `window` (every 5 s by default):
  - `update`: game frames from al.frame (exact time stamps on the game's
    frame thread): count, fps, the frame thread's id, the interval between
    frames and the game update's own time (BEGIN to END), each with mean,
    p50/p90/p95/p99/p99.9, max and the count over 25 ms;
  - `present`: the game's Presents from al.d3d12: count, fps, the present
    thread's id, the interval and the time inside Present;
  - `render`: sRender's frame counter advance (read once per frame at BEGIN);
  - per-thread CPU time (cycle counted), context switches, thread count, busy
    share of every logical processor (all processes);
  - `poller` only if al.frame cannot follow the game's frames.
- `job_cap`: the four job_cap signatures' match counts and the sMain global
  (`src/plugin/job_cap.hpp`); `job` in each window: state (no target,
  waiting, native, capped, layout mismatch, drift, restored), the game's own
  count, requested and applied threads per job round.
- `job_timing_switch`, `job_timing` (toggle 4): the executeJob detour's
  registration, and per window the rounds per round type and per job type
  (`tools/job_report.py`); `job_trace`: whole rounds, one in 499, for
  `tools/job_sim.py`.
- `job_hook`: the executeJob detour registered or switched (toggles 4 and
  8 share it); `job_order_site`: whether the camera round's call site was
  found; `order` in each window (toggle 8): mode, rounds reordered, jobs
  moved, learning rounds, classes known.
- `prim_stores`, `server_time`: their signature searches at PRE_MAIN (match
  counts, site and global RVAs). `prim_buffer`, once: the memory type of
  sPrimitive's record ring (read-only VirtualQuery; `write_combined`).
- `prim_patch`, `server_hooks`: the al.patch change and the two detours
  applied or switched, with the reason when they cannot be. In each window:
  `prim_stores` (true while the change is in), `server_time` (active,
  time_ms, reads answered by the game's code, reads reused, calls in another
  session state), `inline` (on, threshold, rounds run alone, timed
  rounds, and per call site: RVA, mean work in 0.1 us, timed rounds, alone)
  and `spin` (active, spin_us, rounds announced, permits taken by spinning
  threads, waits that blocked, claimed places whose permit did not come).
  `worker_spin` / `spin_patch`: the job threads' wait found, and the change
  applied or switched. `units` (unit_threshold: state, native 60, requested,
  applied) and `steam_running` (active, calls Steam answered, answers
  reused); `unit_lines`, `steam_running` and `running_patch` events at start
  and when switched.
- `profile` (only while toggle 1 is on): whole stacks per sampled thread with
  counts; the game and present threads are always sampled. From v0.6 also
  `leaves`: the game exe's hottest exact instructions ([rva, game thread,
  all]), for `profile_report.py --leaves N`.
- `stopping` / `stopped` at EXITING (the last window, sampler joined),
  `shutdown` at SHUTDOWN, `exit` from DllMain.

## Measurement rules

- A result is several windows per configuration in the same scene, with the
  switch state in the log; one fps reading is not a result.
- The sampler suspends the sampled threads about 1,000 times a second (about
  15 us each); frame times measured with it on are not baseline numbers.
- The engine caps at 300 fps; the unmodded hub start ran 255-261 fps on the
  test PC (dlssupdate perf13). Gains near the cap need heavier scenes or a
  smaller core budget (Process Lasso) to show.
- Record the scene, graphics settings and which mods are loaded in the run
  note; the log records what it can detect itself.

## The switches (v0.6)

| Toggle | Name | What it changes | Parameters |
|---|---|---|---|
| 1 | sample_profile | research: the stack sampler | |
| 2 | job_thread_cap | threads per job round | job_threads |
| 4 | time_jobs | research: times every job (executeJob detour) | |
| 8 | order_jobs | the camera round's job order | job_order (2), order_us (20) |
| 16 | plain_prim_stores | sPrimitive's record loop stores without reading (al.patch, 56 bytes) | |
| 32 | reuse_server_time | Steam's server time read once per time_ms (two detours) | time_ms (1000) |
| 64 | inline_rounds | rounds with little work run on the game thread alone | inline_us (25) |
| 128 | spin_workers | job threads spin after a round and take the next round's places without being woken (al.patch, 11 bytes, and a jump stub); slower in game (al-13), research only | spin_us (300) |
| 256 | unit_threshold | sMhUnit's threshold between running a unit line alone and on the job threads | unit_us (60, the game's) |
| 512 | cache_steam_running | SteamAPI_IsSteamRunning answered once per time_ms at its two per-frame call sites (al.patch, 2 x 6 bytes) | time_ms (1000) |

Toggles add up: 8 + 16 + 32 + 64 = 120 is every optimization switch of
v0.6; v0.7.2 adds 256 and 512 (888 with both). A
switch whose code is not found, or that Astera Loader refuses, stays off and
says why in the log; the rest still apply.
