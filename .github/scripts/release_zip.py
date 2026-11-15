"""Builds one release ZIP of this repository for one platform (run by .github/workflows/release.yml).

    python .github/scripts/release_zip.py --name PS2Recomp --platform windows --sha <commit> \
        --launcher Launcher.exe --launcher-tag v0.6.1 --out out/PS2Recomp-windows.zip

What goes in: the files git tracks in this checkout and in its submodules (so tools/port-installer is filled, never
an empty folder), read from the working tree as this runner checked them out (CRLF text on Windows, LF elsewhere;
file modes kept). Left out: .git, CI files (.github/), git metadata files. Added: <name>/RELEASE.txt and
<name>/.local/source-version.txt (the commit, which the launcher's updater reads), and on Windows <name>/Launcher.exe.
Everything sits under one top folder <name>/, so Launcher.exe is at the recompiler's root, next to launcher.json.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import zipfile
from pathlib import Path

SKIP_DIRS = {'.git', '.github'}
SKIP_FILES = {'.gitignore', '.gitattributes', '.gitmodules', '.git'}
# Disc images and console executables never belong in a release.
FORBIDDEN = {'.iso', '.cso', '.chd', '.xiso', '.gcm', '.wbfs', '.pbp', '.xex', '.nsp', '.pkg', '.cue', '.mdf', '.ecm'}


def git(*args):
    return subprocess.run(['git', *args], check=True, capture_output=True).stdout


def tracked_files():
    names = git('ls-files', '--recurse-submodules', '-z').decode('utf-8').split('\0')
    keep = []
    for name in filter(None, names):
        parts = name.split('/')
        if any(p in SKIP_DIRS for p in parts[:-1]) or parts[-1] in SKIP_FILES:
            continue
        keep.append(name)
    return sorted(keep)


def submodule_paths():
    if not Path('.gitmodules').is_file():
        return []
    out = subprocess.run(['git', 'config', '-f', '.gitmodules', '--get-regexp', r'^submodule\..*\.path$'],
                         capture_output=True).stdout.decode()
    return [line.split(' ', 1)[1].strip() for line in out.splitlines() if ' ' in line]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--name', required=True)
    p.add_argument('--platform', choices=['windows', 'linux', 'macos'], required=True)
    p.add_argument('--sha', required=True)
    p.add_argument('--launcher', type=Path, help='Launcher.exe (Windows only)')
    p.add_argument('--launcher-tag', default='')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()

    files = tracked_files()
    for sub in submodule_paths():
        if not any(f.startswith(sub + '/') for f in files):
            sys.exit(f'submodule {sub} is empty in the checkout')
    bad = [f for f in files if Path(f).suffix.lower() in FORBIDDEN]
    if bad:
        sys.exit('refusing to package disc images / console executables: ' + ', '.join(bad))
    missing = [f for f in files if not Path(f).is_file()]
    if missing:
        sys.exit('tracked files missing from the checkout: ' + ', '.join(missing[:20]))

    launcher_line = ''
    if a.platform == 'windows':
        if not a.launcher or not a.launcher.is_file():
            sys.exit('the Windows ZIP needs --launcher Launcher.exe')
        digest = hashlib.sha256(a.launcher.read_bytes()).hexdigest()
        launcher_line = f'Launcher.exe: port-installer {a.launcher_tag}, SHA-256 {digest}\n'
        start = ('Start: open Launcher.exe in this folder (Windows 10/11 x64). It finds launcher.json next to it, installs\n'
                 'what the build needs, builds the game from your own disc and starts it.\n')
    else:
        label = 'Linux' if a.platform == 'linux' else 'macOS'
        start = (f'Start: there is no launcher for {label} (Launcher.exe is a Windows program). Build from this source\n'
                 f'as README.md describes for {label}; text files have LF line endings and scripts keep their\n'
                 'executable bit.\n')
    crlf = 0
    top = a.name
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            z.write(f, f'{top}/{f}')
            if f.endswith(('.md', '.txt', '.py', '.cmake')) and b'\r\n' in Path(f).read_bytes()[:65536]:
                crlf += 1
        nl = '\r\n' if a.platform == 'windows' else '\n'
        info = (f'{a.name} release ZIP for {a.platform}\n'
                f'Source: https://github.com/batatinhafcbatatinha-pixel/{a.name} commit {a.sha}\n'
                f'{launcher_line}{start}'
                'Source code and configuration only: nothing from any game. The game always comes from your own disc.\n')
        z.writestr(f'{top}/RELEASE.txt', info.replace('\n', nl))
        z.writestr(f'{top}/.local/source-version.txt', a.sha + '\n')
        if a.platform == 'windows':
            z.write(a.launcher, f'{top}/Launcher.exe')
    print(f'{a.out}: {len(files)} source files (+RELEASE.txt, .local/source-version.txt'
          f'{", Launcher.exe" if a.platform == "windows" else ""}), {a.out.stat().st_size >> 10} KiB, '
          f'{crlf} text files with CRLF')


if __name__ == '__main__':
    main()
