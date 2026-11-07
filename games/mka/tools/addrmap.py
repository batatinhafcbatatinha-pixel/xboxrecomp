"""Map FIFA Street 1 guest addresses to FIFA Street 2 ones.

Both titles link the same XDK 5849 libraries and much of the same EA engine,
at different addresses. Functions are matched by a normalized instruction
signature (mnemonics, operands with image addresses blanked, branch offsets
inside the function, instruction sizes); a unique signature on both sides is a
match, and calls/data operands of matched pairs seed further matches and the
data map. Iterates until stable.

usage:
  python -I addrmap.py build <s1.xbe> <s1 funcs> <s2.xbe> <s2 funcs> <out.json>
  python -I addrmap.py query <map.json> <addr> [<addr> ...]
funcs = recomp_funcs.h (sub_XXXXXXXX names) or disasm functions.json.
"""
import bisect
import collections
import difflib
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

import capstone


class Image:
    def __init__(self, path):
        blob = Path(path).read_bytes()
        assert blob[:4] == b'XBEH'
        self.base = struct.unpack_from('<I', blob, 0x104)[0]
        self.size = struct.unpack_from('<I', blob, 0x10C)[0]
        nsec = struct.unpack_from('<I', blob, 0x11C)[0]
        shdr = struct.unpack_from('<I', blob, 0x120)[0] - self.base
        self.mem = bytearray(self.size)
        self.sections = []
        for i in range(nsec):
            flags, va, vsz, raw, rsz, name_va = struct.unpack_from('<6I', blob, shdr + i * 56)
            n = blob[name_va - self.base:].split(b'\0', 1)[0].decode()
            off = va - self.base
            take = min(rsz, vsz, self.size - off)
            self.mem[off:off + take] = blob[raw:raw + take]
            self.sections.append((n, va, vsz, bool(flags & 4)))
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.cs.detail = True

    def inimg(self, v):
        return self.base <= v < self.base + self.size

    def section(self, v):
        for n, va, vsz, _ in self.sections:
            if va <= v < va + vsz:
                return n
        return None

    def code(self, v):
        return self.section(v) in ('.text', 'D3D', 'DSOUND', 'XGRPH', 'XPP', 'DOLBY')

    def read(self, va, n):
        o = va - self.base
        return bytes(self.mem[o:o + n])


def load_funcs(path):
    text = Path(path).read_text(encoding='utf-8', errors='replace')
    if path.endswith('.json'):
        d = json.loads(text)
        items = d if isinstance(d, list) else d.get('functions', list(d.values()))
        out = set()
        for f in items:
            s = f.get('start') if isinstance(f, dict) else f
            out.add(int(s, 16) if isinstance(s, str) else int(s))
        return sorted(out)
    return sorted({int(m, 16) for m in re.findall(r'\bsub_([0-9A-Fa-f]{8})\b', text)})


PAD = {'int3', 'nop'}
BR = 1 << 32


def signature(img, start, end):
    """Normalized tokens, operand image refs, call targets, instruction offsets."""
    code = img.read(start, max(0, min(end - start, 0x20000)))
    toks, refs, calls, offs = [], [], [], []
    for ins in img.cs.disasm(code, start):
        m = ins.mnemonic
        refs_here = []
        ops = []
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_IMM:
                v = op.imm & 0xFFFFFFFF
                if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
                    if start <= v < end:
                        ops.append(f'L{v - start:x}')
                    else:
                        ops.append('X')
                        if ins.group(capstone.CS_GRP_CALL) or m == 'jmp':
                            calls.append((len(toks), v))
                        refs_here.append(v | BR)
                elif img.inimg(v):
                    ops.append('A')
                    refs_here.append(v)
                else:
                    ops.append(f'i{v:x}')
            elif op.type == capstone.x86.X86_OP_MEM:
                d = op.mem.disp & 0xFFFFFFFF
                if img.inimg(d):
                    ops.append(f'[{op.mem.base},{op.mem.index},{op.mem.scale},A]')
                    refs_here.append(d)
                else:
                    ops.append(f'[{op.mem.base},{op.mem.index},{op.mem.scale},{op.mem.disp:x}]')
            else:
                ops.append(f'r{op.reg}')
        toks.append(f'{m}/{ins.size}/' + ','.join(ops))
        refs.append(refs_here)
        offs.append(ins.address - start)
    # trailing alignment padding is layout, not code
    while toks and (toks[-1].split('/')[0] in PAD or toks[-1].startswith('lea/') and not refs[-1]
                    and toks[-1].count('r') >= 1 and len(toks) > 1 and toks[-2].startswith(('ret', 'jmp'))):
        toks.pop(); refs.pop(); offs.pop()
    h = hashlib.sha1('\n'.join(toks).encode()).hexdigest()
    return h, refs, calls, offs, len(toks), toks


