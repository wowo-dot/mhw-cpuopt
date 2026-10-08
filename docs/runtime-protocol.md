# In-game run protocol

## Installation

- MO2 mod folder `cpuoptTEST` (path in `local/machine.json` → `slot`):
  `nativePC\plugins\mhw_cpuopt.dll` plus the sidecar
  `mhw_cpuopt.dll.output-dir` naming the current run folder. Stracker's
  Loader loads every `*.dll` (by extension) from `nativePC\plugins`; the
  sidecar is not loaded.
- Without the sidecar, or if the folder is not writable, the plugin does
  nothing.
- Profile `barebonesdlss` with `cpuoptTEST` enabled:
  - executable "Monster Hunter: World": game + Stracker's Loader (its
    MonsterLoader and QuestLoader plugins) + this plugin;
  - executable "Monster Hunter: World - DLSS 1.2.110 test": the same plus the
    DLSS mod (forced library).

## One run

1. `python -B tools/run.py prepare <name> --note "<scene, config>"` (game
   closed). Installs the DLL only if it changed, backs up the previous one,
   writes `run-receipt.json`.
2. The user launches through MO2 and goes to the scene.
3. Scene labels and switches while the game runs:
   `python -B tools/run.py control <name> --marker hub-start`,
   `--toggles 1` (stack sampler on), `--toggles 0`. Each change closes the
   current statistics window first, so no window mixes two states.
4. `python -B tools/run.py status <name>` shows the last windows live.
5. After the game has closed: `python -B tools/run.py seal <name>`.
6. Reports: `tools/report.py <name>`, `tools/profile_report.py <name>`.

## What the log holds (`cpuopt-<pid>.jsonl`)

- `start`: version and commit, executable and DLL SHA-256 (and whether the
  executable is the tested build), CPU brand, caches with their processor
  masks, CPU sets (core, last-level cache, parked), process affinity, OS
  build, `graphics_option.ini`, `config.ini` `[CPU]` and `[GRAPHICS]`.
- `modules`: every loaded module with base, size and SHA-256 (outside the
  Windows folder); logged again when the set changes.
- `frame_clock`: the frame-counter signature search (attempt, result,
  derived singleton RVA and counter offset, whether they equal the tested
  build's).
- `control`: switch and marker changes (or a rejected file).
- `window` (every 5 s by default): frames (exact counter advance), fps and
  mean frame time (exact), poll-timed frame interval percentiles (each
  interval carries the poll latency, `poll_gap_avg_us`), CPU time per thread
  (cycle counted), context switches, thread count, busy share of every logical
  processor (all processes).
- `profile` (only while toggle 1 is on): whole stacks per sampled thread with
  counts.
- `exit`: written from DllMain at process exit.

## Measurement rules

- A result is several windows per configuration in the same scene, with the
  switch state in the log; one fps reading is not a result.
- The sampler suspends the sampled threads about 1,000 times a second (about
  15 us each); frame times measured with it on are not baseline numbers.
- Record the scene, graphics settings and which mods are loaded in the run
  note; the log records what it can detect itself.
