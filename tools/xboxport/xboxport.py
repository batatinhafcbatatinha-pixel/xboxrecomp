#!/usr/bin/env python3
"""xboxport: build a native PC port of any original-Xbox game from its disc files, with the same steps for every game.

usage: python tools/xboxport/xboxport.py <step> [--port DIR] [--jobs N] [--force]

The generic path of this repository: no game-specific code. The ports in games/ replace the Xbox's graphics and
sound libraries with native code; a generic port keeps the game's own libraries (translated with the rest of the
game) and runs them on the toolkit's emulated hardware (NV2A, APU), as templates/new-game does. A port folder holds
game_files/ (the disc, copied and checked by the launcher). Steps (each skips itself when done; --force redoes it):
  learn      functions a run found missing (observed/missing_code.txt, written by play) become seeds of the
             analysis (observed/seed_functions.json, checked by tools.seed_from_log): the next build includes them
  analyze    XBE parser, disassembly and function identification of default.xbe (build/analysis)
  translate  tools.recomp --all: the game as C (project/src/recomp/gen)
  prepare    learn analyze translate
  compile    compile and link build/out/bin/game.exe (clang-cl, the toolkit runtime)
  play       run the game
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
TEMPLATE = HERE / 'project'


class Port:
    def __init__(self, folder):
        self.dir = Path(folder).resolve()
        self.files = self.dir / 'game_files'
        self.xbe = self.files / 'default.xbe'
        self.build = self.dir / 'build'
        self.analysis = self.build / 'analysis'
        self.project = self.dir / 'project'
        self.gen = self.project / 'src/recomp/gen'
        self.out = self.build / 'out'
        self.exe = self.out / 'bin' / 'game.exe'
        self.observed = self.dir / 'observed'
        self.missing = self.observed / 'missing_code.txt'
        self.seeds = self.observed / 'seed_functions.json'


def newer(target, *sources):
    """True when target exists and no existing source is newer."""
    if not target.is_file():
        return False
    t = target.stat().st_mtime
    return all(not s.is_file() or s.stat().st_mtime <= t for s in sources)


def log(msg):
    print(msg, flush=True)


def run(cmd, cwd=None, env=None):
    log('$ ' + ' '.join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env)
    if r.returncode != 0:
        raise SystemExit(f'failed ({r.returncode}): {Path(str(cmd[0])).name}')


def toolkit(*args):
    """A module of the toolkit (tools.xbe_parser, ...) run from the repository root."""
    run([sys.executable, '-u', '-m', *args], cwd=REPO, env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})


def need(name):
    path = shutil.which(name)
    if not path:
        raise SystemExit(f'{name} was not found: run this through the launcher or Setup (they put the build tools in PATH)')
    return path


def step_learn(port, args):
    if not port.missing.is_file() or newer(port.seeds, port.missing):
        log('- functions found missing by a run: nothing new')
        return
    if not (port.analysis / 'disasm/functions.json').is_file():
        return
    # seed_from_log reads run logs; the runtime's missing_code.txt lists one address per line.
    addresses = [line.split()[0] for line in port.missing.read_text(encoding='utf-8', errors='replace').splitlines()
                 if line.startswith('0x')]
    fake_log = port.observed / 'missing_as_log.txt'
    fake_log.write_text(''.join(f'Failed to resolve VA {a}\n' for a in addresses), encoding='utf-8')
    if not port.seeds.is_file():
        port.seeds.write_text('[]\n', encoding='utf-8')
    toolkit('tools.seed_from_log', fake_log, port.xbe, '--functions', port.analysis / 'disasm/functions.json',
            '--seeds', port.seeds, '--analysis-json', port.analysis / 'xbe-analysis.json')
    port.seeds.touch()


def step_analyze(port, args):
    if not port.xbe.is_file():
        raise SystemExit(f'{port.xbe} is missing: install the game files first (the launcher does it from your disc)')
    a = port.analysis
    a.mkdir(parents=True, exist_ok=True)
    functions = a / 'disasm/functions.json'
    if not newer(functions, port.seeds) or args.force:
        if not (a / 'xbe-analysis.json').is_file() or args.force:
            toolkit('tools.xbe_parser', port.xbe, '--quiet', '--json', a / 'xbe-analysis.json')
        seeds = ['--seed-functions', port.seeds] if port.seeds.is_file() else []
        toolkit('tools.disasm', port.xbe, '--analysis-json', a / 'xbe-analysis.json', '--text-only', *seeds, '-o', a / 'disasm')
    else:
        log('- disassembly: already done')
    if not newer(a / 'func-id/identified_functions.json', functions) or args.force:
        toolkit('tools.func_id', port.xbe, '--functions', a / 'disasm/functions.json', '--strings', a / 'disasm/strings.json',
                '--xrefs', a / 'disasm/xrefs.json', '-o', a / 'func-id')
    else:
        log('- function identification: already done')


def project_files(port):
    """The generic project (tools/xboxport/project) and the runtime headers the generated code includes."""
    for src in TEMPLATE.rglob('*'):
        if src.is_file():
            dst = port.project / src.relative_to(TEMPLATE)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
    (port.project / 'src/recomp').mkdir(parents=True, exist_ok=True)
    for name in ('recomp_types.h', 'xbox_memory.h'):
        shutil.copyfile(REPO / 'templates/runtime' / name, port.project / 'src/recomp' / name)


def mark_missing(port):
    """Calls the generator could not resolve report the address at run time instead of doing nothing."""
    unresolved = port.gen / 'recomp_stubs_unresolved.c'
    if not unresolved.is_file():
        return
    out = ['#include <stdint.h>', 'extern int xbox_missing_code(uint32_t va, const char *how);']
    for line in unresolved.read_text(encoding='utf-8').splitlines():
        m = re.match(r'void (\w+)\(void\).*(?:direct call|missing direct target) 0x([0-9A-Fa-f]+)', line)
        if m:
            out.append(f'void {m[1]}(void) {{ xbox_missing_code(0x{m[2]}u, "direct call"); }}')
    if len(out) > 2:
        unresolved.write_text('\n'.join(out) + '\n', encoding='utf-8')


def step_translate(port, args):
    done = port.gen / 'recomp_funcs.h'
    if newer(done, port.analysis / 'func-id/identified_functions.json') and not args.force:
        log('- translated code: already done')
        return
    project_files(port)
    shutil.rmtree(port.gen, ignore_errors=True)
    name = port.dir.name
    toolkit('tools.recomp', port.xbe, '--all', '--game-name', name, '--disasm-dir', port.analysis / 'disasm',
            '--func-id-dir', port.analysis / 'func-id', '--split', '128', '--gen-dir', port.gen, '-o', port.build / 'aot')
    mark_missing(port)


def step_prepare(port, args):
    step_learn(port, args)
    step_analyze(port, args)
    step_translate(port, args)


def step_compile(port, args):
    need('cmake'), need('ninja')
    # llvm-mingw (setup sets CC/CXX to clang): no Visual Studio needed; else clang-cl / cl with the MSVC environment.
    if os.environ.get('CC') and shutil.which(os.environ['CC']):
        cc, cxx = os.environ['CC'], os.environ.get('CXX', 'clang++')
    elif shutil.which('clang') and shutil.which('x86_64-w64-mingw32-clang'):
        cc, cxx = 'clang', 'clang++'
    else:
        cc = cxx = shutil.which('clang-cl') or need('cl')
    project_files(port)
    run(['cmake', '-S', port.project, '-B', port.out, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_C_COMPILER=' + cc, '-DCMAKE_CXX_COMPILER=' + cxx, '-DXBOXRECOMP_DIR=' + REPO.as_posix()])
    run(['cmake', '--build', port.out, '--parallel', str(args.jobs), '--target', 'game'])


def step_play(port, args):
    if not port.exe.is_file():
        raise SystemExit('The game is not built yet: install it from the launcher.')
    port.observed.mkdir(parents=True, exist_ok=True)
    # Code the run reaches that was not translated is recorded for the next build (see learn).
    env = {**os.environ, 'RECOMP_MISSING_FILE': str(port.missing)}
    r = subprocess.run([str(port.exe), str(port.files)], cwd=port.exe.parent, env=env)
    sys.exit(r.returncode)


STEPS = {'learn': step_learn, 'analyze': step_analyze, 'translate': step_translate, 'prepare': step_prepare, 'compile': step_compile,
         'play': step_play, 'all': lambda p, a: (step_prepare(p, a), step_compile(p, a))}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('step', choices=STEPS)
    ap.add_argument('--port', default='.')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()
    STEPS[args.step](Port(args.port), args)


if __name__ == '__main__':
    main()