def xbsdb(p):
    d = {}
    for l in Path(p).read_text().splitlines():
        m = re.match(r'(\w+?)__(FUN|VAR)__(?:\w+?__)?(\w+?)(\(.*\))? = 0x([0-9a-f]+)', l.strip())
        if m:
            d[m.group(1) + '.' + m.group(3)] = (int(m.group(5), 16), m.group(2))
    return d


def build(a_xbe, a_funcs, b_xbe, b_funcs, out, a_sym=None, b_sym=None):
    A, B = Image(a_xbe), Image(b_xbe)
    fa, fb = load_funcs(a_funcs), load_funcs(b_funcs)
    seeds = []
    if a_sym:
        sa_, sb_ = xbsdb(a_sym), xbsdb(b_sym)
        seeds = [(sa_[k][0], sb_[k][0], sa_[k][1]) for k in sa_ if k in sb_ and A.inimg(sa_[k][0])]
        fa = sorted(set(fa) | {a for a, _, t in seeds if t == 'FUN'})
        fb = sorted(set(fb) | {b for _, b, t in seeds if t == 'FUN'})

    def sigs(img, funcs):
        res = {}
        for i, s in enumerate(funcs):
            e = funcs[i + 1] if i + 1 < len(funcs) else s + 0x4000
            if not img.section(s):
                continue
            # do not run into the next section
            res[s] = signature(img, s, e)
        return res
    sa, sb = sigs(A, fa), sigs(B, fb)
    fmap_b = set()
    by_a, by_b = collections.defaultdict(list), collections.defaultdict(list)
    for s, v in sa.items():
        by_a[v[0]].append(s)
    for s, v in sb.items():
        by_b[v[0]].append(s)

    fmap = {}
    for h, la in by_a.items():
        lb = by_b.get(h, [])
        if len(la) == 1 and len(lb) == 1 and sa[la[0]][4] >= 3:
            fmap[la[0]] = lb[0]
            fmap_b.add(lb[0])
    # XbSymbolDatabase names on both sides are authoritative.
    for na, nb, t in seeds:
        if t == 'FUN' and na in sa and nb in sb:
            fmap[na] = nb
            fmap_b.add(nb)

    data = collections.defaultdict(collections.Counter)
    inner = {}       # a function -> list of (a offset, b offset) for aligned instructions
    done = set()
    changed = True
    rounds = 0
    while changed and rounds < 30:
        changed = False
        rounds += 1
        for x, y in list(fmap.items()):
            if x in done:
                continue
            done.add(x)
            ta, tb = sa[x], sb[y]
            if ta[0] == tb[0]:
                pairs = [(i, i) for i in range(ta[4])]
            else:
                sm = difflib.SequenceMatcher(None, ta[5], tb[5], autojunk=False)
                pairs = [(blk.a + k, blk.b + k) for blk in sm.get_matching_blocks() for k in range(blk.size)]
            inner[x] = [(ta[3][i], tb[3][j]) for i, j in pairs]
            for i, j in pairs:
                for u, v in zip(ta[1][i], tb[1][j]):
                    br = u & BR
                    u &= 0xFFFFFFFF
                    v &= 0xFFFFFFFF
                    if u in sa and v in sb:
                        if u not in fmap and v not in fmap_b:
                            fmap[u] = v
                            fmap_b.add(v)
                            changed = True
                    elif u not in sa and not br:
                        data[u][v] += 1
    dmap = {}
    conflicts = 0
    for a_, b_, t in seeds:
        if t == 'VAR':
            data[a_][b_] += 1000
    for u, c in data.items():
        (v, n), = c.most_common(1)
        if len(c) > 1:
            conflicts += 1
        dmap[u] = v
    res = {
        'functions': {f'{k:08X}': f'{v:08X}' for k, v in sorted(fmap.items())},
        'data': {f'{k:08X}': f'{v:08X}' for k, v in sorted(dmap.items())},
        'stats': dict(a_funcs=len(sa), b_funcs=len(sb), matched=len(fmap), data=len(dmap),
                      data_conflicts=conflicts, rounds=rounds),
        'a_funcs': [f'{s:08X}' for s in fa],
        'inner': {f'{k:08X}': v for k, v in inner.items() if sa[k][0] != sb[fmap[k]][0]},
        'same': [f'{k:08X}' for k in fmap if sa[k][0] == sb[fmap[k]][0]],
        'a_sections': A.sections, 'b_sections': B.sections,
    }
    Path(out).write_text(json.dumps(res, indent=0))
    print(res['stats'])


