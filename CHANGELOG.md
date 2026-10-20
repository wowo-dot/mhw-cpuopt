# Changelog

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
