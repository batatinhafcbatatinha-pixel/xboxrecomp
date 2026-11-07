# Mortal Kombat: Armageddon (Xbox, USA) — native PC port

Title MK7, XDK 5849, D3D8 non-LTCG (same library as FIFA Street 1/2).
Base: Street 2 project (D3D8 HLE + native D3D11 renderer + DirectSound/XInput HLE).

## Layout
- `default.xbe`, `game_files -> D:/CodexBuilds/mka/game_files` (extracted disc, 981 files)
- `hle/` — HLE tables remapped from Street 2 (`addrmap.json`, `remap_overrides.json`, reports)
- `tools/` — remap/addrmap helpers, ebp-boundary pass
- `project/` — CMake project; `src/recomp/gen` = generated C (not published)
- Recompiler work: `D:/CodexBuilds/mka/recomp-work` (disasm/ident/abi/gen, 29598 funcs, 119 units)
- Build: `build-local.ps1` → `D:/CodexBuilds/mka/build-release` (buildsup, status file `build-release.status`)

## Memory layout
Image ends 0x0095A000: kernel data 0x00960000, stack 0x00970000 (6 MB), heap from 0x00F70000.

## Regenerate pipeline (after recomp)
1. copy `recomp-work/gen` → `project/src/recomp/gen`
2. `hle/translate-nv-interrupt-writes.py` (6 writes)
3. `tools/translate-ebp-boundaries.py --apply --directory project/src/recomp/gen`
4. `hle/apply-hle-wrappers.py` (143 D3D funcs; 2 dead: InsertFence, CreateIndexBuffer2 → `src/hle_dead_gen.c`)

## Log
- 2026-11-03: extraction, recomp, remap (412 literals), CMake. Fixed post-pass globs
  (`recomp_00*` missed units 0100+), HLE pass made single-regex (10 min → 6 s). First build.
- 2026-11-03 later: fixed missing DS entry 004AA6DF (IDirectSoundBuffer_SetMixBinVolumes) in dsound_functions.json;
  dead entries get `_gen` declarations appended to recomp_funcs.h. Second build launched (buildsup, status
  `D:/CodexBuilds/mka/build-release.status`, log `.status.log`). `run-test.ps1` added (exe/runs on D:).
  NEXT: check build result → fix link errors → first boot run (`run-test.ps1 -Seconds 30`), expect ICALL failures
  (Street 2 fix: seed_functions.json / add_missing_functions.py).
- 2026-11-03 18:10: 2nd build compiled everything; link failed only on _mm_set_ss/_mm_cvt_ss2si (recomp_0065.c,
  sub_00313F10, no intrinsics header) → CMake force-includes xmmintrin.h in gen units. 3rd build launched (full gen recompile).
- 18:40: force-include did not help — root cause: C:/Program Files/LLVM lacks lib/clang (resource headers), so
  clang-cl took MSVC's xmmintrin.h. Build switched to D:/llvm22 clang-cl (build-local.ps1), force-include removed,
  build dir recreated, 4th build launched.
- 2026-11-04: 4th build OK. First boot (run 20261104-024241): XBE/kernel/heap fine, D3D11 device created
  (640x480), then `[ICALL] Failed 0x002C8960` and `0x001C4FEE` → crash in sub_001C4DD0.
  - 0x002C8960: function hidden behind sub_002C85F0's jump table → `tools/add_missing_functions.py`
    (MISSING list; entry hand-added to `D:/CodexBuilds/mka/recomp-work/disasm-extra/functions.json`) → gen/recomp_extra.c.
  - 0x001C4FEE: the scheduler's return address. MK runs its logic as Midway "processes"
    (coroutines that swap guest esp and `jmp [0x5B8B40]`). Vtables 0x55DB5C (simple), 0x55DB84 (stack copy,
    0x200), 0x55DBAC/0x55DBD4 (own 0x4000 stack): +0xC kill, +0x10 run, +0x14 sleep, +0x18/+0x1C main stack,
    +0x20 jump. Replaced natively with host fibers: `project/src/game_process.c`, table `hle/process_functions.json`
    (read by apply-hle-wrappers.py). Build 5 launched.
