# cpuopt: CPU optimizations for Monster Hunter: World

Monster Hunter: World on PC is held back by its CPU code, not by the
graphics card: on most PCs one game thread sets the frame rate, which is why
upscaling alone does little for it. cpuopt is an
[Astera Loader](https://github.com/wowo-dot/AsteraLoader) plugin that fixes
the worst of that in memory while the game runs. It changes no game files.

On the test PC (Ryzen 9 9950X3D, RTX 5090, 1440p, the
[DLSS mod](https://github.com/wowo-dot/mhw-dlss-update) at quality):

| Scene | Without cpuopt | With cpuopt | |
|---|---|---|---|
| Ancient Forest, standing at a camp | 147 fps | 265 fps | +80% |
| The same, with a Process Lasso rule already keeping MHW on the cache die | 182 fps | 269 fps | +48% |
| Arch-Tempered Velkhana fight | 175 fps | 286 fps | +63%, then the graphics card is the limit (97% busy) |

The game thread's CPU time per frame falls by 30 to 45%, and the worst frames
improve with it (the forest's 99th percentile frame time: 16.7 to 4.4 ms
with the Lasso rule). These are one PC's numbers. An earlier version gained
as much with the same CPU limited to six or four cores (+33-34%), but other
CPUs have not been measured yet: that is what the beta is for.

### Will my frame rate go up?

Only where the CPU is what holds it back: a fast graphics card (an RTX
4090, 5080 or 5090 at 1440p, for example) or a lower resolution, heavy
scenes and fights, high refresh rates. If the graphics card is
already fully busy, cpuopt cannot raise the frame rate; it still lowers the
CPU time every frame needs, which keeps frames steadier where effects pile
up and leaves room for a faster graphics card later. With the DLSS mod's
Reflex on, a shorter CPU frame most likely means lower input latency too.
The benchmark (below) measures both and says which one held your frame
rate.

**Status:** closed beta (0.14). Tested with MHW 421810, the final version.

## What it changes

Each optimization is a switch on its own page in the game's Mods menu (with
[CMFM](https://github.com/wowo-dot/CMFM)) or in `cpuopt.ini`; all of them
are on by default except the experimental one.

| Switch | What it does | Measured |
|---|---|---|
| Effect drawing | Effects (fire, ice, sparks, dust) build their vertex data in memory the graphics card reads from. That memory is very slow to read from the CPU, and the game's loop read every record back eight times without needing to. cpuopt rewrites 56 bytes of that loop so it only writes. | the biggest gain: effect drawing about ten times cheaper, heavy effect moments (Fatalis's nova) no longer drop to 60 fps |
| Draw job order | Each frame the engine hands drawing work to its job threads in a fixed order, and the expensive jobs often came last, so every thread waited for them. cpuopt starts the slowest jobs first (learned per object type). | +4.8% at the hub |
| Steam server time | The game asked Steam for the server time several times a frame, each time waiting on a lock inside Steam. cpuopt asks once a second. | +3% at the hub |
| Update threads | The engine runs a batch of object updates on its job threads only above 60 microseconds of work; at 40, more of it runs in parallel. | +2.8% in the Ancient Forest |
| Stay on the cache die | On CPUs with two dies where one has 3D V-Cache (7900X3D, 7950X3D, 9900X3D, 9950X3D), the game runs on the cache die only, with its job threads sized to it. Greyed out on other CPUs. | +40% where Windows spread the game over both dies |
| Parallel hit detection | The engine has a parallel version of its collision pass but never turns it on, because it decides before its job threads exist. cpuopt sets the engine's own flag after they do. | about 0.14 ms a frame back in fights |
| Sleeping frame limiter | With a frame rate limit set, the game keeps a core busy while it waits for the next frame. cpuopt sleeps on a precise timer until 1 ms before, then lets the game finish the wait. | same frame pacing with about a fifth less game-thread CPU time at a 300 fps limit |
| Remove 300 fps limit (experimental, off) | The engine stops at 300 fps even with the frame rate set to No Limit. This raises that to 1000. A lower limit you set in the options stays. | for fast PCs and measuring |

Every change is made in memory at code that cpuopt finds by its bytes and
checks to match exactly once. If anything does not match (another game
version, another mod changing the same code), that switch stays off, is
greyed out on the page with the reason, and the game runs that part as it
always did. Everything goes back when a switch is turned off and when the
game exits. No gameplay values are changed: hit detection runs the game's
own code, only on more threads.

## Install

You need:

- Monster Hunter: World on Steam, version 421810 (the current one).
- [Astera Loader](https://github.com/wowo-dot/AsteraLoader) 0.2.2 or newer
  (on Linux with Proton, 0.2.3 or newer for the loader itself; cpuopt has
  not been tried there yet).
- Recommended: [CMFM](https://github.com/wowo-dot/CMFM) 0.1.0 or newer, the
  in-game Mods menu. Without it, settings are in `cpuopt.ini`.

Then put `cpuopt.dll` here, or install the zip as a mod in Mod Organizer 2
(it has this layout):

```
<game folder>\AsteraLoader\plugins\cpuopt\cpuopt.dll
```

Start the game. `cpuopt.ini` (the settings) and `cpuopt-session.log` (what cpuopt
found and did, new each start) appear next to the DLL. In the Mods menu
(below Options on the title screen, or in the Start menu's System tab), the
**CPU optimizations** page should say "all 7 optimizations on" ("all 6" on
CPUs without a cache die).

To uninstall, delete the `cpuopt` folder or turn the mod off in Mod
Organizer 2. Nothing else on the PC or in the game is changed.

## Using it

- **All optimizations:** everything at once. Give it a hotkey on the page
  (the row below it) to compare with and without cpuopt while you play.
- **The switches:** each optimization on its own, applied at once.
- **Status:** the frame rate and how many optimizations run; if one could
  not start, it says so and `cpuopt-session.log` says why.
- `cpuopt.ini` holds the same settings with a comment on each line. Edits
  made while the game runs are picked up within a second.

### Benchmark

The page's **Run benchmark** measures your PC with and without the
optimizations in the spot where you stand:

1. Go somewhere busy and stand still: a camp in the Ancient Forest, or the
   middle of Astera or Seliana. Keep the camera still too.
2. Mods menu, CPU optimizations, **Run benchmark**, Yes. Close all menus.
3. It starts 10 seconds later and takes under two minutes: 8
   pairs of 6 seconds, the optimizations off and on in turns. Opening a
   menu or switching to another window pauses it; that part is measured
   again. A sound plays when it is done.
4. The result shows on the page, and the full report is added to
   `cpuopt-benchmark.txt` next to the DLL: frame rate, frame times, CPU time
   per frame, your CPU, graphics card and game settings. It contains no
   file paths or user names.

The frame rate only shows the gain where the CPU is the limit. With a frame
rate limit, V-Sync, or a graphics card that is already fully busy, look at
the CPU time per frame: the report says when that is the case.

### Tips

- **Dual-die X3D CPUs:** with "Stay on the cache die" on, MHW needs no
  Process Lasso rule or Game Bar setting. On the test PC, Windows Game Mode
  with AMD's chipset driver cost about 10% on top: it parks the other die
  for the focused game, so everything else running also crowds onto the
  cache die. If you get less than expected, try with Game Mode off.
- **CPU affinity tools:** if a Process Lasso rule or another tool puts MHW on
  particular cores, either point it at the cache die or turn "Stay on the
  cache die" off, so the two do not fight.
- **Frame rate limit:** with a limit set (60, 120, 144...), the frame rate
  will not rise, but the game then needs less CPU per frame: cooler, quieter
  and steadier.

## Reporting a problem

Say what happened and attach `cpuopt-session.log` from that session (it is replaced
at each start) and `cpuopt-benchmark.txt` if you ran one. The log contains
the paths of the game and mod folders, which can include your Windows user
name. Beta testers: see `docs/beta-testing.md`.

## How it was found

Every switch started as a measurement: a stack sampler and per-job timing
inside the running game found where the game thread spends its time, and
each change was measured on and off in the same scene before it was kept.
The game thread was about 94% busy; most of that was waiting for the job
threads to finish one or two late, expensive jobs per round, and the rest
was a handful of slow loops and calls. The details, with the numbers and the
functions involved, are in `docs/research/engine-notes.md`; what is kept,
rejected and planned is in `docs/roadmap.md`.

cpuopt also has a research mode (a run folder named by a sidecar file):
per-frame timing, CPU per thread, a stack sampler and every switch
controlled live from a file. `docs/runtime-protocol.md` describes it.

## Building

Windows, Visual Studio 2026 (x64), CMake 3.24 or newer:

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

- `src/core/`: pure logic (signatures, histograms, settings, the benchmark's
  summary), unit tested.
- `src/plugin/`: the plugin.
- `third_party/`: the Astera Loader and CMFM interface headers (MIT), pinned.
- `tests/`: `core_tests`, `plugin_tests`, and `al_host_smoke`, which runs the
  built DLL under a stand-in Astera Loader and CMFM with a fake game.
- `tools/`: research runs and reports.

No game files, executables, decoded images, databases or logs are in this
repository. Research notes name functions by address in MHW 421810
(SHA-256 `c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`),
with names from Fexty's annotated database where they exist. `AGENTS.md`
holds the working rules for the project's AI coding agents.

## Licence

MIT (`LICENSE`). The Astera Loader and CMFM headers in `third_party` are MIT
as well.
