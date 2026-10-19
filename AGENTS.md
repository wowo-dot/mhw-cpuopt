# Monster Hunter: World CPU Cost Research

## Objective

This repository investigates why the Windows PC version of Monster Hunter:
World (DirectX 12) is CPU-limited, and reduces that cost where it is safe and
measurable. Measurement comes first: no game code is changed until a profile
proves where the time goes. Every change must be switchable at runtime and
measured against the same scene with the switch on and off.

The sibling DLSS, Reflex and frame generation mod is the source of most
engine knowledge and of reusable tools.

## Local Paths

Machine-local paths (this repository, the sibling DLSS project, the game
directory and executable, IDA, the clean decoded game image, the reversing
tools, Mod Organizer 2, Python and CMake) are in `local/paths.md`, which is
not tracked. Read it before working. Never write them into tracked files or
distributable binaries, and do not assume other contributors use the same
paths.

- The clean decoded game image (its path in `local/paths.md`) is Capcom
  code: never commit or redistribute it. It is a read-only memory copy of
  the tested exe at the title screen (plain Steam start, no mods); the exe
  on disk has SteamStub-encrypted code, so offline signature checks run on
  this image. Read it in place, never write into the folder that holds it.
  SHA-256 `a78ea69b0566414a42f16d0f8ff7d729e095041e2ae76a34cc02dbe0939f1d80`.
- Private IDA database: `local/ida/MonsterHunterWorld_C2EBBBD2_clean.i64`,
  built from `local/ida/MonsterHunterWorld_C2EBBBD2_clean.exe` (a copy of
  the clean image, same SHA-256) with `tools/ida_build.py`, plus Fexty's
  names (`docs/research/ida-database.md`).
- Do not use the old decoded image with SHA-256 `d7861173…884c`: it was
  dumped with mods loaded and differs from the original code in 29 places.
  The old database `local/ida/MonsterHunterWorld_C2EBBBD2.i64` is retired
  with it.

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
  suspension, no remote threads or allocations (x64dbg and Frida: see
  Reverse-Engineering Tools).
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

## Reverse-Engineering Tools

Claude Code has an IDA MCP (Hex-Rays), an x64dbg MCP, a Frida MCP and
`sigscan.py`; the user-level `reversing` skill describes them. This file's
rules come first.

- **IDA:** open only `local\ida\MonsterHunterWorld_C2EBBBD2_clean.i64`, never
  the DLSS project's or CMFM's databases or anything in `ai\glavenus`. Open a
  fresh binary with `-Opdb:off` (no symbol-server request). Ask through the MCP's
  `execute_python` instead of writing one-off headless scripts. Write-back
  and exports stay under `local\`.
- **Names for profiles:** `tools/ida_names.py` runs inside IDA and writes
  `local\ida\names.tsv` (`rva<TAB>name`, chunks as `name.part`) plus
  `names.meta.json` (source database, exe SHA-256, counts).
  `tools/profile_report.py` picks it up automatically.
  - Run it with the IDA MCP: `execute_python` on the script text plus
    `export(r'<repo>\local\ida\names.tsv')`. Or use IDA's File > Script file.
  - The 2026-10-11 export (clean database with Fexty's names) has 53,197
    named functions (53,229 lines). When a hotspot is understood, name it in
    the private database, save, and re-export.
- **Signatures** (`sigscan.py` from the reversing tools in `local/paths.md`,
  on the clean image):
  - `audit <image> src` before staging a build that changes a pattern. A
    searched pattern must match exactly once; the frame-clock signature
    matched once, at `0x23922C2`, on the clean image on 2026-10-11.
  - `make <image> <VA>` and `scan <image> "<pattern>"` find and check patterns
    for new sites.
- **x64dbg and Frida on the running game** are, with PIX below, the only
  exceptions to the read-only rule in the Runtime Testing Protocol:
  - the user launches through MO2 as always and says OK in the session before
    each attach; never attach during startup (SteamStub);
  - offline only, as for experimental builds;
  - prefer x64dbg hardware breakpoints and non-stopping logging breakpoints:
    they change no code bytes, and the game re-checks its code with CRC;
  - Frida hooks only functions this project, the DLSS mod or Astera Loader
    already hooks. Software breakpoints, memory writes, other hooks and calls
    into game code each need their own OK;
  - attaching changes timing, so never measure while attached and never mix
    attached windows into a profile;
  - detach when done (x64dbg `detach`, never `terminate_session`) and note
    what was attached, when and why in the run folder.
- **PIX GPU captures** (Microsoft PIX, installed by the user; from v0.14.4):
  - cpuopt loads PIX's capture layer (`WinPixGpuCapturer.dll`) at startup
    only while `<plugin folder>\load-pix-capturer.txt` exists. Make that file
    only with the user's OK, for a session or for a stretch of work they
    agree to (the GPU research from 2026-10-17), and remove it when that
    ends or they ask; take it out for any launch that measures. It never
    goes into a package;
  - the user launches through MO2 as always and PIX attaches to the running
    game; never launch the game from PIX. Offline only;
  - captures hold the game's shaders, textures and meshes: they stay under
    `local/` and are never committed, shared or sent anywhere;
  - the layer changes timing: benchmark and measure without it loaded.
- Point the tools at the game's code only, never at NVIDIA binaries or other
  mods' DLLs.

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
