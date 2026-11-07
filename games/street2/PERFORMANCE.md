# Native animation math — 2026-10-30

Three of the game's own functions now run as native C: the skinning palette
blend (0x1A4BF0), the skeleton's bone matrices (0x1A7DD0) and the matrix
multiply used all over the game (0x25424B). They are the same code as Street
1's 0x1A03B0, 0x1A4EE0 and 0x161386 (the translations differ only in
addresses) and were the hottest game code in a match. Each is a port of the
translated code operation by operation, so results are bit-identical.

Switches: `RECOMP_GAME_NATIVE_SKIN=0` runs the translated functions;
`RECOMP_GAME_NATIVE_CHECK=1` runs both and compares every result (`MISMATCH`
in the log); `RECOMP_HLE_AB=2` alternates them every 600 frames.

Check mode over a match: 18 million matrix multiplies, 139 thousand skeletons
and 2.5 million skinning blends, 0 mismatches. Interleaved A/B in one match
(220 s, texture watch off to isolate this change), 12 consecutive window
pairs with similar draw counts and no load spikes: 6.68 → 5.62 ms per frame
(+18.8% fps); skinning blend 3.49 → 1.12 µs per call.

# Texture change detection — 2026-10-30

Textures in the contiguous window are now watched instead of re-hashed: after a
full hash their pages are write-protected in both views of that memory
(0x80000000 and the tiled alias at 0xF0000000). The first write faults, the
handler reopens the page and stamps it, and a texture is reused only while
none of its pages (or its palette's) were written after its own stamp. No
write can be missed, unlike the sampled probe that checked static textures
before. Kernel-mode writes (ReadFile into guest memory) do not fault, so the
xboxrecomp runtime calls a hook around them (fork commit aa0fa6d). Textures
that changed lately, or whose pages the game rewrites every frame with the same
bytes, stay on the previous hashing scheme.

Switches: `RECOMP_HLE_NO_TEXWATCH=1` restores the previous scheme;
`RECOMP_HLE_TEXWATCH_CHECK=1` also hashes every texture the watch calls
unchanged and logs any difference as `MISS`; `RECOMP_HLE_AB=2` alternates only
this change every 600 frames; `RECOMP_HLE_TEXWATCH_LOG=<n>` logs who writes
watched pages. Linux builds keep the hashing scheme.

Same machine and settings as below (RTX 3050 Laptop, 1080p, VSync and limit
off, monitor 1), interleaved 600-frame windows in one session, gameplay windows
only (≥250 draws/frame for Street 1, ≥200 for Street 2):

| Run | Windows off / on | Draw time per frame | Throughput |
|---|---:|---:|---:|
| Street 2 (Brazil × Denmark match) | 21 / 21 | 1.224 → 1.050 ms (−14%) | 163.2 → 172.9 fps |

Check-mode runs in a match: 0 misses for both games (Street 1 ~110k frames,
Street 2 ~19k frames). Screens captured with and without the watch show the
same rendering (menus, matches, HUD, replay).

# Renderer update — 2026-10-22

The native renderer avoids rebinding unchanged D3D11 state and uploading
identical constant buffers. Changed textures with matching dimensions, mip
count and format receive new texels in place. Uncompressed conversion selects
the format outside the pixel loop; YUV video converts pairs with shared chroma
arithmetic. Rendering resolution, pixel calculations and timing defaults are
preserved.

## Validation

Windows / RTX 3050 Laptop GPU / D3D11, 1920×1080 internal resolution, VSync and
frame limit off, monitor index 1 (75 Hz). Two 160-second runs alternated caches
and texture reuse every 600 frames within the same match. Intro/loading were
excluded. Selected gameplay windows had at least 250 draws/frame and less
than 0.1 ms average build work; consecutive on/off pairs differed by at most
15% in their draw counts.

| Measurement | First run, off → on | Repeat, off → on |
|---|---:|---:|
| Selected pairs | 10 | 13 |
| Mean frame time | 6.489 → 6.167 ms | 6.558 → 6.103 ms |
| Throughput | 154.11 → 162.15 fps | 152.49 → 163.85 fps |
| Draw cost per call | 4.335 → 3.773 µs | 4.297 → 3.755 µs |
| Draw cost reduction | 12.96% | 12.61% |
| Worst frame | 23.1 → 40.0 ms | 18.0 → 19.0 ms |
| Frames over 20 ms | 1 → 6 | 0 → 0 |

The first run's six optimized-mode pauses clustered in one window. Its worst
frame spent 1.75 ms in measured draw work, 0.17 ms presenting and zero time
building resources; the remaining delay is outside those measured renderer
phases. The 40-ms pause did not recur in the repeat. Its cause has not been
established, and these results do not prove that all frame-time spikes are
fixed. Average throughput improved about 5–7% in the measured gameplay windows.

Both production converters matched the pre-update native converter exactly
in 798 synthetic cases each, including odd YUV widths, missing palettes,
padded rows and output guards. A separate capped run's 19 captures covered
menus, gameplay, goals, replay and Gamebreaker. Both updated Windows builds
and runtime comparisons completed without unresolved native calls. Six kit
tool tests and the publication audit passed. A fresh private generation from
the public kit was not repeated; the four changed kit source files match the
tested development files. These observations cover one machine and one match
configuration. Uncapped mode accelerates the simulation; normal-play defaults
remain unchanged.

## Diagnostics

- `RECOMP_HLE_PROFILE=1`: timings and CPU use every 600 frames.
- `RECOMP_HLE_AB=1` with profiling: alternate optimization modes each window.
- `RECOMP_HLE_NO_STATE_CACHE=1`: rebind state every draw.
- `RECOMP_HLE_NO_CB_SKIP=1`: upload constants even when unchanged.
- `RECOMP_HLE_NO_TEX_REUSE=1`: recreate changed textures.
- `RECOMP_HLE_MONITOR=1`: select the second monitor for tests.
- `RECOMP_HLE_FPS=0` and `RECOMP_HLE_VSYNC=0`: uncapped throughput testing.

Per-draw clock queries run only while profiling. No new effect toggles were
added. Glow, depth-of-field and motion-blur properties were found in script
objects; their values are copied or animated into rendering objects. An
independently verified control is still needed before exposing these in F1.
