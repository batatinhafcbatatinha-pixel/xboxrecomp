"""Translate your own verified FIFA Street 2 XBE into private C and build the native PC port.

Everything produced (disassembly, generated C, build) stays in --local-dir
(default .local/), which is excluded from version control. Never upload it.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KIT = json.loads((ROOT / 'kit.json').read_text(encoding='utf-8'))
sys.path.insert(0, str(ROOT / 'tools'))
import validate_game  # noqa: E402

TOOLKIT = ROOT.parents[1]  # this xboxrecomp repository (games/street2/../..)
# Functions whose entries the disassembler rejects (an overlapping decode already starts
# there); their boundaries are known from the original code, so they are listed directly.
MANUAL_FUNCTIONS = [
    {'start': '0x000FA9A0', 'end': '0x000FABD5'},   # player creator, mov [ecx+0x238], 0xFA9A0
    {'start': '0x00213C19', 'end': '0x00213C5A'},   # mov [edx], 0x213C19
    {'start': '0x0016F2C0', 'end': '0x0016F3BE'},   # front-end update, hidden by a false gap prologue at 0x16F29E
]
# Functions the disassembler cuts short (seeds land on its case labels); the true end is given so
# every branch and the jump table at the end stay inside one translated function.
FUNCTION_ENDS = {
    0x00227853: 0x002279A1,   # command worker: 31-entry jump table at 0x2279A1
}


def run(args, cwd, env):
    print('Running:', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)


def toolkit():
    """xboxrecomp runtime + translator: this repository (games/street2/../..)."""
    if not (TOOLKIT / 'CMakeLists.txt').is_file():
        raise ValueError('Run this from games/street2 inside an xboxrecomp checkout')
    return TOOLKIT


def generate(xbe, local, tools, env):
    source = local / 'source/street2'
    if source.exists():
        shutil.rmtree(source)
    shutil.copytree(ROOT / 'street2', source)
    gen = source / 'project/src/recomp/gen'
    work = local / 'recomp-work'
    if work.exists():
        shutil.rmtree(work)
    disasm, ident, abi = work / 'disasm', work / 'ident', work / 'abi'
    for directory in (disasm, ident, abi, gen):
        directory.mkdir(parents=True, exist_ok=True)
    py = sys.executable
    # tools.disasm reads <xbe stem>_analysis.json beside the XBE: parse a private copy of the XBE first.
    xbe_copy = work / 'default.xbe'
    shutil.copy2(xbe, xbe_copy)
    xbe = xbe_copy
    run([py, '-m', 'tools.xbe_parser', xbe, '--json', work / 'default_analysis.json'], tools, env)
    seeds = []
    for name in ('seed_functions.json', 'seed_manual.json'):
        seeds += ['--seed-functions', source / 'hle' / name]
    run([py, '-m', 'tools.disasm', xbe, '--text-only', '--extra-sections', 'D3D,XGRPH,DSOUND,XPP,DOLBY',
         '--force', *seeds, '--output', disasm], tools, env)
    functions_path = disasm / 'functions.json'
    functions = json.loads(functions_path.read_text(encoding='utf-8'))
    known = {int(f['start'], 16) for f in functions}
    for entry in functions:
        end = FUNCTION_ENDS.get(int(entry['start'], 16))
        if end:
            entry['end'], entry['size'] = f'0x{end:08X}', end - int(entry['start'], 16)
    for manual in MANUAL_FUNCTIONS:
        start, end = int(manual['start'], 16), int(manual['end'], 16)
        if start not in known:
            functions.append({'start': manual['start'], 'end': manual['end'], 'size': end - start,
                              'name': f'sub_{start:08X}', 'section': '.text', 'confidence': 0.95,
                              'detection_method': 'seed_manual', 'num_instructions': 0, 'has_prologue': False,
                              'calls_to': [], 'called_by': []})
    functions.sort(key=lambda f: int(f['start'], 16))
    functions_path.write_text(json.dumps(functions, indent=2), encoding='utf-8')
    run([py, '-m', 'tools.func_id', xbe, '--functions', functions_path, '--strings', disasm / 'strings.json',
         '--xrefs', disasm / 'xrefs.json', '--output', ident], tools, env)
    run([py, '-m', 'tools.abi_analysis', xbe, '--disasm-dir', disasm, '--func-id-dir', ident,
         '--output', abi / 'abi_functions.json'], tools, env)
    run([py, '-m', 'tools.recomp', xbe, '--game-name', 'FIFA Street 2', '--all', '--split', '250',
         '--disasm-dir', disasm, '--func-id-dir', ident, '--abi-dir', abi, '--gen-dir', gen], tools, env)
    missing = [m['start'] for m in MANUAL_FUNCTIONS
               if not any(f'void sub_{int(m["start"], 16):08X}(void)' in p.read_text(encoding='utf-8', errors='replace')
                          for p in gen.glob('recomp_*.c'))]
    if missing:
        raise ValueError('Translation is missing required functions: ' + ', '.join(missing))
    # Post-passes, in the order the port was validated with.
    run([py, source / 'hle/translate-nv-interrupt-writes.py'], source, env)
    run([py, ROOT / 'tools/translate-ebp-boundaries.py', '--apply', '--directory', gen], source, env)
    run([py, source / 'hle/make-smp-serial.py'], source, env)
    run([py, source / 'hle/apply-hle-wrappers.py'], source, env)
    (local / 'generation-complete.json').write_text(json.dumps({'xbeSha256': KIT['required_files']['default.xbe']}) + '\n',
                                                    encoding='utf-8')
    return source / 'project'


def developer_environment():
    """MSVC headers/libraries for clang-cl (from the Visual Studio vcvars64 script)."""
    if os.name != 'nt' or os.environ.get('VCToolsInstallDir'):
        return {key.upper() if os.name == 'nt' else key: value for key, value in os.environ.items()}
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        raise ValueError('Visual Studio 2022 (Build Tools) with the C++ workload is required')
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                       'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                       '-property', 'installationPath'], text=True).strip()
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    output = subprocess.check_output(f'"{vcvars}" >nul && set', shell=True, text=True)
    return {key.upper(): value for key, value in (line.split('=', 1) for line in output.splitlines() if '=' in line)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-files', type=Path, required=True, help='Extracted disc folder named game_files')
    parser.add_argument('--local-dir', type=Path, default=ROOT / '.local')
    parser.add_argument('--llvm-dir', type=Path, help='LLVM install folder containing bin/clang-cl or bin/clang (default: PATH)')
    parser.add_argument('--dxvk-native-dir', type=Path, help='Linux: DXVK native prefix (include/dxvk, lib/libdxvk_d3d11.so)')
    parser.add_argument('--vkd3d-dir', type=Path, help='Linux, macOS: vkd3d prefix (include/vkd3d, lib/libvkd3d-shader)')
    parser.add_argument('--dxmt-dir', type=Path, help='macOS: DXMT prefix from tools/macos/build-deps.sh')
    parser.add_argument('--reuse-generated', action='store_true', help='Skip the translation after a completed one')
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    args = parser.parse_args()
    game_files = args.game_files.resolve()
    problems = validate_game.validate(game_files)
    if problems:
        raise ValueError('Unsupported game files: ' + '; '.join(problems))
    local = args.local_dir.resolve()
    local.mkdir(parents=True, exist_ok=True)
    env = developer_environment()
    if args.llvm_dir:
        env['PATH'] = str(args.llvm_dir.resolve() / 'bin') + os.pathsep + env.get('PATH', '')
    compiler_name = 'clang-cl' if os.name == 'nt' else 'clang'
    clang_cl, cmake, ninja = (shutil.which(n, path=env.get('PATH')) for n in (compiler_name, 'cmake', 'ninja'))
    if not (clang_cl and cmake and ninja):
        raise ValueError(compiler_name + ', cmake and ninja must be on PATH (or pass --llvm-dir)')
    tools = toolkit()
    stamp = local / 'generation-complete.json'
    if args.reuse_generated and stamp.is_file():
        project = local / 'source/street2/project'
    else:
        stamp.unlink(missing_ok=True)
        project = generate(game_files / 'default.xbe', local, tools, env)
    # These host sources must also reach an existing --reuse-generated tree:
    # the command-worker hook renames the generated entry to *_gen, so omitting
    # its native implementation leaves sub_00227853 undefined at link time.
    for relative in ('CMakeLists.txt', 'src/native_metal_bridge.m', 'src/native_command_worker.c', 'src/native_kernel_clock.c', 'src/hle_posix_win32.h'):
        shutil.copy2(ROOT / 'street2/project' / relative, project / relative)
    build = local / 'build'
    configure = [cmake, '-S', project, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                 '-DCMAKE_C_COMPILER=' + clang_cl, '-DXBOXRECOMP_DIR=' + str(tools)]
    if args.dxvk_native_dir:
        configure.append('-DDXVK_NATIVE_DIR=' + str(args.dxvk_native_dir.resolve()))
    if args.vkd3d_dir:
        configure.append('-DVKD3D_DIR=' + str(args.vkd3d_dir.resolve()))
    if sys.platform == 'darwin':
        if not (args.dxmt_dir and args.vkd3d_dir):
            raise ValueError('macOS: pass --dxmt-dir and --vkd3d-dir (see MACOS.md)')
        configure += ['-DDXMT_DIR=' + str(args.dxmt_dir.resolve()), '-DCMAKE_OSX_ARCHITECTURES=x86_64']
    run(configure, local, env)
    run([cmake, '--build', build, '--parallel', str(args.jobs), '--target', 'fifastreet_recomp'], local, env)
    if os.name == 'nt':
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        __import__('fetch_pad_support').install(build, sdl3=True)
    print('Built privately in', build)
    print('Play with: python tools/play.py --game-files', game_files)
    print('Do not upload .local/, the generated code or the executable.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('Build failed:', error, file=sys.stderr)
        sys.exit(1)
