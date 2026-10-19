"""The public repository's first commit, built from this private one.

  py -3.12 -B tools/public_tree.py                      export and check only
  py -3.12 -B tools/public_tree.py --author "Name <id+name@users.noreply.github.com>"

Exports HEAD's tracked files (git archive: nothing under local/, no build
output) into local/public/<version>/, then checks every file for what must
not go public: this machine's paths (the repository, the user profile), the
Windows user name, and the e-mail addresses that authored this repository's
commits. With --author it also makes a fresh repository there: one commit
by that author (as committer too) and an annotated tag v<version>. It never
pushes and never touches this repository; publishing is the owner's step.
"""
import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(*args, cwd=ROOT, env=None):
    return subprocess.run(['git', '-C', str(cwd), *args], capture_output=True, check=True, env=env).stdout


def version():
    m = re.search(r'project\(mhw_cpuopt VERSION (\d+\.\d+\.\d+)', (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))
    if not m:
        raise SystemExit('no version in CMakeLists.txt')
    return m.group(1)


def markers():
    """Lower-case byte strings that must not appear in a public file."""
    found = set()
    for path in (str(ROOT), os.path.expanduser('~')):
        for form in (path, path.replace('\\', '/'), path.replace('\\', '\\\\')):
            found.add(form.lower().encode('utf-8'))
    user = os.environ.get('USERNAME', '')
    if len(user) >= 4:
        found.add(user.lower().encode('utf-8'))
    for email in git('log', '--format=%ae%n%ce').decode().split():
        if email and 'noreply' not in email:
            found.add(email.lower().encode('utf-8'))
    found.add(b'mh world modding')
    return found


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('--author', help='"Name <email>" for the public first commit (a GitHub noreply address)')
    a.add_argument('--out', type=Path, help='default local/public/<version>')
    args = a.parse_args()

    if git('status', '--porcelain', '--untracked-files=no').strip():
        raise SystemExit('the work tree has uncommitted changes: commit first')
    v = version()
    out = args.out or ROOT / 'local' / 'public' / v
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(git('archive', '--format=tar', 'HEAD'))) as tar:
        tar.extractall(out, filter='data')

    bad = []
    marks = markers()
    files = [p for p in out.rglob('*') if p.is_file()]
    for p in files:
        lower = p.read_bytes().lower()
        for m in marks:
            if m in lower:
                bad.append(f'{p.relative_to(out)}: holds {m[:30]!r}')
    if bad:
        print('\n'.join(bad))
        raise SystemExit(f'{len(bad)} problem(s): nothing committed; fix them in this repository first')
    head = git('rev-parse', '--short=12', 'HEAD').decode().strip()
    print(f'{out}: {len(files)} files from {head}, checked: no local paths, user name or private e-mail')

    if not args.author:
        print('no --author: exported and checked only')
        return 0
    m = re.fullmatch(r'\s*(.+?)\s*<([^<>@\s]+@[^<>\s]+)>\s*', args.author)
    if not m:
        raise SystemExit('--author must look like "Name <email>"')
    name, email = m.groups()
    env = dict(os.environ, GIT_AUTHOR_NAME=name, GIT_AUTHOR_EMAIL=email, GIT_COMMITTER_NAME=name, GIT_COMMITTER_EMAIL=email)
    git('init', '-q', '-b', 'main', cwd=out)
    git('config', 'user.name', name, cwd=out)
    git('config', 'user.email', email, cwd=out)
    git('add', '-A', cwd=out)
    message = (f'cpuopt {v}: the first public release\n\n'
               'CPU optimizations for Monster Hunter: World, an Astera Loader plugin.\n'
               'See README.md and CHANGELOG.md.\n\n'
               'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n')
    subprocess.run(['git', '-C', str(out), 'commit', '-q', '-F', '-'], input=message.encode(), check=True, env=env)
    git('tag', '-a', f'v{v}', '-m', f'cpuopt {v}', cwd=out, env=env)
    print(f'{out}: one commit by {name} <{email}>, tag v{v}; nothing pushed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
