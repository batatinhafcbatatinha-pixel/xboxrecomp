"""Locate Street 2 data addresses in the MKA image.

usage: python -I find_data.py <s2.xbe> <s2 functions.json> <mka.xbe> <mka functions.json> <addrmap.json> ADDR...
For each address: (1) initialised bytes around it searched in MKA (unique hit);
(2) every Street 2 instruction that names it, with the MKA function the map pairs
with its function, and the operand found at the same instruction index there.
"""
import bisect
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from addrmap import Image, load_funcs  # noqa: E402

s2x, s2f, mkx, mkf, mapf = sys.argv[1:6]
targets = [int(x, 16) for x in sys.argv[6:]]
A, B = Image(s2x), Image(mkx)
def ends(l):
    return {a: min(b, a + 0x4000) for a, b in zip(l, l[1:] + [l[-1] + 0x100])}


fa, fb = ends(load_funcs(s2f)), ends(load_funcs(mkf))
m = json.loads(Path(mapf).read_text())
fmap = {int(k, 16): int(v, 16) for k, v in m['functions'].items()}
starts = sorted(fa)


def insns(img, funcs, start):
    end = funcs[start]
    return list(img.cs.disasm(bytes(img.read(start, end - start)), start))


def operands(ins):
    vals = []
    for op in ins.operands:
        if op.type == 2:      # imm
            vals.append(op.imm & 0xFFFFFFFF)
        elif op.type == 3:    # mem
            vals.append(op.mem.disp & 0xFFFFFFFF)
    return vals


Bmem = bytes(B.mem)
for t in targets:
    print(f'== {t:08X} section {A.section(t)}')
    for lo, hi in ((16, 16), (0, 16), (8, 8), (0, 8)):
        pat = bytes(A.read(t - lo, lo + hi))
        if pat.count(0) == len(pat):
            continue
        hits = []
        i = Bmem.find(pat)
        while i >= 0 and len(hits) < 3:
            hits.append(B.base + i + lo)
            i = Bmem.find(pat, i + 1)
        if len(hits) == 1:
            print(f'  bytes[-{lo},+{hi}] unique -> {hits[0]:08X}')
            break
        if hits:
            print(f'  bytes[-{lo},+{hi}] {len(hits)} hits {[hex(h) for h in hits]}')
    seen = 0
    for s in starts:
        if not A.code(s):
            continue
        try:
            ia = insns(A, fa, s)
        except Exception:
            continue
        for k, ins in enumerate(ia):
            for v in operands(ins):
                if t - 0x40 <= v <= t + 0x40 and seen < 12:
                    seen += 1
                    other = fmap.get(s)
                    note = ''
                    if other is not None and other in fb:
                        ib = insns(B, fb, other)
                        if k < len(ib) and ib[k].mnemonic == ins.mnemonic:
                            ov = operands(ib[k])
                            note = f' -> MKA sub_{other:08X}[{k}] {ib[k].mnemonic} {ib[k].op_str} (delta {[hex(x - v) for x in ov]})'
                        else:
                            note = f' -> MKA sub_{other:08X} (index differs)'
                    print(f'  {ins.address:08X} in sub_{s:08X}: {ins.mnemonic} {ins.op_str} [{v - t:+#x}]{note}')
