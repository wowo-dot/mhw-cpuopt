# Sending results from your PC

cpuopt's gain depends a lot on the CPU (see the README's "Results on
different PCs"). So far it has been measured on a Ryzen 9 9950X3D (two PCs)
and on three Intel CPUs. Results from other CPUs help most: non-X3D and
single-die X3D Ryzens (5800X, 5800X3D, 7800X3D, 9800X3D), six-core Ryzens,
CPU-limited Intel setups, older CPUs, Radeon graphics cards. `README.md`
explains what every switch does.

## Install

1. You need Astera Loader 0.2.2 or newer, and CMFM 0.1.1 or newer for the
   in-game menu (the benchmark needs CMFM). On Linux with Proton, cpuopt's
   first try did not reach the title screen; if you are on Linux, tell us
   how it goes.
2. Install the zip as a mod in Mod Organizer 2, or extract it into the game
   folder. Either way the DLL ends up at
   `<game folder>\AsteraLoader\plugins\cpuopt\cpuopt.dll`.
3. Start the game. In the Mods menu (below Options on the title screen, or
   in the Start menu's System tab) open **CPU optimizations**. The status
   line should say "all 6 optimizations on", or "all 7" on a 7900X3D,
   7950X3D, 9900X3D or 9950X3D. "Stay on the cache die" is greyed out on
   other CPUs; that is expected.

## What to do

1. **Before anything else:** take a screenshot of the CPU optimizations page.
2. **Benchmark, two spots.** For each spot:
   - Stand still and keep the camera still.
   - Run **Run benchmark** and answer Yes, then close every menu.
   - It runs under 2 minutes and plays a sound when done.
   - Do not touch the mouse or keyboard meanwhile; opening a menu or
     alt-tabbing pauses it, and that part is measured again.

   The spots:
   - the Ancient Forest, at the Southwest Camp (the first camp), looking out
     from the tent;
   - Elder's Recess, where effects are heaviest (or Astera or Seliana,
     somewhere with people around).

   Each result is added to `cpuopt-benchmark.txt` next to the DLL.
3. **Play.** A hunt or two with everything on. Set a hotkey on the page
   ("On/off hotkey") and flip it in a fight to feel the difference. Things to
   watch for:
   - hits that do not land or land oddly, monsters or the player passing
     through walls or each other;
   - stutter or hitches that are not there with the optimizations off;
   - crashes, freezes, or the game not closing.
4. **If something is off:** turn the switches off one at a time until it
   goes away and tell us which one did it.

## What to send

- `cpuopt-benchmark.txt` (no file paths or user names in it).
- `cpuopt-session.log` from the session where something happened, next to
  `cpuopt.dll` (Astera Loader's `logs\plugins\cpuopt.log` is a different,
  shorter one). It is replaced at every start, so copy it before you launch
  again. It contains the paths of your game and mod folders, which can
  include your Windows user name; check it before sending if that matters to
  you.
- The screenshot of the page.
- In a few words:
  - where you stood for each benchmark;
  - your resolution and upscaling (the DLSS mod and its mode, or none), and
    whether Reflex was on;
  - whether Windows Game Mode is on;
  - anything running alongside, such as a Discord stream, recording,
    overlays or Process Lasso;
  - anything odd you noticed, with or without cpuopt.

Open an issue on the project's GitHub page and attach the files there.

## Good to know

- The frame rate only shows the gain where the CPU is the limit. With a
  frame rate limit, V-Sync, or a fully busy graphics card, the benchmark's
  "CPU time per frame" shows it instead, and the report says so. The Mods
  page's "Limited by" line shows which one holds your frame rate.
- Reflex (the DLSS mod's) paces the game's frames itself, so with it on the
  frame rate shows less of the gain; for the clearest comparison, run one
  benchmark with Reflex off.
- Frame generation's extra frames are not counted; the benchmark measures
  the game's own frames.
- "Remove 300 fps limit" is experimental and off by default. Leave it off
  for the benchmark unless your PC runs into 300 fps.
- To remove cpuopt, delete the `AsteraLoader\plugins\cpuopt` folder (or turn
  the mod off in Mod Organizer 2). Nothing else is changed.
