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
import hashlib
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
    """Calls the generator could not resolve report the address at run time instead of doing nothing; with the
    fallback emulator (project/src/x86_fallback.c) each one first tries to run the code emulated."""
    unresolved = port.gen / 'recomp_stubs_unresolved.c'
    if not unresolved.is_file():
        return
    text = unresolved.read_text(encoding='utf-8')
    if 'RECOMP_STUB_HIT(' in text:
        if 'xboxport_fallback_run(' not in text:
            text = re.sub(r'\{ RECOMP_STUB_HIT\((0x[0-9A-Fa-f]+)u?\);',
                          lambda m: '{ if (xboxport_fallback_run(%su, "direct call")) return; RECOMP_STUB_HIT(%su);'
                          % (m[1], m[1]), text)
            text = text.replace('#define RECOMP_STUB_HIT(va)',
                                'extern int xboxport_fallback_run(uint32_t va, const char *how);\n'
                                '#define RECOMP_STUB_HIT(va)', 1)
            unresolved.write_text(text, encoding='utf-8')
        return
    out = ['#include <stdint.h>', 'extern int xbox_missing_code(uint32_t va, const char *how);']
    for line in unresolved.read_text(encoding='utf-8').splitlines():
        m = re.match(r'void (\w+)\(void\).*(?:direct call|missing direct target) 0x([0-9A-Fa-f]+)', line)
        if m:
            out.append(f'void {m[1]}(void) {{ xbox_missing_code(0x{m[2]}u, "direct call"); }}')
    if len(out) > 2:
        unresolved.write_text('\n'.join(out) + '\n', encoding='utf-8')


# The fallback emulator's CPU: Unicorn's own Windows build (the PyPI wheel carries unicorn.dll, its import library
# and headers), pinned by hash.
UNICORN = ('2.1.1', 'unicorn-2.1.1-py2.py3-none-win_amd64.whl',
           'f0ebcfaba67ef0ebcd05ee3560268f1c6f683bdd08ff496888741a163d29735d')


def unicorn_dir():
    """Unicorn for the fallback emulator (.local/xboxport/unicorn), fetched once; None when unavailable (the port is
    then built without the fallback and skips untranslated code as before)."""
    dest = REPO / '.local/xboxport/unicorn'
    if (dest / 'lib/unicorn.dll').is_file():
        return dest
    if os.name != 'nt':
        return None
    version, wheel_name, sha = UNICORN
    tmp = dest.parent / 'unicorn-wheel'
    shutil.rmtree(tmp, ignore_errors=True)
    r = subprocess.run([sys.executable, '-m', 'pip', 'download', f'unicorn=={version}', '--no-deps', '--only-binary=:all:',
                        '--platform', 'win_amd64', '-d', str(tmp), '-q'], capture_output=True, text=True)
    wheel = tmp / wheel_name
    if r.returncode or not wheel.is_file() or hashlib.sha256(wheel.read_bytes()).hexdigest() != sha:
        log('- Unicorn (fallback emulator): not available, untranslated code will be skipped')
        return None
    import zipfile
    with zipfile.ZipFile(wheel) as z:
        for name in z.namelist():
            if name.startswith(('unicorn/include/', 'unicorn/lib/')) and not name.endswith('/'):
                target = dest / name[len('unicorn/'):]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(z.read(name))
    shutil.rmtree(tmp, ignore_errors=True)
    return dest


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
    mark_missing(port)   # translations made before the fallback get it too
    uc = unicorn_dir()
    run(['cmake', '-S', port.project, '-B', port.out, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_C_COMPILER=' + cc, '-DCMAKE_CXX_COMPILER=' + cxx, '-DXBOXRECOMP_DIR=' + REPO.as_posix(),
         '-DXBOXPORT_UNICORN_DIR=' + (uc.as_posix() if uc else '')])
    run(['cmake', '--build', port.out, '--parallel', str(args.jobs), '--target', 'game'])


