"""Seeds for tools.disasm: code addresses referenced as a dword anywhere in the
image (data tables, immediates) that no detected function covers. Such code
(a function placed right after a ret with no padding, reached only through a
pointer) is otherwise never translated and every call to it fails at run time.
usage: orphan_seeds.py <xbe> <functions.json> <out seeds.json> [extra hex ...]"""
import bisect, json, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from addrmap import Image
import capstone
capstone_imm = capstone.x86.X86_OP_IMM

img = Image(sys.argv[1])
fs = json.loads(Path(sys.argv[2]).read_text())
rng = sorted((int(f['start'], 16), int(f['end'], 16)) for f in fs)
starts = [s for s, _ in rng]
fstarts = set(starts)
code = [(va, va + sz) for n, va, sz, _ in img.sections if n in ('.text', 'D3D', 'DSOUND', 'XGRPH', 'XPP')]

def in_code(v):
    return any(lo <= v < hi for lo, hi in code)

def covered(v):
    i = bisect.bisect_right(starts, v) - 1
    return i >= 0 and rng[i][0] <= v < rng[i][1]

mem = img.mem
cands = {}
for off in range(0, len(mem) - 3):
    v = struct.unpack_from('<I', mem, off)[0]
    if v < 0x11000 or v >= 0x2A9020 or not in_code(v) or v in fstarts:
        continue
    o = v - img.base
    # inside a detected range only after alignment padding (int3/nop run)
    from_table = ((off & 3) == 0 and img.section(img.base + off) in ('.rdata', '.data')
                  and any(struct.unpack_from('<I', mem, off + d)[0] in fstarts for d in (-4, 4)))
    if covered(v) and not from_table and not (mem[o - 1] in (0xCC, 0x90) and mem[o - 2] == mem[o - 1]):
        continue
    cands.setdefault(v, []).append((img.base + off, from_table))
seeds = []
for v, refs in sorted(cands.items()):
    # must decode as a few sane instructions and follow a terminal instruction
    prev = mem[v - img.base - 1]
    prev3 = mem[v - img.base - 3]
    tables = sum(1 for _, t in refs if t)
    if not (prev in (0xC3, 0xCC, 0x90) or prev3 == 0xC2 or mem[v - img.base - 5] == 0xE9 or tables >= 2):
        continue
    ins = list(img.cs.disasm(img.read(v, 32), v))
    if len(ins) < 3 or any(i.mnemonic in ('(bad)', 'int3') for i in ins[:3]):
        continue
    seeds.append(v)
for x in sys.argv[4:]:
    seeds.append(int(x, 16))
seeds = sorted(set(seeds))
Path(sys.argv[3]).write_text(json.dumps([{'start': f'0x{v:08X}'} for v in seeds], indent=0))
print(len(cands), 'uncovered referenced code addresses,', len(seeds), 'seeds')
for v in (0x239C1A, 0x23A4C3, 0x21421A, 0xDCF10, 0x1D864E):
    print(hex(v), v in seeds)

# Second rule: code immediates (push imm32 / mov r32,imm32 / mov m32,imm32)
# pointing into code that is not a function start: function pointers built in
# code (a function placed right after a data block is invisible otherwise).
imm_seeds = set()
for fs_, fe_ in rng:
    for ins in img.cs.disasm(img.read(fs_, fe_ - fs_), fs_):
        if ins.mnemonic not in ('push', 'mov'):
            continue
        for op in ins.operands:
            if op.type != capstone_imm:
                continue
            v = op.imm & 0xFFFFFFFF
            if v in fstarts or not in_code(v) or v >= 0x2A9020:
                continue
            if fs_ <= v < fe_:
                continue   # a label of the same function
            dec = list(img.cs.disasm(img.read(v, 24), v))
            if len(dec) >= 2:
                imm_seeds.add(v)
print(len(imm_seeds), 'code-immediate seeds', 0x1D8079 in imm_seeds)
Path(sys.argv[3]).write_text(json.dumps([{'start': f'0x{v:08X}'} for v in sorted(set(seeds) | imm_seeds)], indent=0))
