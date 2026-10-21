# Changelog

## 0.18.0 (test build): multi-hit timing as at 60 fps

- New, on by default: **Multi-hit timing as at 60 fps** (Mods page, Fixes;
  `multi_hit_timing` in cpuopt.ini). Multi-hit attacks (pierce and element
  ammo, and more) lose ticks at frame rates other than 60: the game counts
  each attack's tick interval in 60 fps frames but lets it hit again only on
  a frame and throws the time past zero away, so every tick waits for the
  next whole frame (Moonbunnie's "Effects of FPS" research measured this
  first). cpuopt gives that time back to the same attack's next tick. Measured
  on the test PC with Pierce Ammo Lv3 on the training dummy: at 144 fps the
  tick interval was 11% too long without the fix and on schedule with it;
  uncapped (about 260 fps) 3.7% too long without. At 60 fps nothing changes.
  It works both ways: monsters' multi-hit attacks on you also tick as at
  60 fps. It is not under "All optimizations", so the on/off hotkey and the
  benchmark compare speed only.
- Parallel hit detection is greyed out, and stays off, if the hit-detection
  guard from 0.17.0 cannot be set up on this game version.
- Includes 0.17.0's Parallel hit detection fix and the Linux start-up fix.

## 0.17.0 (test build): Parallel hit detection made safe

- Fixed: with **Parallel hit detection** on, the game could crash in big
  fights (reported in the Arch-Tempered Velkhana fight on an Intel laptop
  CPU). The switch turns on the game's own multi-threaded hit detection,
  which the game never uses by itself, and the code that reacts to a hit
  was written for one thread: when many hits landed at once, two threads
  could write the same hit list, and a half-written entry crashed the game.
  The same kind of problem was found in the three other reactions that run
  there. cpuopt now runs every hit reaction one at a time, while the
  expensive part, finding the hits, stays on all threads. If cpuopt cannot
  set that up, Parallel hit detection stays off.
- The hit reactions wait for each other only briefly (a lock that spins
  before it sleeps), so the switch still gives back about 0.12 ms a frame in
  the Velkhana fight on the test PC.
- Includes the Linux start-up fix from 0.16.1 (below).

If you turned Parallel hit detection off because of the crash, this build is
meant to be tried with it on again.

## 0.16.1: Linux start-up fix

- Fixed: on Linux with Proton (or Wine), the game closed during start-up
  with cpuopt installed, before the title screen. cpuopt's first record in
  its log asked Windows for the game's default CPU sets through a function
  that Wine and Proton do not have, and Wine ends the game at the first
  call to a missing function. cpuopt now checks that the function exists
  before calling it. Windows is not affected.

Known:

- Not yet tested on Linux with Proton.

## 0.16.0: the first public release

For Monster Hunter: World 421810 (the final version) on Windows, with Astera
Loader 0.2.2 or newer and, for the in-game Mods page, CMFM 0.1.1 or newer.
Experimental: see the README for what it fixes and why the gain depends on
the CPU.

What it does:

- Seven switches, all on by default: effect drawing (the effect loop's
  reads of graphics memory), draw job order, Steam server time, update
  threads, stay on the cache die (dual-die X3D CPUs only), parallel hit
  detection, sleeping frame limiter. One experimental switch, off by
  default: remove the 300 fps limit.
- A "CPU optimizations" page in the game's Mods menu (CMFM), or
  `cpuopt.ini` next to the DLL; an on/off hotkey for everything.
- The page's live "Limited by" line: whether the CPU, the graphics card,
  V-Sync or a frame rate limit holds your frame rate right now.
- A built-in benchmark: eight pairs of windows with the optimizations off
  and on, in the spot where you stand; a report with frame rate, frame
  times and CPU time per frame in `cpuopt-benchmark.txt`.
- `cpuopt-session.log`, new each start: what cpuopt found and did.

Changes since the closed beta (0.14.1 to 0.15.0):

- Fixed: "Update threads" was not applied in normal play until a setting
  was changed, because its request came before the game had set up what it
  changes. Benchmarks were not affected: each benchmark window applies
  every switch again.
- Removed two research switches that were never offered on the Mods page
  (spinning job threads, a cache for Steam's "is Steam running" calls),
  together with the code and the memory they set up at every start. cpuopt
  now changes the game only through Astera Loader's hooks and byte
  patches.
- The game's file hash comes from Astera Loader instead of being read
  again, so the switches apply sooner on slow disks.
- The log's first line names the version, the mode and, under Wine or
  Proton, Wine's version.
- From 0.15.0: the "Limited by" line, and the benchmark's result line says
  when something besides the CPU held the frame rate.
- From 0.14.2 to 0.14.6: a benchmark under two minutes that pauses for menus
  and alt-tab and keeps the pairs measured when stopped early; reports that
  explain GPU-limited PCs; the log renamed `cpuopt-session.log` (Astera
  Loader's own `logs\plugins\cpuopt.log` is a different file).

Known:

- On Linux with Proton, the first try did not reach the title screen; that
  is being looked into.
- Measured so far on a Ryzen 9 9950X3D (two PCs) and three Intel CPUs:
  `docs/sending-results.md` says how to add yours.
