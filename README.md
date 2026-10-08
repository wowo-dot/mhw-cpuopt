# MHW CPU cost research

Why Monster Hunter: World (PC, DirectX 12) is CPU-limited, and what can be
recovered safely. Measurement first: no game code is changed until a profile
shows where the time goes. Read `AGENTS.md` and `docs/handoff-20261008.md`.

## Layout

- `src/core/`: pure logic (signatures, interval histogram, control file, JSON),
  unit tested.
- `src/plugin/`: `mhw_cpuopt.dll`, a Stracker's Loader plugin. Version 0.1 only
  observes: start record, engine frame counter, per-thread CPU, optional stack
  sampler. No hooks, no writes to game memory.
- `tests/`: `core_tests`, `plugin_tests` (in-process parts against a stand-in),
  `runtime_smoke` (the built DLL in a stand-in process).
- `tools/`: `run.py` (prepare, control, status, seal), `report.py` (frame
  times and CPU per scene and switch state), `profile_report.py` (hotspots),
  `evidence.py` (hash manifests).
- `docs/runtime-protocol.md`: how an in-game run is done.

## Build

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

Machine-local paths (MO2 slot, tools) live in `local/machine.json`, which is
not tracked.