def query(mapfile, addrs, quiet=False):
    m = json.loads(Path(mapfile).read_text())
    fm = {int(k, 16): int(v, 16) for k, v in m['functions'].items()}
    dm = {int(k, 16): int(v, 16) for k, v in m['data'].items()}
    af = [int(s, 16) for s in m['a_funcs']]
    dkeys = sorted(dm)
    same = {int(s, 16) for s in m['same']}
    inner = {int(k, 16): [tuple(p) for p in v] for k, v in m['inner'].items()}
    secs = m['a_sections']

    def code(v):
        return any(va <= v < va + sz and n in ('.text', 'D3D', 'DSOUND', 'XGRPH', 'XPP', 'DOLBY')
                   for n, va, sz, _ in secs)
    out = {}
    for a in addrs:
        if a in fm:
            out[a] = (fm[a], 'func')
            continue
        if a in dm:
            out[a] = (dm[a], 'data')
            continue
        i = bisect.bisect_right(af, a) - 1
        if i >= 0 and af[i] in fm and a < (af[i + 1] if i + 1 < len(af) else a + 1):
            f = af[i]
            if f in same:
                out[a] = (fm[f] + a - f, f'in sub_{f:08X}+{a - f:X}')
                continue
            pr = dict(inner.get(f, []))
            if a - f in pr:
                out[a] = (fm[f] + pr[a - f], f'in CHANGED sub_{f:08X}+{a - f:X} (aligned)')
            else:
                out[a] = (None, f'in CHANGED sub_{f:08X}+{a - f:X} (not aligned)')
            continue
        if code(a):
            out[a] = (None, 'unmapped code')
            continue
        j = bisect.bisect_right(dkeys, a) - 1
        if j >= 0 and a - dkeys[j] < 0x200:
            k = dkeys[j]
            out[a] = (dm[k] + a - k, f'near data {k:08X}+{a - k:X}')
            continue
        out[a] = (None, 'unmapped')
    if not quiet:
        for a, (v, why) in out.items():
            print(f'{a:08X} -> {"%08X" % v if v is not None else "????????"}  {why}')
    return out


if __name__ == '__main__':
    if sys.argv[1] == 'build':
        build(*sys.argv[2:9])
    else:
        query(sys.argv[2], [int(x, 16) for x in sys.argv[3:]])
