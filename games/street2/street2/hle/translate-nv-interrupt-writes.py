"""Street 2: translate the six NV2A status writes (PGRAPH/PCRTC +0x100) of the
D3D library ISR/DPC to hardware W1C semantics, as street1 does. Idempotent."""
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent / 'project/src/recomp/gen'
pattern = r'MEM32\(((?:esi|edi|ecx|eax) \+ 0x(?:400100|600100))\) = ([^;]+);'
declaration = 'extern void xbox_Nv2aAckInterrupt(uint32_t address, uint32_t value);'
total = calls = 0
for path in sorted(root.glob('recomp_00*.c')):
    original = path.read_text(encoding='utf-8')
    text, changed = re.subn(pattern, r'xbox_Nv2aAckInterrupt(\1, \2);', original)
    n = len(re.findall(r'xbox_Nv2aAckInterrupt\((?:esi|edi|ecx|eax) \+ 0x(?:400100|600100),', text))
    if n and declaration not in text:
        text = text.replace('#include <math.h>', '#include <math.h>\n' + declaration, 1)
    if text != original:
        path.write_text(text, encoding='utf-8', newline='\n')
    total += changed
    calls += n
if calls != 6:
    raise SystemExit(f'expected 6 NV2A status writes, found {calls}')
print(f'NV2A W1C: six status writes verified, {total} translated')
