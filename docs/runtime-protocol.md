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
     switches' parameters (they stay in control.ini for later steps);
     `--set KEY=VALUE` writes any other control.ini key, e.g.
     `--set wc_scan=1`.
   Each change closes the current statistics window first, so no window mixes
   two states.
4. `python -B tools/run.py status <name>` shows the last windows live.
5. After the game has closed: `python -B tools/run.py seal <name>`.
6. Reports: `tools/report.py <name>` (frame times and CPU per scene and
   switch state), `tools/profile_report.py <name>` (hotspots, the game and
   present threads first).

## What the log holds (`cpuopt-<pid>.jsonl`)

- `init` (from v0.16.0), the first line, written before anything slow:
  version and commit, mode (player or research), the loader's API and game
  version, whether the loader gave the exe's hash, and Wine's version under
  Wine or Proton (empty on Windows). A log from a game that ends early still
  says what ran.
- `start`: version and commit, loader (name, API version, MHW build string,
  its exe hash), executable and DLL SHA-256 (the exe's from the loader when
  it gave one, `sha256_from`; and whether the executable is the tested
  build), CPU brand, caches with their processor masks, CPU sets
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
  and `units` (unit_threshold: state, native 60, requested, applied);
  `unit_lines` at start. Logs before v0.16.0 also hold `spin`,
  `worker_spin`, `spin_patch`, `steam_running` and `running_patch` (the two
  switches removed then, below).
