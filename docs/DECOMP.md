# Using xboxrecomp for Decompilation

> Part of the [Documentation Index](INDEX.md)

This is a different job from the rest of the toolkit. [GETTING_STARTED.md](GETTING_STARTED.md)
is about turning an Xbox binary into a native `.exe` that runs. This is about
splitting one into per-function assembly you can decompile by hand, matching the
shipped encoding byte for byte.

You do not need the recompiler for this, and you never have to run it. The
analysis half — where the functions are, what shape each one is, what it calls,
what its calling convention is — is the same either way, and `tools.split`
hands it to you in the shape a decomp wants.

## What you get

```
build/split/
  manifest.json              every function: address, size, signature, call graph, file
  asm/
    text/sub_00011000.s      one file per function
    text/sub_00011040.s
    D3D/sub_000B2630.s
```

Each `.s` looks like this (an invented function, written for this document; it is not taken from any title):

```asm
; sub_00011000
; 0x00011000 - 0x0001100F  (16 bytes, 9 instructions)
; section .text, detected by call_target (confidence 0.90)
; cdecl, 0 params, returns int_or_void, frame fpo_leaf
; called by: sub_00011040
;
; The bytes below are the original encoding, emitted as data so
; this assembles byte-identically. The disassembly is the comment
; beside each one -- read that, write the C, delete this file.

sub_00011000:
    db 0x56                                       ; 00011000  push esi
    db 0x8B, 0xF1                                 ; 00011001  mov esi, ecx
    db 0x8B, 0x46, 0x04                           ; 00011003  mov eax, [esi+4]
    db 0x85, 0xC0                                 ; 00011006  test eax, eax
    db 0x74, 0x04                                 ; 00011008  je 0x1100e
    db 0x40                                       ; 0001100A  inc eax
    db 0x89, 0x46, 0x04                           ; 0001100B  mov [esi+4], eax

.L_0001100E:
    db 0x5E                                       ; 0001100E  pop esi
    db 0xC3                                       ; 0001100F  ret
```

Which reads straight out as:

```c
void Counter_Bump(Counter *c)
{
    if (c->value)
        c->value++;
}
```

## Why the bytes are `db` and the mnemonics are comments

This is the one design decision worth explaining, because it is the opposite of
what N64 decomp projects do.

On MIPS you can round-trip mnemonics through an assembler and get the original
encoding back — fixed width, one encoding per instruction. That is why splat
emits real `.s` and the workflow reads so cleanly.

x86 is not that. `mov eax, ecx` has two encodings, immediates have short forms,
and which one MSVC picked is **not recoverable from the mnemonic**. Reassembling
a listing gives you code that runs identically and does not *match* — and
matching is the entire point of a decomp, because it is the only oracle you have.

So the bytes go in as data and the disassembly rides alongside as a comment. The
file assembles to the shipped encoding by construction, and it is still what you
read while writing the C. When a function matches, delete its `.s`.

This is verified, not asserted: every emitted file is compared against the
binary. On the Xbox Dashboard's `.text`, **2,254 of 2,254 functions are
byte-identical**, including the ones with MSVC switch tables parked mid-body
(`memcpy` has two) which come out as labelled data runs in place.

## The pipeline

Everything runs from inside the toolkit clone.

```bash
# 1. Parse. --json is required and the name matters: later steps look for
#    <xbe stem>_analysis.json beside the XBE.
py -3 -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json

# 2. Find the functions.
py -3 -m tools.disasm game_files/default.xbe
#    --text-only for just .text; --extra-sections XIPS,DOLBY for code the XBE
#    does not mark executable.

# 3. Recover calling conventions, parameter counts, return types, frame shapes.
#    Optional, but it is what puts a signature line on every function.
py -3 -m tools.abi_analysis game_files/default.xbe

# 4. Optional: real names instead of sub_XXXXXXXX (see below).

# 5. Split.
py -3 -m tools.split game_files/default.xbe --out build/split
```

Useful flags on step 5:

| Flag | Effect |
|---|---|
| `--section .text` | only that section; repeatable |
| `--text-only` | analyse only `.text` |
| `--extra-sections XIPS,DOLBY` | treat non-executable sections as code |
| `-n 50` | first 50 functions, for a look before committing to 40,000 files |
| `--abi PATH` | a different `abi_functions.json` |

## Getting real names first

