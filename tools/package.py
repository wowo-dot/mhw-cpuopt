"""The player zip: py -3.12 -B tools/package.py [--label beta]
                    py -3.12 -B tools/package.py --tested v0.16.0 --dll local/runs/al-43/cpuopt.dll

Builds nothing: it packs build/bin/Release/cpuopt.dll after checking that
- the work tree is clean (tracked files) and the DLL was built at HEAD (its
  commit stamp is HEAD's, without "-dirty": reconfigure after committing);
- no shipped file holds this machine's paths (the repository, the user
  profile);
- the DLL imports no function that Wine and Proton lack (WINE_ABSENT).

--tested TAG --dll FILE packs the DLL that ran in game instead (a run
folder's copy), so the release is byte for byte what was tested: its stamp
must be TAG's commit, CMakeLists.txt's version TAG's, and nothing that
builds into the DLL (src/, third_party/, CMakeLists.txt) may have changed
between TAG and HEAD; the documents come from HEAD.

Output in local/release/: cpuopt-<version>[-<label>].zip with the layout
AsteraLoader/plugins/cpuopt/{cpuopt.dll, README.md, CHANGELOG.md, BETA.md (with a label)},
which installs as a Mod Organizer 2 mod or extracts into the game folder,
and a .sha256.txt beside it. The zip's entries carry a fixed time, so the
same inputs give the same zip.
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DLL = ROOT / 'build' / 'bin' / 'Release' / 'cpuopt.dll'
OUT = ROOT / 'local' / 'release'
STAMP = (2026, 1, 1, 0, 0, 0)

# Windows functions Wine 11.0 and Proton 10 do not export (their kernel32 and
# kernelbase .spec files, 2026-10-20). Wine fills such an import with a stub
# that ends the process with 0x80000100 when called, so the code looks these
# up with GetProcAddress instead.
WINE_ABSENT = {'GetProcessDefaultCpuSets', 'GetThreadSelectedCpuSets',
               'PssCaptureSnapshot', 'PssDuplicateSnapshot', 'PssFreeSnapshot', 'PssQuerySnapshot',
               'PssWalkMarkerCreate', 'PssWalkMarkerFree', 'PssWalkMarkerGetPosition',
               'PssWalkMarkerSeekToBeginning', 'PssWalkMarkerSetPosition', 'PssWalkSnapshot'}


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, text=True, check=True).stdout.strip()


def version():
    m = re.search(r'project\(mhw_cpuopt VERSION (\d+\.\d+\.\d+)', (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))
    if not m:
        raise SystemExit('no version in CMakeLists.txt')
    return m.group(1)


def private_markers():
    """Byte strings that must not ship: this machine's paths, in the forms a binary or text would hold them."""
    markers = set()
    # The repository, the folder that holds it (named in this repository's own
    # paths, so not written out here) and the user profile.
    for path in (str(ROOT), str(ROOT.parent), os.path.expanduser('~')):
        for form in (path, path.replace('\\', '/'), path.replace('\\', '\\\\')):
            markers.add(form.encode('utf-8').lower())
            markers.add(form.encode('utf-16-le').lower())
    return markers


def imported_names(data):
    """The names of the functions a PE32+ file imports by name (not by ordinal)."""
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE file')
    sections, = struct.unpack_from('<H', data, pe + 6)
    optional_size, = struct.unpack_from('<H', data, pe + 20)
    optional = pe + 24
    if struct.unpack_from('<H', data, optional)[0] != 0x20B:
        raise ValueError('not a 64-bit PE file')
    table = [struct.unpack_from('<IIII', data, optional + optional_size + 40 * i + 8) for i in range(sections)]

    def at(rva):
        for virtual_size, address, raw_size, raw in table:
            if address <= rva < address + max(virtual_size, raw_size):
                return raw + rva - address
        raise ValueError(f'RVA {rva:#x} is in no section')

    names = set()
    directory, = struct.unpack_from('<I', data, optional + 120)  # data directory 1: imports
    if not directory:
        return names
    descriptor = at(directory)
    while True:
        lookup, _, _, dll_name, address_table = struct.unpack_from('<5I', data, descriptor)
        if not dll_name:
            return names
        entry = at(lookup or address_table)
        while thunk := struct.unpack_from('<Q', data, entry)[0]:
            if not thunk >> 63:
                name = at(thunk) + 2  # after the hint
                names.add(data[name:data.index(b'\0', name)].decode('ascii'))
            entry += 8
        descriptor += 20


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--label', default='', help='e.g. beta: the zip name gets it and BETA.md goes in')
    a.add_argument('--tested', metavar='TAG', help='pack the DLL tested in game at TAG (with --dll) instead of the build')
    a.add_argument('--dll', type=Path, help='with --tested: the DLL that ran in game')
    args = a.parse_args()

    if git('status', '--porcelain', '--untracked-files=no'):
        raise SystemExit('the work tree has uncommitted changes: commit, reconfigure and build first')
    if args.tested:
        if not args.dll:
            raise SystemExit('--tested needs --dll, the DLL that ran in game')
        commit = git('rev-parse', '--short=12', f'{args.tested}^{{commit}}')
        if version() != args.tested.lstrip('v'):
            raise SystemExit(f'CMakeLists.txt says {version()}, the tag {args.tested}')
        built = [f for f in git('diff', '--name-only', args.tested, 'HEAD').splitlines()
                 if f.startswith(('src/', 'third_party/')) or f == 'CMakeLists.txt']
        if built:
            raise SystemExit(f'code changed since {args.tested}: {", ".join(built)}; test a new build instead')
        data = args.dll.read_bytes()
        stamp = commit
    else:
        stamp = git('rev-parse', '--short=12', 'HEAD')
        data = DLL.read_bytes()
    if stamp.encode() not in data:
        raise SystemExit(f'the DLL was not built at {stamp}' + ('' if args.tested else ': run cmake -S . -B build, then build'))
    if (stamp + '-dirty').encode() in data:
        raise SystemExit('the DLL was built from a dirty tree')
    absent = sorted(imported_names(data) & WINE_ABSENT)
    if absent:
        raise SystemExit(f'the DLL imports {", ".join(absent)}: Wine and Proton lack it, look it up at run time')
    head = stamp

    files = {'cpuopt.dll': data, 'README.md': (ROOT / 'README.md').read_bytes(),
             'CHANGELOG.md': (ROOT / 'CHANGELOG.md').read_bytes()}
    if args.label:
        files['BETA.md'] = (ROOT / 'docs' / 'sending-results.md').read_bytes()
    markers = private_markers()
    for name, content in files.items():
        lower = content.lower()
        for m in markers:
            if m in lower:
                raise SystemExit(f'{name} holds a local path ({m[:40]!r}...): not shipped')

    name = f'cpuopt-{version()}' + (f'-{args.label}' if args.label else '')
    OUT.mkdir(parents=True, exist_ok=True)
    zip_path = OUT / f'{name}.zip'
    with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for file, content in files.items():
            info = zipfile.ZipInfo(f'AsteraLoader/plugins/cpuopt/{file}', date_time=STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, content)
    sums = {f: hashlib.sha256(c).hexdigest() for f, c in files.items()}
    sums[zip_path.name] = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (OUT / f'{name}.sha256.txt').write_text(''.join(f'{h}  {f}\n' for f, h in sums.items()), encoding='utf-8')
    print(f'{zip_path} ({zip_path.stat().st_size} bytes), commit {head}')
    for f, h in sums.items():
        print(f'  {h}  {f}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
