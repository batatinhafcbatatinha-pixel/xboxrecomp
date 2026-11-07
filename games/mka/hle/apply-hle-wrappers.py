"""Route the game's D3D8 library calls through the HLE layer.

For every function listed in hle/hle_functions.json, the generated definition
`void sub_X(void)` is renamed `sub_X_gen` (declared in recomp_funcs.h too), and
project/src/hle_d3d8.c defines `sub_X` itself. Direct calls, the dispatch
table and indirect calls therefore all reach the HLE entry, which may call the
original library body as sub_X_gen. Idempotent; reapply after regeneration.
"""
from pathlib import Path
import json
import re

here = Path(__file__).resolve().parent
gen = here.parent / 'project/src/recomp/gen'
table = json.loads((here / 'hle_functions.json').read_text(encoding='utf-8'))
# DirectSound functions routed through project/src/hle_dsound.c.
ds_path = here / 'dsound_functions.json'
if ds_path.exists():
    table.update(json.loads(ds_path.read_text(encoding='utf-8')))
# D3D8 internals given native versions in project/src/hle_sync.c.
sync_path = here / 'sync_functions.json'
if sync_path.exists():
    table.update(json.loads(sync_path.read_text(encoding='utf-8')))
# XAPI XInput/XPP functions in C: project/src/hle_input.c.
input_path = here / 'input_functions.json'
if input_path.exists():
    table.update(json.loads(input_path.read_text(encoding='utf-8')))
# Title heap code serialised under RECOMP_GUEST_SMP: project/src/hle_smp_serial.c
# (generated with the table by hle/make-smp-serial.py).
serial_path = here / 'smp_serial_functions.json'
if serial_path.exists():
    table.update(json.loads(serial_path.read_text(encoding='utf-8')))
# Game functions observed by project/src/game_hooks.c (front-end screens).
hooks_path = here / 'game_hooks.json'
if hooks_path.exists():
    table.update(json.loads(hooks_path.read_text(encoding='utf-8')))
# Midway process scheduler (stack-switching coroutines) on host fibers:
# project/src/game_process.c.
proc_path = here / 'process_functions.json'
if proc_path.exists():
    table.update(json.loads(proc_path.read_text(encoding='utf-8')))
addrs = sorted(int(a, 16) for a in table)

renamed = 0
found = set()
wanted = set(addrs)
definition = re.compile(r'^void sub_([0-9A-F]{8})(_gen)?\(void\)(\s*\{|\s*$)', re.M)
for path in sorted(gen.glob('recomp_[0-9]*.c')):
    original = path.read_text(encoding='utf-8')

    def rename(m):
        global renamed
        a = int(m.group(1), 16)
        if a not in wanted:
            return m.group(0)
        found.add(a)
        if m.group(2):
            return m.group(0)
        renamed += 1
        return f'void sub_{m.group(1)}_gen(void){m.group(3)}'

    text = definition.sub(rename, original)
    if text != original:
        path.write_text(text, encoding='utf-8', newline='\n')

# Library functions this title never calls (no call sites, so the recompiler
# emitted no body): project/src/hle_dead_gen.c supplies a reporting _gen.
dead = {int(a, 16) for a, v in table.items() if v.get('dead')}
missing = [f'{a:08X}' for a in addrs if a not in found and a not in dead]
if missing:
    raise SystemExit(f'HLE: no generated definition for {missing}')

funcs = gen / 'recomp_funcs.h'
original = funcs.read_text(encoding='utf-8')
text = original
for a in addrs:
    decl = f'void sub_{a:08X}_gen(void);'
    if decl not in text:
        anchor = f'void sub_{a:08X}(void);'
        if anchor in text:
            text = text.replace(anchor, f'{anchor}\n{decl}', 1)
        else:  # dead entry: no generated declaration to sit next to
            end = text.rindex('#endif')
            text = text[:end] + f'{decl}\n' + text[end:]
if text != original:
    funcs.write_text(text, encoding='utf-8', newline='\n')
print(f'HLE wrappers: {len(addrs)} D3D functions routed ({renamed} renamed this pass)')