Do this **before** splitting. Names live in `functions.json`, and both the
splitter and the recompiler emit whatever name is on the entry — so applying
them first carries them into the filenames, the labels and the call annotations.

```bash
XBE=game_files/default.xbe tools/ghidra_naming/run_ghidra.sh
py -3 tools/ghidra_naming/merge_names.py --apply
```

Ghidra's Function ID databases recognise the statically linked MSVC CRT and XDK
helpers — `malloc`, `_ftol`, `__SEH_prolog`, the 64-bit math helpers — which are
exactly the functions you would otherwise waste days identifying by hand. Expect
a few hundred, not thousands: proprietary game code has no signatures to match.
Full detail in [tools/ghidra_naming/README.md](../tools/ghidra_naming/README.md).

If your binary shipped with RTTI left on, you can do far better. Half-Life 2
(Xbox) yields 2,336 classes, 2,932 vtables and 12,288 virtual method addresses
with class names and an inheritance graph — and 84% of those class names are
declared in the publicly released Source SDK 2013, with an exact file to read.
That is address → class → real `.cpp`. See `tools/rtti` and the hl2-recomp
project's `docs/symbols.md`.

## The manifest

`manifest.json` is the machine-readable half, for driving a build system:

```json
{
  "name": "sub_00011000",
  "address": "0x00011000",
  "end": "0x00011010",
  "size": 16,
  "section": ".text",
  "instructions": 9,
  "has_prologue": false,
  "detection_method": "call_target",
  "confidence": 0.9,
  "called_by": ["0x00011040"],
  "calls_to": [],
  "calling_convention": "cdecl",
  "estimated_params": 0,
  "return_hint": "int_or_void",
  "frame_type": "fpo_leaf",
  "stack_frame_size": 0,
  "file": "asm/text/sub_00011000.s"
}
```

`called_by` and `calls_to` give you the call graph without re-deriving it, which
is what you want for picking an order to work in — leaves first, or whatever
reaches the function you actually care about.

Treat `confidence` and `detection_method` as real information. A function found
by `prologue` is a function; one found by `tail_jump_alias` or `imm_ref_target`
is a good guess, and in a region where data was decoded as code it can be
neither. `heuristic_confidence` on the ABI fields says the same about the
signature — `0.5` with `"low-confidence default"` means nothing was inferred.

## Checking a function without matching it yet

The toolkit has something most decomp setups do not: a behavioural oracle.

Xbox code is 32-bit x86 and the test harness is a 32-bit x86 process, so the
shipped machine code is not merely readable, it is **executable**.
`tools/conformance/xbe_run.py` maps the XBE where it was linked, calls one of its
real functions, runs the lifted C over the same arguments, and compares.

```bash
py -3 -m tools.conformance
```

For a decomp that means you can check your C is *semantically* right before it is
byte-exact — a different and easier question than matching, and a good way to
find out you have misread a function early. Candidates are chosen mechanically
(frame prologue, plain `ret`, no calls, no `fs:` access, every memory operand
either frame-relative or a mapped absolute), so only functions that are provably
self-contained get run.

## What this does not do

Worth knowing before you build on it.

- **No object-file grouping.** Recovering which functions came from which `.obj`
  needs debug info, name clustering or address-order heuristics, and none of that
  is here. RTTI plus an SDK cross-reference is the best available substitute and
  it is only available on binaries that shipped with RTTI.
- **Code only.** `.data` and `.rdata` are not split per-symbol. `tools.disasm`
  writes `data.asm`, `labels.json` and `strings.json`, which is a start.
- **Function boundaries are inferred**, and in data-heavy regions they are wrong.
  The kernel import thunk table at the bottom of `.text` decodes as nonsense
  "functions". Trust `detection_method`.
- **Byte-identical, not build-identical.** This gets each function's bytes right.
  Reproducing the original *link* — section order, padding, alignment, COMDAT
  folding — is a separate problem this does not touch.

## One shared output directory

`tools/disasm/output/` is a single directory, so analysing a second binary
overwrites the first. Downstream tools check for this rather than silently using
the wrong data:

```
ERROR: disassembly is for 'default.xbe' but you asked to recompile 'xboxdash.xbe'.
       Re-run tools.disasm on this binary, or point --disasm-dir at the matching output.
```

If you work on more than one binary, give each its own with `tools.disasm -o
build/<title>/disasm` and pass `--disasm-dir` to the tools that read it.
`tools.split` writes to its own `--out` and does not have this problem.
