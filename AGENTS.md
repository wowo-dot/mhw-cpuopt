# Monster Hunter: World CPU Cost Research

## Objective

This repository investigates why the Windows PC version of Monster Hunter:
World (DirectX 12) is CPU-limited, and reduces that cost where it is safe and
measurable. Measurement comes first: no game code is changed until a profile
proves where the time goes. Every change must be switchable at runtime and
measured against the same scene with the switch on and off.

The sibling project `D:\mh world modding\dlssupdate` (DLSS, Reflex and frame
generation mod) is the source of most engine knowledge and of reusable tools.
Start with `docs/handoff-20261008.md`.

## Canonical Local Paths

- Repository: `D:\mh world modding\cpuopt`
- Sibling DLSS project (read-only reference from here): `D:\mh world modding\dlssupdate`
- Game directory: `D:\SteamLibrary\steamapps\common\Monster Hunter World`
- Game executable: `D:\SteamLibrary\steamapps\common\Monster Hunter World\MonsterHunterWorld.exe`
- IDA directory: `C:\Program Files\IDA Professional 9.3`
- IDA executable: `C:\Program Files\IDA Professional 9.3\ida.exe`
- Private decoded audit image (never commit or copy into the repository):
  `D:\mh world modding\ai\glavenus\MonsterHunterWorld_decrypted.exe`
- Mod Organizer 2 (portable): `C:\Modding\MO2\ModOrganizer.exe`, data directory `D:\MO2\MHW`
- Python 3.10: `C:\Users\akifs\AppData\Local\Programs\Python\Python310\python.exe` (run with `-B`)
- CMake/CTest (Visual Studio 18 2026, x64):
  `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`

These paths are machine-local configuration. Do not encode them into
distributable binaries or assume other contributors use the same paths.

## Safety and Scope

- Treat the game directory as read-only unless the user explicitly requests an
  installation or runtime test. Install test DLLs through an MO2 mod folder,
  never into the game directory.
- Never overwrite or patch `MonsterHunterWorld.exe` or original game assets on
  disk. In-memory hooks and patches are allowed only at signature-verified
  sites and must restore cleanly.
- Do not disable, bypass or weaken integrity checks, CRC/anti-tamper code, DRM,
  anti-cheat, licensing or platform protections. Their CPU cost may be
  measured and documented; it is not a fix target. (An old third-party mod
  that removes the game's CRC checks exists; the user considers it outdated,
  it is off in every MO2 profile, and it is out of scope here.)
- Do not run experimental builds in public multiplayer. Prefer offline mode.
- Put builds, logs, profiles, captures, exports and analysis artifacts under
  this repository (`local/` for anything private), never in the game directory.
- Do not commit or redistribute Capcom binaries, decoded images, game assets,
  shader bytecode, NVIDIA runtime binaries, crash dumps or IDA databases.
- Never send project data, logs, profiles or binaries to external services.

## Runtime Testing Protocol

- The user launches the game, always through MO2. Never launch MHW through
  Steam, a script or computer-use.
- Before every launch, prepare a fresh run folder (`local/runs/<name>`) and the
  installed DLL; after the game closes, seal the folder with a hash manifest.
- Keep the installed test DLL in place between runs; announce any intentional
  stop or removal.
- Every run log must identify the executable SHA-256, the DLL SHA-256 and
  version, the CPU model, the scene, the graphics settings and which other
  DLLs/mods are loaded (DLSS mod on or off, other plugins).
- External tools that read the game process must be strictly read-only:
  `PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ`, no writes, no thread
  suspension, no remote threads or allocations.
- When screenshotting the desktop, crop to the game window.

## Reverse-Engineering Workflow

1. Record the executable version and SHA-256 before deriving signatures or offsets
   (tested: `c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`).
2. Prefer signatures and structural discovery over hard-coded absolute addresses.
3. Keep confirmed observations separate from hypotheses in research notes.
4. For every hook or patch, document the target function, calling convention,
   threading and ownership assumptions, and the tested executable hash.
5. Attribute CPU time to functions with a profile before proposing any change.
   A hotspot claim needs sample counts, the scene and the thread.

## Implementation Conventions

- C++20 and CMake for native components; 64-bit Windows only.
- RAII, smart COM pointers, bounded logging and clean hook removal.
- Every optimization is a named runtime switch (the sibling project's
  `src/perf/toggles.hpp` pattern), off by default until measured.
- Fail closed: if a signature, invariant or expected structure is missing,
  leave the native code path untouched and log why.
- Coexist with the DLSS mod: it hooks Direct3D 12 command-list, queue and
  swap-chain methods and several engine functions with inline detours. Do not
  hook the same functions unless unavoidable; if you must, chain correctly and
  test with the DLSS mod both on and off.
- Keep third-party dependencies explicit and pinned; record their licenses.

## Verification

- Build with warnings enabled and treat new warnings in project code as defects.
- Unit-test pure logic (signature parsing, profile aggregation, configuration)
  outside the game process.
- Performance claims need fixed scenes, several windows per configuration,
  frame-time percentiles (p50/p95/p99) and the A/B switch state in the log.
  A single fps reading is not a result.
- Check stability across loading screens, cutscenes, menus, alt-tab and quest
  transitions for every patch.

## Git Hygiene

- Git is the source of truth: focused commits, an annotated tag per version the
  user has tested in game.
- Commit locally; do not push without the user saying so.
- Keep build trees, logs, profiles, dumps, game files, SDK binaries and IDA
  databases untracked.
- Do not rewrite or discard user changes without explicit approval.

## Working With the User

- The user grants broad autonomy: decide, act and report; they object when
  they disagree. Present plans briefly and proceed on "go ahead".
- Report measured counters and numbers, not impressions, and say plainly what
  is unverified.
- Use plain language in user-facing summaries.
