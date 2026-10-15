"""Export named game functions from an IDA database for profile_report.py.

Runs inside IDA (IDA's own Python), not with the project's Python 3.10:
  - IDA MCP:  execute_python(code=open(r"<repo>\\tools\\ida_names.py").read() + "\\nexport(r'<repo>\\local\\ida\\names.tsv')")
  - IDA GUI:  File > Script file... (writes <repo>/local/ida/names.tsv next to this script's repo)

Writes "rva<TAB>name" lines (hex RVA, sorted) for every function with a real
name: user, library or RTTI names, never IDA's dummy sub_/nullsub_ names.
Function chunks get their own line ("name.part"), because the stack sampler
attributes frames to .pdata entries and a chunk has its own entry. A
names.meta.json next to the output records the database, the input file's
SHA-256 and the counts, so a names file is never used against the wrong exe.
"""
import json
import re
import time
from pathlib import Path

import ida_bytes
import ida_funcs
import ida_kernwin
import ida_nalt
import idautils
import idc

DUMMY = re.compile(r"^(?:j_)*(?:sub|nullsub|unknown_libname)_")


def clean(name):
    if name.startswith("?"):
        name = idc.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES)) or name
    return re.sub(r"[\t\r\n]", " ", name)


def export(out_path, meta_path=None):
    base = ida_nalt.get_imagebase()
    out = {}
    functions = named = 0
    for ea in idautils.Functions():
        functions += 1
        name = ida_funcs.get_func_name(ea) or ""
        if not name or ida_bytes.has_dummy_name(ida_bytes.get_flags(ea)) or DUMMY.match(name):
            continue
        named += 1
        name = clean(name)
        out[ea - base] = name
        for start, _ in idautils.Chunks(ea):
            if start != ea:
                out.setdefault(start - base, name + ".part")
    # The exe's unwind table splits some functions into several entries, and
    # the sampler names frames by entry. An entry inside a function gets that
    # function's name and offset ("sMain::executeJob+0x1b0"); inside an
    # unnamed function, its sub_ name, which is still the IDA function to open.
    pdata_entries = 0
    nt = base + ida_bytes.get_dword(base + 0x3C)
    pdata_rva = ida_bytes.get_dword(nt + 24 + 112 + 3 * 8)  # data directory 3: exceptions
    pdata_size = ida_bytes.get_dword(nt + 24 + 112 + 3 * 8 + 4)
    for at in range(base + pdata_rva, base + pdata_rva + pdata_size - 11, 12):
        begin = ida_bytes.get_dword(at)
        f = ida_funcs.get_func(base + begin)
        if not begin or f is None or f.start_ea == base + begin or (begin in out):
            continue
        name = clean(ida_funcs.get_func_name(f.start_ea) or "")
        if name:
            out[begin] = f"{name}+0x{base + begin - f.start_ea:x}"
            pdata_entries += 1
    # Vtables by name (Fexty's "Class::vft" labels), for naming the objects a
    # job ran on (tools/job_report.py): vtables.tsv next to names.tsv.
    vtables = {ea - base: name for ea, name in idautils.Names() if "vft" in name.lower()}
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.with_name("vtables.tsv").write_text("".join(f"{rva:x}\t{vtables[rva]}\n" for rva in sorted(vtables)), encoding="utf-8")
    out_path.write_text("".join(f"{rva:x}\t{out[rva]}\n" for rva in sorted(out)), encoding="utf-8")
    sha = ida_nalt.retrieve_input_file_sha256()
    meta = {
        "database": idc.get_idb_path(),
        "input_file": ida_nalt.get_input_file_path(),
        "input_sha256": sha.hex() if sha else None,
        "imagebase": hex(base),
        "functions": functions,
        "named_functions": named,
        "pdata_entries_inside_functions": pdata_entries,
        "vtables": len(vtables),
        "lines": len(out),
        "exported": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    Path(meta_path or out_path.with_name("names.meta.json")).write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return meta


if __name__ == "__main__" and "__file__" in globals():
    result = export(Path(__file__).resolve().parents[1] / "local" / "ida" / "names.tsv")
    ida_kernwin.msg(f"ida_names: {result}\n")
