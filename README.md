# MHW cpuopt

Research into why Monster Hunter: World (PC, DirectX 12) is CPU-limited, and
what can be recovered safely. On most PCs the CPU, not the graphics card,
sets MHW's frame rate, which is also why DLSS alone adds little there.

**Status:** research builds, not a release. Every switch is off by default.
The first one that changes the game, the camera draw round's job order, gave
+4.8% at the hub (244.5 to 256.2 fps, measured on and off in one session).
Measurement comes first: no game code is changed until a profile shows where
the time goes, and every change is a runtime switch measured on and off in
the same scene.

## What has been found so far

Hub start, standing still, an RTX 5090 with a Ryzen 9 9950X3D (details and
every number in `docs/research/engine-notes.md`):

- The game thread runs the game update and the Present, and is about 94%
  busy; the GPU is not the limit (about 0.22 ms inside Present).
- About 64% of the game thread's time is inside `sMain::executeJob`, the
  engine's fork-join step: about 20% running jobs itself and about 39%
  spinning, waiting for its job threads.
- The game starts 30 job threads on a 32-thread CPU (from GetSystemInfo,
  whatever the process affinity); they wait for work about 92% of the time.
  Capping the threads per round did not speed up frames.
- What the game thread waits for is one or two late, expensive jobs per
  round. In the camera draw rounds the job that ends last sits on average 94%
  of the way down the job list; the most expensive jobs are effects
  (`uMhEffect`). Perfect ordering of the same jobs would save about 0.3 ms per
  frame there, about 0.6 ms per frame over all rounds.
- Reordering that round so the expensive jobs start first (switch 8, two
  modes: reversed, or learned per object class) saved 0.19 ms per frame at
  the hub: +4.8%, every reordered step faster than every step in the game's
  order, no visual change.

## How it works

`cpuopt.dll` is an [Astera Loader](https://github.com/wowo-dot/AsteraLoader)
plugin (API 1.8). It measures:

- exact game frame and Present timing from Astera Loader's callbacks;
- CPU time per thread and how busy each core is;
- an optional stack sampler, with a read-only probe of the engine's job
  rounds;
- optional timing of every job of every job round (a detour on
  `sMain::executeJob`, installed only when switched on).

Every code site is found by signature and checked to match exactly once; if
anything does not match, that feature stays off and the game runs untouched.

## Layout

- `src/core/`: pure logic (signatures, histograms, control file, JSON), unit
  tested.
- `src/plugin/`: the plugin.
- `third_party/astera_loader/`: the plugin API header (MIT), pinned.
- `tests/`: `core_tests`, `plugin_tests`, and `al_host_smoke`, which runs the
  built DLL under a stand-in Astera Loader with a fake game.
- `tools/`: run folders and control (`run.py`), reports (`report.py`,
  `profile_report.py`, `job_report.py`), IDA name export (`ida_names.py`).
- `docs/runtime-protocol.md`: how an in-game measurement is done.
- `docs/research/`: findings, with confirmed results kept apart from
  hypotheses.

## Build

Windows, Visual Studio 2026 (x64), CMake 3.24 or newer:

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

## What this repository does not contain

No game files, executables, decoded images, IDA databases, game assets or
logs: those stay on the developer's machine. Research notes name functions
by address in the tested executable (MHW 421810, SHA-256
`c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`) and use
names from Fexty's annotated database where they exist.

`AGENTS.md` holds the project's working rules for its AI coding agents,
including the developer's machine-local paths.

## Licence

MIT (`LICENSE`). The Astera Loader header in `third_party/astera_loader` is
MIT as well.