- 2026-11-04 night (Claude): build 5 (process fibers) boots past the old crash: audio (XAudio2) up, permanent.mbx
  opened, then ICALL failures at 0x004A6B40 (PSFD_I start), 0x004A77C0 (PSFD_P start; the false entry 0x4A7800 hid
  it) and 0x0031BCF0 (decoder with jump tables up to 0x31F32B; bogus 0x318800/0x319800 blocks hid it) -> crash
  (run 20261104-225159). Entries hand-added to disasm-extra/functions.json (backup `.bak-20261104b`) + MISSING list.
  Build 6 OK; run 20261104-232737: those three resolve, next ICALL 0x00318260 (sibling decoder, table 0x31B858)
  -> followed by wild pointers (0x0050F98C/0x0050F4DC in .rdata, 0x44800) and the crash; likely a cascade.
  0x00318260 added (end 0x31B858); build 7 queued by `D:/CodexBuilds/mka/b7.sh` (log `build7.out`).
  NEXT: `run-test.ps1 -Seconds 45 -CaptureFrames` and check whether the wild pointers disappear.
- 2026-11-05 00:01 (Claude): build 7 OK, run 20261105-000111: 0x00318260 resolves; next ICALL 0x004A6510 (PSFD00,
  starts exactly where sub_004A62C0 ends, reached by computed pointer), then wild values (0x005CDF10/0x005CE9A0 in
  .data, 0xFFFFFF10) -> crash. Coverage gap scan of the PSFD sections also found 0x004A6050 (after the jump table at
  0x4A6024, same prologue). Both added; build 8 via `b7.sh` (log `D:/CodexBuilds/mka/build8.out`).
  NEXT: run-test again. The PSFD00/_I/_B/_P sections look like a codec (I/B/P frame decoders).
- 2026-11-05 ~01:30 (Claude):
  - build 8 (0x4A6510/0x4A6050): no ICALL failures; boots to the LOGON.SFD intro movie and runs 45 s without a crash,
    then freezes in D3DDevice_BlockUntilVerticalBlank (0x40ADC0, xbsdb symbols in hle/xbsdb_symbols.txt).
  - Cause chain: the kernel bridge hands the guest CPU to other threads during a wait only when the published IRQL
    byte (fake KPCR fs:[0x24] = VA 0x4024, native 0x14024) is < 2. That byte held garbage (195/116/187/45...). A cdb
    hardware breakpoint (`ba w1 0x14024`) caught the writer: sub_0032C240 (movie PCM interleave copy) writing through a
    NULL-based pointer from IDirectSoundBuffer_Lock. With RECOMP_DS_NATIVE the buffer objects are native handles, and
    the library Lock read garbage from them.
  - Fixes: native IDirectSoundBuffer_Lock (0x4AA837) / Unlock (0x4A8254) in hle_dsound_native.c, added to
    hle/dsound_functions.json + hle_dsound_table.h (note: Lock is 0x4AA837, Unlock is 0x4A8254). hle_api_vblank also
    sets the device's m_VerticalBlankEvent (g_pDevice 0x427AE8 + 0x1DBC). BlockUntilVerticalBlank goes through
    hle/sync_functions.json -> hle_sync.c sub_0040ADC0, which repairs the IRQL byte if it is > 31 and reports it
    ("[HLE-SYNC] KPCR IRQL byte held ...": none since the Lock fix).
  - build 12 (run 20261105-012548): 115 swaps, past LOGON.SFD, opens art\attract.ssf, then ICALL 0x002C95D0 -> crash in
    sub_0005A7D0. 0x002C95D0 added (ends 0x2C9756); build 13 via b7.sh (log build13.out).
  - Tools: `D:/CodexBuilds/mka` cdb session template = tools/cdb-watch.ps1 + tools/watch-irql.cdb (`ba w1` with a value condition).
- 2026-11-05 12:36 (Claude): build 13 run (20261105-123628): no crash in 60 s; past attract.ssf, LOGON.SFD and
  OPENINGN.SFD play (23 fps swaps, movie decode), but every frame is black: `0 draws/s`. MK draws through
  D3DDevice_DrawVerticesUP (0x4100D0, 8 sites) and DrawIndexedVerticesUP (0x410240, 1 site), which were not in the HLE
  table. Added both (hle_functions.json, hle_d3d8_table.h, hle_d3d8.c skip in primary mode, hle_draw.c `draw_up`:
  one inline stream from the caller's buffer, attribute offsets/formats from the current shader declaration
  +24/+28). Build 14b (full, the table header changed) via D:/CodexBuilds/chain-mka-ctw.sh. NEXT: run-test -CaptureFrames.
