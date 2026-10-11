# Engine notes for CPU work

Executable SHA-256 `c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`
(PE timestamp `0x66D13CB6`), checked 2026-10-08 against the game folder.

## Confirmed (static, decoded image)

First found on the old decoded image (`ai\glavenus`, SHA-256 `d7861173…884c`),
which was dumped with mods loaded and differs from the original code in 29
places. Re-checked on 2026-10-11 on the clean image (SHA-256
`a78ea69b…1d80`, CMFM `docs/handoff-clean-image-20261011.md`): the three
code sites below have the same bytes on both images, their functions
(0x239223A, 0x2293280, 0x2290880) are byte-identical on both, and the
nearest of the 29 places is 0xC75E bytes from the signature. Nothing here
depended on modified bytes.

- Sections: `.text` 0x1000 (0x2E1102C bytes, encrypted on disk and decrypted
  at start by the Steam stub in `.bind`), `.rdata`, `.data` 0x3E78000,
  `.pdata` 0x5216000 (0x34A7C0 bytes: about 287,000 unwind entries, so most
  functions have boundaries for the sampler).
- Frame counter: the signature
  `48 8B 05 ?? ?? ?? ?? 49 B9 AB AA AA AA AA AA AA AA 4C 8B 80 ?? ?? ?? ??`
  (renderer pointer load, the multiply-by-1/3 constant for frame % 3, the
  counter load) matches once in `.text`, at 0x23922C2; its operands give the
  renderer singleton at 0x51C4480 and the counter at +0x1F210 (the DLSS
  project's values).
- The counter is incremented in one place: `inc qword [rbx+1F210h]` at
  0x22932DB, and written at 0x2290E78 (`mov [rsi+1F210h], rdi`), which is in
  `sRender::ctor` (Fexty's name, 0x2290880): the singleton at 0x51C4480 is
  the engine's `sRender`. The function containing 0x22932DB (0x2293280,
  unnamed, one caller 0x1AF2B60) runs once per frame; which thread runs it
  is not yet observed.

## Game configuration on the test PC (read only)

- `graphics_option.ini`: windowed 2560x1440, no frame limit, V-Sync off,
  manual preset, ResolutionScaling High, LODBias High, MaxLODLevel No Limit,
  volume rendering Highest, DLSS off, AA off.
- `config.ini` `[CPU]`: `NumCPUCore=32`, `NumProcessAffinity=0`,
  `RenderingThread=ON`. The user restricts the game to CCD0 (16 logical
  processors) with Process Lasso, so the game may size thread pools for 32
  logical processors while running on 16.

## Hypotheses to test (not results)

- H1: the render thread's submission is the limit (handoff: hub render
  submission 2.6-3.2 ms of a 4.8-5.3 ms frame with the DLSS mod). Test: render
  thread cycles per frame against frame time, vanilla.
- H2: worker pools sized for 32 logical processors on 16 cause extra context
  switching. Test: thread count and switches per frame; A/B
  `NumCPUCore=16` (a user-side setting, no code).
- H3: a few functions dominate the render thread. Test: sampler, hub and
  training grounds, with and without the DLSS mod.
