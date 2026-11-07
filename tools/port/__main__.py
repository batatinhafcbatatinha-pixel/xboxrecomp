"""
tools/port -- the generic port pipeline (work in progress).

  py -3 -m tools.port symbols  <xbe> -o symbols.txt [--cli XbSymbolDatabaseCLI]
  py -3 -m tools.port resolve  <xbe> --symbols symbols.txt [--header hle_game.h] [--expect old_hle_game.h]
  py -3 -m tools.port learn    --game name=<xbe>,<symbols>,<hle_game.h> ...      (writes hle/d3d8/hle_rules.json)
  py -3 -m tools.port eval     --game name=<xbe>,<symbols>,<hle_game.h> ...      (leave-one-out accuracy)

resolve derives the values the shared D3D8 HLE needs from the XBE alone (see resolve.py); learn rebuilds the rules
from the ported games (their XBEs stay on your PC; only the rules are kept).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from . import resolve as R

ROOT = Path(__file__).resolve().parents[2]
RULES = ROOT / 'hle' / 'd3d8' / 'hle_rules.json'


def parse_games(items):
    out = {}
    for it in items:
        name, rest = it.split('=', 1)
        xbe, sym, hdr = rest.split(',')
        out[name] = (xbe, sym, hdr)
    return out


def load(games):
    return {g: (R.Title(x, s), R.read_header(h)) for g, (x, s, h) in games.items()}


def compare(res, want):
    right = sum(1 for n in want if res.get(n) == want[n])
    wrong = [n for n in want if n in res and res[n] != want[n]]
    return right, wrong, [n for n in want if n not in res]


def find_cli(explicit):
    for c in (explicit, os.environ.get('XBSDB_CLI'), shutil.which('XbSymbolDatabaseCLI'),
              ROOT / '.local' / 'tools' / 'xbsdb' / 'XbSymbolDatabaseCLI.exe'):
        if c and Path(c).exists():
            return str(c)
    raise SystemExit('XbSymbolDatabaseCLI not found: build https://github.com/Cxbx-Reloaded/XbSymbolDatabase '
                     '(MIT) and pass --cli or set XBSDB_CLI')


def main():
    ap = argparse.ArgumentParser(prog='tools.port', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('symbols'); p.add_argument('xbe'); p.add_argument('-o', required=True); p.add_argument('--cli')
    p = sub.add_parser('resolve'); p.add_argument('xbe'); p.add_argument('--symbols')
    p.add_argument('--rules', default=str(RULES)); p.add_argument('--header'); p.add_argument('--expect')
    p.add_argument('--game-id', default='HLE_GAME_GENERIC'); p.add_argument('--title', default='')
    for name in ('learn', 'eval'):
        p = sub.add_parser(name); p.add_argument('--game', action='append', required=True)
        p.add_argument('--rules', default=str(RULES))
    a = ap.parse_args()

    if a.cmd == 'symbols':
        out = subprocess.run([find_cli(a.cli), a.xbe, '-e'], capture_output=True, text=True)
        Path(a.o).write_text(out.stdout, encoding='utf-8')
        print(f'{sum(1 for l in out.stdout.splitlines() if " = 0x" in l)} symbols -> {a.o}')
    elif a.cmd == 'resolve':
        rules = json.loads(Path(a.rules).read_text(encoding='utf-8'))
        title = R.Title(a.xbe, a.symbols)
        res, conflicts = R.resolve_all(title, rules)
        print(f'{title.d3d}: {len(res)} of {len(rules)} values resolved, {len(conflicts)} conflicts')
        if a.expect:
            right, wrong, miss = compare(res, R.read_header(a.expect))
            print(f'vs {a.expect}: {right} right, {len(wrong)} wrong {wrong[:8]}, {len(miss)} unresolved')
        if a.header:
            Path(a.header).write_text(R.header_text(res, a.game_id, a.title), encoding='utf-8')
    elif a.cmd == 'learn':
        rules = R.learn(load(parse_games(a.game)))
        Path(a.rules).write_text(json.dumps(rules, indent=0), encoding='utf-8')
        print(f'{len(rules)} macros with rules -> {a.rules}')
    else:
        games = load(parse_games(a.game))
        for g in games:
            rules = R.learn({k: v for k, v in games.items() if k != g})
            res, conflicts = R.resolve_all(games[g][0], rules)
            right, wrong, miss = compare(res, games[g][1])
            print(f'{g}: {len(games[g][1])} macros, {right} right, {len(wrong)} wrong {wrong[:6]}, '
                  f'{len(miss)} unresolved, {len(conflicts)} conflicts', flush=True)


if __name__ == '__main__':
    sys.exit(main())
