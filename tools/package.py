"""The player zip: py -3.12 -B tools/package.py [--label beta]

Builds nothing: it packs build/bin/Release/cpuopt.dll after checking that
- the work tree is clean (tracked files) and the DLL was built at HEAD (its
  commit stamp is HEAD's, without "-dirty": reconfigure after committing);
- no shipped file holds this machine's paths (the repository, the user
  profile).

Output in local/release/: cpuopt-<version>[-<label>].zip with the layout
AsteraLoader/plugins/cpuopt/{cpuopt.dll, README.md, BETA.md (with a label)},
which installs as a Mod Organizer 2 mod or extracts into the game folder,
and a .sha256.txt beside it. The zip's entries carry a fixed time, so the
same inputs give the same zip.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DLL = ROOT / 'build' / 'bin' / 'Release' / 'cpuopt.dll'
OUT = ROOT / 'local' / 'release'
STAMP = (2026, 1, 1, 0, 0, 0)


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
    for path in (str(ROOT), os.path.expanduser('~')):
        for form in (path, path.replace('\\', '/'), path.replace('\\', '\\\\')):
            markers.add(form.encode('utf-8').lower())
            markers.add(form.encode('utf-16-le').lower())
    markers.add(b'mh world modding')
    markers.add('mh world modding'.encode('utf-16-le'))
    return markers


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--label', default='', help='e.g. beta: the zip name gets it and BETA.md goes in')
    args = a.parse_args()

    if git('status', '--porcelain', '--untracked-files=no'):
        raise SystemExit('the work tree has uncommitted changes: commit, reconfigure and build first')
    head = git('rev-parse', '--short=12', 'HEAD')
    data = DLL.read_bytes()
    if head.encode() not in data:
        raise SystemExit(f'{DLL.name} was not built at HEAD {head}: run cmake -S . -B build, then build')
    if (head + '-dirty').encode() in data:
        raise SystemExit(f'{DLL.name} was built from a dirty tree')

    files = {'cpuopt.dll': data, 'README.md': (ROOT / 'README.md').read_bytes()}
    if args.label:
        files['BETA.md'] = (ROOT / 'docs' / 'beta-testing.md').read_bytes()
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