PC_SETTINGS = '''; PC video settings of this game (tools/xboxport play reads them; an environment variable wins).
[video]
; Enlarging the frame to the window: linear | point | fsr (AMD FidelityFX Super Resolution 1.0) |
; dlss (NVIDIA DLSS, RTX cards, with api = vulkan; FSR otherwise)
filter = fsr
; 1 = vertical sync, 0 = off
vsync = 1
; keep = the frame's shape with bars; stretch = fill the window (ultrawide)
aspect = keep
; 1 = tell the game the TV is 16:9 (games with a widescreen mode then draw for it)
widescreen = 1
; 1 = tell the game the TV takes 480p (progressive)
progressive = 1
; fxaa = smooth jagged edges (with the linear or point filter), off = none
antialiasing = off
; -1 = Windows' default GPU; 0, 1, 2... = that DXGI adapter (the launcher lists them by name)
gpu = -1
; native = Direct3D 11; vulkan = Direct3D 11 on Vulkan (DXVK, when the graphics card supports Vulkan)
api = native
'''


def video_env(port):
    """Runtime environment for <port>/pc-settings.ini (written with the defaults the first time)."""
    import configparser
    ini = port.dir / 'pc-settings.ini'
    if not ini.is_file():
        ini.write_text(PC_SETTINGS, encoding='utf-8')
    cfg = configparser.ConfigParser(inline_comment_prefixes=(';', '#'))
    try:
        cfg.read(ini, encoding='utf-8')
    except configparser.Error:
        return {}
    v = cfg['video'] if cfg.has_section('video') else {}
    get = lambda key, default: str(v.get(key, default)).strip().lower()
    return {
        'RECOMP_D3D_PRESENT': '1',
        'RECOMP_PRESENT_FILTER': get('filter', 'fsr') if get('filter', 'fsr') in ('linear', 'point', 'fsr', 'dlss') else 'fsr',
        'RECOMP_PRESENT_VSYNC': '0' if get('vsync', '1') == '0' else '1',
        'RECOMP_PRESENT_ASPECT': 'stretch' if get('aspect', 'keep') == 'stretch' else 'keep',
        'RECOMP_XBOX_WIDESCREEN': '0' if get('widescreen', '1') == '0' else '1',
        'RECOMP_VIDEO_480P': '0' if get('progressive', '1') == '0' else '1',
        'RECOMP_PRESENT_AA': 'fxaa' if get('antialiasing', 'off') == 'fxaa' else 'off',
        'RECOMP_GRAPHICS_API': 'vulkan' if get('api', 'native') == 'vulkan' else 'native',
        'RECOMP_PRESENT_ADAPTER': get('gpu', '-1') if get('gpu', '-1').lstrip('-').isdigit() else '-1',
    }


def step_play(port, args):
    if not port.exe.is_file():
        raise SystemExit('The game is not built yet: install it from the launcher.')
    port.observed.mkdir(parents=True, exist_ok=True)
    # Code the run reaches that was not translated is recorded for the next build (see learn).
    # input.ini in the port folder: this game's controller settings (port_input), over the global ones.
    env = {**video_env(port), **os.environ, 'RECOMP_MISSING_FILE': str(port.missing), 'PORT_INPUT_GAME_DIR': str(port.dir)}
    # [video] api = vulkan: Direct3D 11 on Vulkan through DXVK (vulkan_layer.py), when the card supports Vulkan.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import vulkan_layer
    # With Vulkan the framebuffer window presents through port_gfx itself, and the D3D8 layer's Direct3D 11 goes
    # through DXVK. DLSS (filter = dlss) needs NVIDIA's network next to the game.
    cache = Path(__file__).resolve().parents[2] / '.local/downloads/vulkan'
    vulkan = env.get('RECOMP_GRAPHICS_API') == 'vulkan'
    vulkan_layer.stage(port.exe.parent, 'd3d11', vulkan, cache)
    vulkan_layer.stage_dlss(port.exe.parent, vulkan and env.get('RECOMP_PRESENT_FILTER') == 'dlss', cache)
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
