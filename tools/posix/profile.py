#!/usr/bin/env python3
"""Build or launch an existing native profile on Linux or macOS."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]


def call(command, root, env):
    subprocess.run([str(part) for part in command], cwd=root, env=env, check=True)


def environment():
    env = os.environ.copy()
    tools = REPO / '.local/tools'
    folders = [tools / 'cmake-3.31.12/bin', tools / 'cmake-3.31.12/CMake.app/Contents/bin',
               tools / 'ninja-1.13.2']
    env['PATH'] = os.pathsep.join(str(p) for p in folders if p.is_dir()) + os.pathsep + env.get('PATH', '')
    if platform.machine().lower() not in ('x86_64', 'amd64'):
        raise ValueError('This renderer requires x86-64 Python and dependencies; use the macOS build script on Apple Silicon.')
    if sys.platform == 'darwin':
        env['PATH'] = '/usr/local/bin:' + env['PATH']
        prefixes = ['/usr/local', '/usr/local/opt/openssl@3']
        env['CMAKE_PREFIX_PATH'] = os.pathsep.join(prefixes + [env.get('CMAKE_PREFIX_PATH', '')])
        env['PKG_CONFIG_PATH'] = '/usr/local/lib/pkgconfig:/usr/local/opt/openssl@3/lib/pkgconfig:' + env.get('PKG_CONFIG_PATH', '')
    return env


def dependencies(env):
    base = Path(env.get('DEPS', str(Path.home() / '.local/share/native-ports-macos')
                        if sys.platform == 'darwin' else str(REPO / '.local/native-deps'))).expanduser().resolve()
    graphics_key = 'DXMT_DIR' if sys.platform == 'darwin' else 'DXVK_NATIVE_DIR'
    graphics = Path(env.get(graphics_key, str(base / ('dxmt' if sys.platform == 'darwin' else 'dxvk-native')))).resolve()
    shader = Path(env.get('VKD3D_DIR', str(base / 'vkd3d'))).resolve()
    required = [graphics / ('lib/d3d11.dylib' if sys.platform == 'darwin' else 'lib/libdxvk_d3d11.so'),
                graphics / ('lib/dxgi.dylib' if sys.platform == 'darwin' else 'lib/libdxvk_dxgi.so'),
                shader / ('lib/libvkd3d-shader.dylib' if sys.platform == 'darwin' else 'lib/libvkd3d-shader.so')]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise ValueError('Install native renderer development libraries first; see docs/LINUX_MACOS.md. Missing: ' + ', '.join(missing))
    return graphics_key, graphics, shader


def build(root, jobs, extra, env):
    for tool in ('cmake', 'ninja', 'clang'):
        if not shutil.which(tool, path=env['PATH']):
            raise ValueError(tool + ' is required on PATH')
    key, graphics, shader = dependencies(env)
    options = ['-D' + key + '=' + str(graphics), '-DVKD3D_DIR=' + str(shader)]
    if sys.platform == 'darwin':
        options += ['-DCMAKE_OSX_ARCHITECTURES=x86_64']
    pipeline = root / 'scripts/native_pipeline.py'
    builder = root / 'tools/build_local.py'
    if pipeline.is_file():
        # Retain this profile's established environment keys without duplicating its identity.
        source = pipeline.read_text(encoding='utf-8')
        args_key = re.search(r"os.environ.get\('([^']+_CMAKE_ARGS)'", source).group(1)
        jobs_key = re.search(r"os.environ.get\('([^']+_BUILD_JOBS)'", source).group(1)
        env[args_key] = shlex.join(options) + ' ' + env.get(args_key, '')
        env[jobs_key] = str(jobs)
        if not (root / 'default.xbe').is_file():
            shutil.copyfile(root / 'game_files/default.xbe', root / 'default.xbe')
        if not (root / 'project/src/recomp/gen/recomp_funcs.h').is_file():
            call([sys.executable, pipeline, 'generate'], root, env)
        if extra:
            raise ValueError('This pipeline takes additional CMake options through its existing environment variable.')
        call([sys.executable, pipeline, 'build'], root, env)
    elif builder.is_file():
        command = [sys.executable, builder, '--game-files', root / 'game_files', '--jobs', jobs,
                   '--dxmt-dir' if sys.platform == 'darwin' else '--dxvk-native-dir', graphics,
                   '--vkd3d-dir', shader]
        call(command + extra, root, env)
    else:
        project = root / 'project'
        if not (project / 'src/recomp/gen/recomp_funcs.h').is_file():
            raise ValueError('This profile has no disc preparation pipeline. Supply private generated sources before building.')
        out = root / '.local/build'
        call(['cmake', '-S', project, '-B', out, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
              '-DCMAKE_C_COMPILER=clang', *options, *extra], root, env)
        call(['cmake', '--build', out, '--parallel', jobs], root, env)


def play(root, extra, env):
    player = root / 'tools/play.py'
    if player.is_file():
        call([sys.executable, player, '--game-files', root / 'game_files', *extra], root, env)
        return
    config = root / 'launcher.json'
    if config.is_file():
        data = json.loads(config.read_text(encoding='utf-8'))
        data.update(data.get('macos' if sys.platform == 'darwin' else 'linux', {}))
        executable = root / data['exe']
        location = root / 'build-location.txt'
        if location.is_file():
            executable = Path(location.read_text(encoding='utf-8').strip()) / executable.name
        command = [executable, root / 'game_files', *extra]
    else:
        text = (root / 'project/CMakeLists.txt').read_text(encoding='utf-8')
        target = re.search(r'project\((\w+)\s+C\)', text).group(1)
        executable = root / '.local/build' / target
        command = [executable, *extra]
        for key in ('HLE_PRIMARY', 'HLE_NOGPU', 'AUDIO_NATIVE', 'DS_NATIVE', 'INPUT_NATIVE',
                    'VBLANK', 'TITLE_KEVENTS', 'PORT_GUEST_LOCK', 'KEYBOARD'):
            env['RECOMP_' + key] = '1'
        env['RECOMP_GPU'] = 'd3d11'
    if not executable.is_file():
        raise ValueError('Build this profile first.')
    call(command, root, env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['build', 'play'])
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=int(os.environ.get('JOBS', '2')))
    args, extra = parser.parse_known_args()
    if os.name == 'nt':
        parser.error('Use the existing Windows scripts on Windows.')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    root = args.root.resolve()
    env = environment()
    if args.action == 'build':
        build(root, args.jobs, extra, env)
    else:
        play(root, extra, env)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print('Native profile: ' + str(error), file=sys.stderr)
        sys.exit(error.returncode if isinstance(error, subprocess.CalledProcessError) else 1)
