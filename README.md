# MHW cpuopt

Research into why Monster Hunter: World (PC, DirectX 12) is CPU-limited, and
what can be recovered safely. On most PCs the CPU, not the graphics card,
sets MHW's frame rate, which is also why DLSS alone adds little there.

**Status:** research builds, not a release. Every switch is off by default.
Measured at the hub, each on and off in one session:

- the camera draw round's job order: +4.8% (244.5 to 256.2 fps);
- on top of it, effect drawing writing its vertex records without reading
  them back from GPU upload memory: +10.1% (260.9 to 287.3 fps), and 20%
  less CPU time per frame for the whole game;
- reusing Steam's server time instead of asking Steam several times a frame:
  +3.0%;
- sMhUnit's threshold between running a unit line alone and on the job
  threads at 40 us instead of 60: +2.8% (Ancient Forest);
- on a dual-die X3D CPU, the game kept on the 3D V-Cache die: +3.6%.

All together in the Ancient Forest: 181 to 244 fps (+35%); in Elder's
Recess 160 to 265 (+66%); one title screen 115 to 296 (2.6x). On a CPU
limited to six or four cores the gain is the same (+33-34%).

With these on, the hub reaches the engine's 300 fps cap; heavier scenes come
next.
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
- The hottest game code outside the job wait was one loop in effect drawing
  (`sub_142570A40`): it fills vertex records in GPU upload memory, which is
  write-combined (uncached for reads), with eight read-modify-write steps
  per record whose results do not depend on what it reads. Switch 16
  rewrites 56 bytes of that loop to store the same values without reading
  (checked as machine code on random inputs): the effect draw jobs became
  about ten times cheaper.
- The game asks Steam for the server time several times a frame, and each
  call waits on a lock inside Steam (4.3% of the game thread). Switch 32
  reuses one answer for a second.
- Measured as CPU time per frame rather than fps (v0.9: the frame minus the
  frame limiter's wait and the GPU wait), all the kept switches together
  cut the CPU frame 29% in the Ancient Forest (5.78 to 4.10 ms), 32% at the
  hub and 22% at the title screen; light scenes are now often GPU-bound or
  at the engine's 300 fps cap.

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
