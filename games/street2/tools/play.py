"""Launch the privately built FIFA Street 2 port with the native graphics/audio/input profile."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import validate_game  # noqa: E402

# Native D3D8 -> D3D11/Vulkan, DirectSound -> host audio, XInput -> host pads; no console device threads.
PROFILE = {'RECOMP_HLE_PRIMARY': '1', 'RECOMP_HLE_NOGPU': '1', 'RECOMP_AUDIO_NATIVE': '1', 'RECOMP_DS_NATIVE': '1',
           'RECOMP_INPUT_NATIVE': '1', 'RECOMP_VBLANK': '1', 'RECOMP_TITLE_KEVENTS': '1', 'RECOMP_GUEST_LOCK': '1',
           'RECOMP_GPU': 'd3d11', 'RECOMP_KEYBOARD': '1'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-files', type=Path, required=True, help='Extracted disc folder named game_files')
    parser.add_argument('--local-dir', type=Path, default=ROOT / '.local')
    args = parser.parse_args()
    game_files = args.game_files.resolve()
    if game_files.name != 'game_files':
        parser.error('The extracted disc folder must be named game_files')
    problems = validate_game.validate(game_files)
    if problems:
        parser.error('; '.join(problems))
    executable = args.local_dir.resolve() / 'build' / ('fifastreet_recomp.exe' if os.name == 'nt' else 'fifastreet_recomp')
    if not executable.is_file():
        parser.error('Build first: python tools/build_local.py --game-files ' + str(game_files))
    env = {k: v for k, v in os.environ.items() if not k.startswith('RECOMP_')}
    env.update(PROFILE)
    logs = args.local_dir.resolve() / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    with (logs / 'stdout.log').open('wb') as stdout, (logs / 'stderr.log').open('wb') as stderr:
        raise SystemExit(subprocess.call([str(executable)], cwd=game_files.parent, env=env, stdout=stdout, stderr=stderr))


if __name__ == '__main__':
    main()
