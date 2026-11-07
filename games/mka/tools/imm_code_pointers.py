"""List immediates in the game's own code that point into .text at an address
that is no recompiled function start (recomp_dispatch.c) and is not a jump
target inside the function that uses it -- candidates for functions the
disassembly missed (reached only through a pointer built in code, e.g.
mov [ecx+0x238], 0xFA9A0). Prints address, the instruction that holds it,
and the first instruction at the target.

usage: python tools/imm_code_pointers.py   (from street2/, needs capstone)
"""
from pathlib import Path
import json, re, struct
import capstone
from capstone import x86

street2 = Path(__file__).resolve().parent.parent
d = (street2 / 'default.xbe').read_bytes()
base = struct.unpack_from('<I', d, 0x104)[0]
n = struct.unpack_from('<I', d, 0x11C)[0]
off = struct.unpack_from('<I', d, 0x120)[0] - base
secs = {}
for i in range(n):
    f, va, vs, raw, rs, nm = struct.unpack_from('<6I', d, off + i * 56)
    e = d.index(b'\0', nm - base)
    secs[d[nm - base:e].decode()] = (va, vs, raw, rs)
tva, tvs, traw, trs = secs['.text']
gen = street2 / 'project/src/recomp/gen'
known = set(int(m, 16) for m in re.findall(r'\{ 0x([0-9A-F]+)u,', (gen / 'recomp_dispatch.c').read_text()))
funcs = json.load(open(street2 / 'recomp-work/disasm/functions.json'))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

def code(a, size):
    return d[traw + a - tva: traw + a - tva + size]

found = {}
for fn in funcs:
    s, e = int(fn['start'], 16), int(fn['end'], 16)
    if not (tva <= s < tva + trs):
        continue
    for ins in md.disasm(code(s, e - s), s):
        if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
            continue
        for op in ins.operands:
            v = op.imm if op.type == x86.X86_OP_IMM else op.mem.disp if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 else None
            if v is None:
                continue
            v &= 0xFFFFFFFF
            if not (tva <= v < tva + trs) or v in known or s <= v < e:
                continue
            if op.type == x86.X86_OP_MEM:
                continue          # [abs] into .text is a data read (jump tables etc.)
            found.setdefault(v, []).append((ins.address, f'{ins.mnemonic} {ins.op_str}'))
for v in sorted(found):
    first = next(md.disasm(code(v, 16), v), None)
    tgt = f'{first.mnemonic} {first.op_str}' if first else '?'
    a, txt = found[v][0]
    print(f'{v:08X}  <- {a:08X} {txt:45s} | {tgt}  (x{len(found[v])})')
