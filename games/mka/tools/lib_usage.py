"""List SDK library functions (XbSymbolDatabase names) that game code (.text) calls, and whether HLE covers them.

usage: python -I lib_usage.py <recomp-work dir> <hle dir>
"""
import bisect
import json
import re
import sys
from collections import Counter
from pathlib import Path

work, hle = Path(sys.argv[1]), Path(sys.argv[2])
funcs = json.loads((work / 'disasm/functions.json').read_text())
starts = sorted(int(f['start'], 16) for f in funcs)
section = {int(f['start'], 16): f.get('section') for f in funcs}
xrefs = json.loads((work / 'disasm/xrefs.json').read_text())
if isinstance(xrefs, dict):
    xrefs = xrefs.get('xrefs', list(xrefs.values()))
covered = set()
for name in ('hle_functions.json', 'dsound_functions.json', 'sync_functions.json', 'input_functions.json'):
    p = hle / name
    if p.exists():
        covered |= {int(a, 16) for a in json.loads(p.read_text())}
names = {}
for line in (hle / 'xbsdb_symbols.txt').read_text().splitlines():
    m = re.match(r'(\w+?)__FUN__(?:\w+?__)?(\w+?)(\(.*\))? = 0x([0-9a-f]+)', line.strip())
    if m:
        names[int(m.group(4), 16)] = (m.group(1), m.group(2))


def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 else None


sites = Counter()
for x in xrefs:
    if x.get('type') not in ('call', 'jump'):
        continue
    t = int(str(x.get('to') or x.get('target')), 16) if isinstance(x.get('to') or x.get('target'), str) else (x.get('to') or x.get('target'))
    f = int(str(x.get('from') or x.get('source')), 16) if isinstance(x.get('from') or x.get('source'), str) else (x.get('from') or x.get('source'))
    if t in names and section.get(owner(f)) == '.text':
        sites[t] += 1
usage = {}
for a, n in sorted(sites.items()):
    lib, name = names[a]
    usage[f'{a:08X}'] = n
    if lib in ('D3D8', 'DSOUND', 'XAPILIB', 'D3D8LTCG', 'XGRAPHC'):
        print(f'{"HLE " if a in covered else "    "}{lib:8} {name:45} {a:08X} sites={n}')
(hle / 'lib_usage.json').write_text(json.dumps(usage))
