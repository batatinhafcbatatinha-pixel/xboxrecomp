"""Derive a title's hle_game.h -- the guest addresses (GA_*) and recompiled-function names (FN_*) the shared D3D8
HLE (hle/d3d8) needs -- from its XBE alone, with rules learned from the games already ported.

Rules, most robust first:
  section  start/end of a named XBE section
  kthunk   slot of a kernel import (by ordinal) in the XBE's thunk table
  sym      an XbSymbolDatabase symbol (function or variable), optionally plus an offset
  sig      a masked window of library code holding or using the address (sigs.py)
  alias    always equal to another macro
A value is used only when every applicable rule agrees, and a symbol-plus-offset rule alone needs a second,
independent anchor: a wrong address is worse than a missing one. Rules that depend on the library build are only
applied to XBEs with the D3D8 build (and LTCG flavour) they were validated on.
"""
import bisect
import json
import re
from pathlib import Path

from . import sigs as S
from .xbe import Xbe

MAX_SYM_OFF = 0x2000


def read_symbols(path):
    """XbSymbolDatabaseCLI -e output -> {normalised name: VA}."""
    out = {}
    if not path or not Path(path).exists():
        return out
    for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        m = re.match(r'\s*(\w+?)(?:\(.*\))?\s*=\s*0x([0-9a-fA-F]+)\s*$', line)
        if m:
            name = re.sub(r'__FUN__\w+?__', '__', m.group(1))
            name = re.sub(r'^(\w+?)LTCG__', r'\1__', name)            # same symbol in the LTCG libraries
            name = re.sub(r'^(\w+?)__(VAR__)?D3D_', r'\1__\2', name)   # D3D8__D3D_X -> D3D8__X
            out.setdefault(name, int(m.group(2), 16))
    return out


def read_header(path):
    """GA_/FN_ values of an existing hle_game.h."""
    out = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        m = re.match(r'#define ((?:GA|FN)_\w+) (sub_|0x)([0-9A-Fa-f]+)[uU]?$', line)
        if m:
            out[m.group(1)] = int(m.group(3), 16)
    return out


class Title:
    def __init__(self, xbe_path, symbols_path=None):
        self.xbe = Xbe(xbe_path)
        self.sym = read_symbols(symbols_path)
        self.sym_sorted = sorted((v, k) for k, v in self.sym.items())
        self.thunk_va, self.ords = self.xbe.kernel_thunks()
        self.sec = {s['name']: s for s in self.xbe.sections}
        self.d3d = self.xbe.d3d_library()

    def value(self, r):
        k = r['kind']
        if k == 'section':
            s = self.sec.get(r['name'])
            return None if not s else s['va'] + (s['vsize'] if r['end'] else 0)
        if k == 'kthunk':
            return self.thunk_va + 4 * self.ords.index(r['ordinal']) if r['ordinal'] in self.ords else None
        if k == 'sym':
            return self.sym[r['name']] + r['offset'] if r['name'] in self.sym else None
        if k == 'sig':
            return S.resolve(self.xbe, r)
        return None

    def rules_for(self, value):
        out = []
        for s in self.xbe.sections:
            if value == s['va']:
                out.append({'kind': 'section', 'name': s['name'], 'end': 0})
            if value == s['va'] + s['vsize']:
                out.append({'kind': 'section', 'name': s['name'], 'end': 1})
        if self.thunk_va and 0 <= value - self.thunk_va < 4 * len(self.ords) and (value - self.thunk_va) % 4 == 0:
            out.append({'kind': 'kthunk', 'ordinal': self.ords[(value - self.thunk_va) // 4]})
        i = bisect.bisect_right(self.sym_sorted, (value, '￿'))
        near = sorted(self.sym_sorted[max(0, i - 6):i + 6], key=lambda vn: abs(value - vn[0]))
        exact, offset = [], []
        for va, name in near:
            off = value - va
            if off == 0:
                exact.append({'kind': 'sym', 'name': name, 'offset': 0})
            elif abs(off) <= MAX_SYM_OFF and (off > 0 or '__VAR__' in name):
                offset.append({'kind': 'sym', 'name': name, 'offset': off})
        return out + exact + S.signatures_for(self.xbe, value) + offset


def version_free(r):
    return r['kind'] in ('section', 'kthunk') or (r['kind'] == 'sym' and r['offset'] == 0)


def learn(ported):
    """ported: {name: (Title, {macro: value})} -> rules."""
    names = sorted({n for _, vals in ported.values() for n in vals})
    rules = {}
    for n in names:
        have = [g for g, (_, vals) in ported.items() if n in vals]
        best = []
        for src in have:
            for r in ported[src][0].rules_for(ported[src][1][n]):
                if r in best:
                    continue
                if all(ported[g][0].value(r) == ported[g][1][n] for g in have):
                    best.append(r)
                    if len(best) >= 3:
                        break
            if len(best) >= 3:
                break
        if best:
            rules[n] = {'rules': best, 'libs': sorted({ported[g][0].d3d for g in have})}
    # aliases: same value in every game that has both
    by_val = {}
    for n in names:
        key = tuple((g, ported[g][1].get(n)) for g in sorted(ported) if n in ported[g][1])
        by_val.setdefault(key, []).append(n)
    for grp in by_val.values():
        for n in grp:
            others = [m for m in grp if m != n and m in rules]
            if others:
                rules.setdefault(n, {'rules': [], 'libs': []})['alias'] = others[0]
    return rules


def resolve_all(title, rules):
    """{macro: value} for this title; conflicts and unresolved macros are left out."""
    res, conflicts = {}, []
    for n, e in rules.items():
        usable = [r for r in e['rules'] if title.d3d in e.get('libs', []) or version_free(r)]
        hits = [(r, v) for r, v in ((r, title.value(r)) for r in usable) if v is not None]
        got = {v for _, v in hits}
        strong = [r for r, _ in hits if not (r['kind'] == 'sym' and r['offset'] != 0)]
        anchors = {r['name'] for r, _ in hits if r['kind'] == 'sym'}
        if len(got) == 1 and (strong or len(anchors) >= 2):
            res[n] = got.pop()
        elif len(got) > 1:
            conflicts.append(n)
    for n, e in rules.items():
        if n not in res and e.get('alias') in res:
            res[n] = res[e['alias']]
    return res, conflicts


def header_text(res, game_id, title_name=''):
    lines = [f'/* {title_name}: values for the shared D3D8 HLE, derived from the XBE by tools.port.resolve',
             ' * (XDK symbols, library signatures, XBE headers). Macros missing here keep the HLE paths that',
             ' * need them off; a profile can add them. */',
             '#pragma once', '#include "hle_games.h"', f'#define HLE_GAME {game_id}', '']
    for n, v in sorted(res.items()):
        if n.startswith('FN_'):
            lines += [f'#define {n} sub_{v:08X}', f'#define {n}_GEN sub_{v:08X}_gen']
        else:
            lines.append(f'#define {n} 0x{v:X}u')
    return '\n'.join(lines) + '\n'