- 2026-11-06 (Claude/Codex): fixed-function vertex shader (FVF; static D3D object 0x42A1D0, XYZRHW + 4 texcoords for the
  movie quad) handled by `ffp_vs` in hle_draw.c (Codex) — 0 skipped draws. Run 20261106 with -CaptureFrames shows the
  legal screen and the intro movie (OPENINGN.SFD). Next: skip movie / reach the main menu with input (run-test -LiveInputFile).
- 2026-11-06 noite (Claude): "tela preta depois do vídeo" não existe: OPENINGN.SFD (308 MB, MPEG 640x480 29,97 fps) dura
  ~4,5 min e depois vem TITLEN.SFD (logo + PRESS START, correto). Vídeo roda a 22-24 fps (CPU: decodificador numa thread
  do jogo disputa a CPU única do RECOMP_GUEST_LOCK; testar RECOMP_GUEST_SMP=1).
  - Pular vídeo: o player (sub_0025D380(id, cb) -> sub_0028AB00, laço `while(!sub_0028A990(m))`) chama a cada quadro o
    callback em 0x5A8D64 (sub_002EDB80, estado 2/3); se der 1 vai ao estado 4 (fade) e termina. O callback da intro é
    sub_000280E0: lê o pad (sub_0026A9C0) e sub_000282A0 -> sub_000A10F0(ação 0xB = START 0x800 / 6 = A 0x40), que só
    olha portas com o bit 1 de 0x831C0C+porta*0x20 ("conectado").
  - Causa: a init (sub_0026A740) abre os pads que XGetDevices já mostra sem ligar esse bit; só a inserção vista por
    XGetDeviceChanges no leitor por quadro liga. O XInput nativo mostrava o pad na hora -> porta 0 aberta mas
    "desconectada" (flags 8): nenhum botão chegava (nem no menu). Correção: hle_input.c simula a enumeração USB
    (nada conectado nos primeiros RECOMP_INPUT_ENUM_MS = 500 ms depois de XInitDevices).
  - game_process.c: sub_0019C980(code) = longjmp(0x5B8848, code); o wrapper lia o buffer em esp+4 (o código) ->
    "[PROC] longjmp buffer unknown" e crash no sub_001C4DD0 ao sair do título. Corrigido.
  - Funções faltantes após o título (run 20261106-231001): 0x00097E00 (corpo de processo) e 0x00397F16 (cabeça de laço
    de sub_00397EB0, reentrada por sub_00397F7E) -> disasm-extra/functions.json + MISSING.
- 2026-11-07 madrugada (Claude, teste ao vivo com Windows MCP + RECOMP_PAD_LIVE): **chega à luta**. Intro pulável,
  título, perfis, menu principal, Kombat > Arcade, seleção de lutadores, torre e luta (Scorpion x Fujin / x Scorpion)
  com arena completa, HUD, combos, a 60 fps, 0 draws descartados.
  - Crash do A no menu: IDirectSoundBuffer_SetVolume não era nativo (a biblioteca lia o handle nativo ->
    CDirectSoundVoice_SetVolume / CMcpxVoiceClient). Nativos novos em hle_dsound_native.c: SetVolume, SetHeadroom
    (volume da voz = mixbin + volume - (headroom - 600), padrão 600), Stop; SetNotificationPositions, SetPosition e
    SetI3DL2Source aceitos sem efeito (log uma vez). Ainda na biblioteca: família IDirectSoundStream (CreateStream,
    SetMixBins/SetMixBinVolumes/SetHeadroom/FlushEx, chamadas por vtable) e DirectSoundUse*HRTF/OverrideSpeakerConfig.
  - Arena preta: o jogo cria declarações FFP próprias (CreateVertexShader(decl, NULL)); hle_draw.c agora as registra
    (ffp_decl_*) e ffp_vs as aceita (FLOAT4 na posição = XYZRHW, senão XYZ, matrizes do dispositivo; normal ignorada).
  - Funções faltantes adicionadas: 0x2B6900 (torre do Arcade). add_missing_functions.py agora reaproveita os corpos
    já em recomp_extra.c (ADD_MISSING_FULL=1 refaz tudo) e remove stubs [UNRESOLVED] das funções que ganharam corpo.
  - run-test.ps1 -Show (janela visível). Teclado pelo MCP é instantâneo demais (perde toques); usar -LiveInputFile.
  - Abertos: [UNRESOLVED] 0x0039F59B na luta; fundo branco na tela da torre; vídeo 22-24 fps (testar
    RECOMP_GUEST_SMP=1); iluminação FFP (normal ignorada); áudio não ouvido; título da janela ainda "FIFA Street".
