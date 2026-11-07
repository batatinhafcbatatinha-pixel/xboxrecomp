"""Rewrite Street 1 guest addresses in street2/project/src to Street 2 ones.

usage: python -I remap_sources.py <map.json> <overrides.json> [--apply] files...
Every hex literal 0x0011000..0x0036FFFF (5-8 hex digits) is looked up; the
report lists file:line, old, new and the reason. overrides.json maps
"FILE:OLD" or "OLD" -> "NEW" | "keep" for literals that are not addresses or
that the matcher could not map. Without --apply only the report is printed.
Files are only rewritten when every literal is resolved.
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import addrmap  # noqa: E402

LIT = re.compile(r'(?:\b0x(0*[0-9A-Fa-f]{5,6})(u?)\b|(?<![0-9A-Za-z])(00[0-9A-F]{6})(?![0-9A-Za-z]))')


def main():
    mapfile, ovfile = sys.argv[1], sys.argv[2]
    apply = '--apply' in sys.argv
    files = [f for f in sys.argv[3:] if f != '--apply']
    ov = json.loads(Path(ovfile).read_text()) if Path(ovfile).exists() else {}
    found = {}
    for f in files:
        for ln, line in enumerate(Path(f).read_text(encoding='utf-8').splitlines(), 1):
            for m in LIT.finditer(line):
                v = int(m.group(1) or m.group(3), 16)
                if 0x11000 <= v < 0x370000:
                    found.setdefault(v, []).append((f, ln, line.strip()))
    res = addrmap.query(mapfile, sorted(found), quiet=True)
    unresolved = 0
    plan = {}
    for v in sorted(found):
        new, why = res[v]
        for f, ln, line in found[v]:
            key_f = f'{Path(f).name}:{v:X}'
            o = ov.get(key_f, ov.get(f'{v:X}'))
            if o == 'keep':
                n2, w2 = v, 'keep (override)'
            elif o:
                n2, w2 = int(o, 16), 'override'
            else:
                n2, w2 = new, why
            if n2 is None:
                unresolved += 1
            plan[(f, v)] = n2
            print(f'{Path(f).name}:{ln}: {v:08X} -> {"%08X" % n2 if n2 is not None else "????????"} [{w2}]  {line[:110]}')
    print(f'{len(found)} distinct literals, {unresolved} unresolved uses')
    if apply and not unresolved:
        for f in files:
            text = Path(f).read_text(encoding='utf-8')

            def sub(m):
                v = int(m.group(1) or m.group(3), 16)
                if (f, v) not in plan or plan[(f, v)] == v:
                    return m.group(0)
                if m.group(3):
                    return f'{plan[(f, v)]:08X}'
                width = len(m.group(1))
                return f'0x{plan[(f, v)]:0{width}X}{m.group(2)}'
            new = LIT.sub(sub, text)
            if new != text:
                Path(f).write_text(new, encoding='utf-8', newline='\n')
        print('applied')


if __name__ == '__main__':
    main()
