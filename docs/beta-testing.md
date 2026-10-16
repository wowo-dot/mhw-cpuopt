# cpuopt closed beta: notes for testers

Thank you for testing. cpuopt cuts Monster Hunter: World's CPU cost; on the
developer's PC (Ryzen 9 9950X3D) it gains 48 to 80% fps where the CPU is the
limit. What nobody knows yet is how it does on other PCs: Intel CPUs,
single-die Ryzens, fewer cores, other graphics cards. That is what this beta
is for. `README.md` (next to this file) explains what every switch does.

This is a closed beta: please do not pass the files on.

## Install

1. You need Astera Loader 0.2.3 or newer, and CMFM 0.1.0 or newer for the
   in-game menu (the benchmark needs CMFM).
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
   - Astera or Seliana, somewhere with people around.

   Each result is added to `cpuopt-benchmark.txt` next to the DLL.
3. **Play.** A hunt or two with everything on. Set a hotkey on the page
   ("On/off hotkey") and flip it in a fight to feel the difference. Things to
   watch for:
   - hits that do not land or land oddly, monsters or the player passing
     through walls or each other ("Parallel hit detection" is the newest
     switch);
   - stutter or hitches that are not there with the optimizations off;
   - crashes, freezes, or the game not closing.
4. **If something is off:** turn the switches off one at a time until it
   goes away and tell us which one did it.

During the beta, please play solo or in private sessions rather than public
lobbies: if something is wrong, better to find it in your own hunt.

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
  - your resolution and upscaling (the DLSS mod and its mode, or none);
  - whether Windows Game Mode is on;
  - anything running alongside, such as a Discord stream, recording,
    overlays or Process Lasso;
  - anything odd you noticed, with or without cpuopt.

## Good to know

- The frame rate only shows the gain where the CPU is the limit. With a
  frame rate limit, V-Sync, or a fully busy graphics card, the benchmark's
  "CPU time per frame" shows it instead, and the report says so.
- Frame generation's extra frames are not counted; the benchmark measures
  the game's own frames.
- "Remove 300 fps limit" is experimental and off by default. Leave it off
  for the benchmark unless your PC runs into 300 fps.
- To remove cpuopt, delete the `AsteraLoader\plugins\cpuopt` folder (or turn
  the mod off in Mod Organizer 2). Nothing else is changed.