- `profile` (only while toggle 1 is on): whole stacks per sampled thread with
  counts; the game and present threads are always sampled. From v0.6 also
  `leaves`: the game exe's hottest exact instructions ([rva, game thread,
  all]), for `profile_report.py --leaves N`. From v0.15.1, with control.ini
  `wc_scan=1`: each sample's general registers are checked against the
  process's committed write-combined or uncached memory (read-only
  VirtualQuery walk every 2 s; GPU upload heaps, where every CPU read
  bypasses the caches, as in the effect loop). Such samples carry flag 8 on
  their stacks; `wc` holds the totals (samples scanned and flagged, regions,
  bytes, the walk's time, modules) and `wc_leaves` the game instructions
  ([rva, flagged, flagged on the game thread, register mask, all]), for
  `profile_report.py --wc N`. A flagged instruction is a candidate: the
  disassembly says whether it reads that memory or only writes it.
- `contact_guard` (from v0.17.0): the seven signatures of the contact
  guard at PRE_MAIN (counts, RVAs); `guard_hooks` when its detours are
  registered (once, the first time parallel_contacts is on). In each window
  and status line, `contact_guard`: ready (every hook active), locking (the
  contact flag is set), calls per detour while locking, acquired (outermost
  lock takes), contended, unknown_jobs (jobs whose callback is none of the
  four known, run whole under the lock; unknown_rva the last one),
  overflows (record writer calls that found its 128 records full) and
  fullest; in research mode also wait_us, hold_us and overlaps per detour
  (a second thread for the same object while the first was in: the races
  the lock prevented). control.ini `contacts_guard=0` (research only) runs
  the parallel pass without the lock, as v0.16 did, to reproduce the race;
  the window then says `"bypass":true`.
- `multi_hit` (from v0.18.0): the move signature at PRE_MAIN (count, move and
  global RVAs); `multi_hook` when the detour goes in. In each window and
  status line, `multi_hit`: carry (toggle 65536), counting (research runs),
  calls (moves seen), fresh (new timed cache entries: multi-hit ticks),
  expired (entries the game cleared by timer), continued (a fresh entry the
  frame after its attack's expiry), interval_ratio (the previous entry's
  age at expiry over the new one's timer: 1.0 at 60 fps; for Pierce Lv3
  1.111 at 144 fps without the carry), carried (overshoots given back).
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

## The switches (v0.6 to v0.12)

| Toggle | Name | What it changes | Parameters |
|---|---|---|---|
| 1 | sample_profile | research: the stack sampler | |
| 2 | job_thread_cap | threads per job round | job_threads |
| 4 | time_jobs | research: times every job (executeJob detour) | |
| 8 | order_jobs | the camera round's job order | job_order (2), order_us (20) |
| 16 | plain_prim_stores | sPrimitive's record loop stores without reading (al.patch, 56 bytes) | |
| 32 | reuse_server_time | Steam's server time read once per time_ms (two detours) | time_ms (1000) |
| 64 | inline_rounds | rounds with little work run on the game thread alone | inline_us (25) |
| 128 | spin_workers | removed in v0.16.0. Job threads spun after a round and took the next round's places without being woken (al.patch, 11 bytes, and a jump stub); slower in game (al-13) | spin_us (300), gone with it |
| 256 | unit_threshold | sMhUnit's threshold between running a unit line alone and on the job threads (al-15: +2.8% at 40 us) | unit_us (40; the game's is 60) |
| 512 | cache_steam_running | removed in v0.16.0. SteamAPI_IsSteamRunning answered once per time_ms at its per-frame call sites (al.patch, 6 bytes each, and a jump stub): two in v0.7 (al-15: no gain measurable in fps), three from v0.12 (the network session driver's too; al-22: the three were about 1.5% of the game thread in a fight) | time_ms (1000) |
| 2048 | cache_ccd | the process confined to the processors sharing the largest L3 (the 3D V-Cache die of a 7950X3D/9950X3D) and job rounds sized to match; nothing on CPUs with one or equal L3s (al-17: +4.9% on CCD0) | |
| 1024 | smaller_cpu | research: the process confined to its first cpu_count logical processors (SMT siblings adjacent: 12 = six cores) and job rounds to as many threads; the original affinity back when off and at exit | cpu_count (12) |
| 4096 | budget | research: the game thread's frame split into limiter, job rounds, Present path (GPU wait, swapchain call) and serial work; the CPU frame (interval minus limiter and GPU wait). Two detours: sMain's frame timing and cD3DRender::present | |
| 8192 | uncap | research: the engine's frame cap (sMain mMaxFps) set to max_fps on the game thread, the game's value back when off and at exit; hands off if the options change it | max_fps (1000) |
| 16384 | sleep_limiter | the frame limiter waits on a high-resolution timer until margin_us before its deadline instead of spinning; the game's function still does its own timing (the frame-timing detour). al-22: game thread -38% (margin 500, -0.3% fps at the cap) or -21% (1000, no loss) at a 300 cap | margin_us (1000) |
| 32768 | parallel_contacts | sCollision's contact pass (contacts bit 0) and setup pass (bit 1) as job rounds: the collider flags the engine sets itself with more than one job thread, left at 0 because its setup runs before the job threads exist; set at al.frame BEGIN, put back when off and at exit. al-23 (Velkhana): the contact round waits 45 us for 183 us of work the game thread did alone, about 140 us a frame back. From v0.17.0 only with the contact guard's seven hooks active (contact_guard.hpp: the pass's callbacks under one lock; v0.16 crashed on hybrid Intel CPUs without it); the guard's hooks stay in once registered and pass through while the flag is off | contacts (1; al-23: the setup pass adds nothing; the setup pass is not guarded, research only) |
| 65536 | multi_hit_timing | from v0.18.0: multi-hit attacks tick on the 60 fps schedule at any frame rate. A detour on cpObjCollision::move finds the multi-hit cache entries the game clears when their timer runs out and gives the time past zero back to the same attack's next entry on that object, the next frame only (multi_hit.hpp). The detour goes in once in every research run (its counters) and when the switch is on | |

Toggles add up: 8 + 16 + 32 + 64 = 120 is every optimization switch of
v0.6; 376 adds unit_threshold, the best measured set (al-15). A
switch whose code is not found, or that Astera Loader refuses, stays off and
says why in the log; the rest still apply.

## Player mode (v0.13 and later)

Without the sidecar the plugin runs as players get it:

- `cpuopt.ini` next to the DLL holds the settings (made with the defaults
  when missing; edits are picked up within a second), `cpuopt-session.log`
  the log (`cpuopt.log` before v0.14.2; new each launch, at most 1 MB),
  `cpuopt-benchmark.txt` every benchmark's report (appended).
- `run.py prepare <name> --player` installs the DLL without a sidecar and
  keeps the slot's `cpuopt.ini` as `cpuopt.ini.before`; after the game has
  closed, `run.py collect <name>` copies the log, the ini and the benchmark
  report into the run folder, then `seal`.
- The log: `start` (as in research mode), the discoveries, `control` for
  every change (marker `player`, `benchmark-on`, `benchmark-off`),
  `settings_saved`/`settings_read`, `status` 10 s after a change and every
  5 minutes (switch states, failures), `modules` with it when they changed.
- The benchmark (Mods page, from v0.14.0): `bench` (armed, start, paused,
  resumed, stopped), `bench_window` for every window as it ends (from
  v0.14.3: frames, frame times, the game thread's CPU time from its cycle
  count and busy share, frame_budget's parts, all threads' CPU time) and
  `benchmark` with the result (`stopped` names why a run ended early; its
  complete pairs are reported from two on).
- With `load-pix-capturer.txt` in the plugin folder (v0.14.4, research
  only, see `AGENTS.md`): `pix_capturer` with `loaded` 1 or 0.
