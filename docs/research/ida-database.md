# Private IDA database (clean image)

Rebuilt on 2026-10-11, after CMFM found that the old decoded image was
dumped with mods loaded (CMFM `docs/handoff-clean-image-20261011.md`).

## Input

- `local\ida\MonsterHunterWorld_C2EBBBD2_clean.exe`: a copy of CMFM's clean
  image `local\runs\20261011-clean-dump\dump\image.exe`, SHA-256
  `a78ea69b0566414a42f16d0f8ff7d729e095041e2ae76a34cc02dbe0939f1d80`
  (verified after copying). Tested exe `c2ebbbd2…90ea`, version 421810.
- Section headers point at the virtual layout, so file offset = RVA.

## Build

```
idat -A -Opdb:off -S"tools/ida_build.py" -L<log> local\ida\MonsterHunterWorld_C2EBBBD2_clean.exe
```

IDA 9.4, auto-analysis only; `-Opdb:off` so IDA never asks Microsoft's
symbol server for `World300.pdb`. The database file is renamed to
`MonsterHunterWorld_C2EBBBD2_clean.i64`.

## Names

Fexty's annotated Ghidra database of the same build (15.23, PDB
`World300.pdb` GUID `7c554b32-2b94-41f7-8e81-dc3593003b74`) via CMFM's
export `local\ghidra\export` and CMFM's
`tools/re/ida/import_ghidra_export.py` (both read in place), run through the
IDA MCP: `run(export_dir, tag="Fexty")`. Only unnamed places get a name;
comments are prefixed `[Fexty …]`.

Counts on 2026-10-11 (same as CMFM's own import): 100,804 names set, 1,071
places kept their IDA name, 81,113 automatic Ghidra labels skipped, 2,430
comments, 3 function starts IDA could not create, 9 names IDA refused.

Checked in the database after the build: input SHA-256 `a78ea69b…1d80`,
291,889 functions (287,568 from `.pdata`), the original bytes at all 29
places, and the frame-clock site bytes as on the old image.

The 2026-10-11 `names.tsv` export: 53,197 named functions, 53,229 lines
(chunks as `name.part`); `names.meta.json` records the database and input
SHA-256.

Then `tools/ida_names.py` re-exports `local\ida\names.tsv` for
`tools/profile_report.py`.

## The old database

`local\ida\MonsterHunterWorld_C2EBBBD2.i64` (a copy of the DLSS project's
static database, input recorded as the encrypted exe on disk) is retired. No
cpuopt work depended on it at the 29 modified places: none of the 106 names
exported from it on 2026-10-10 is on a function with a modified place, and
the frame-clock sites are byte-identical on both images.
