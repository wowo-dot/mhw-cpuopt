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
     switch states in order (toggles:seconds);
   - or by hand: `run.py control <name> --toggles 2 --job-threads 15`.
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
- `profile` (only while toggle 1 is on): whole stacks per sampled thread with
  counts; the game and present threads are always sampled.
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
