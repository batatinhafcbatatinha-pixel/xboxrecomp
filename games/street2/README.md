# FIFA Street 2 — Native PC Port | No Emulated Xbox Hardware

> [!NOTE]
> This port now lives in the [xboxrecomp](../../) repository as `games/street2`. The runtime is the
> repository root (`../..`) and the Direct3D 8 / DirectSound / input HLE is shared with the other games in
> [`hle/d3d8`](../../hle/d3d8/). Clone xboxrecomp (with `git submodule update --init` for the game-file
> installer) and run the scripts from this folder.

> [!WARNING]
> This repository does **not** include the game, an ISO/XBE, EA assets, generated game code or a precompiled executable.
> You need your own original-Xbox USA disc. See [the full notice](#notice) at the end.

An experimental port of the original-Xbox FIFA Street 2 built through static recompilation, with native PC graphics, audio and input. It is the sibling of the [FIFA Street (2005) port](https://github.com/batatinhafcbatatinha-pixel/fifa-street-pc) and reuses its runtime and native layers with this game's own addresses. **This public repository is a source and local-build kit, not a ready-to-play game download.** The game's x86 code is translated locally from your own verified disc; no game executable or generated game code is distributed here.

## How to play

> [!IMPORTANT]
> **There is no ready-to-play download.** This page has only tools: they turn *your own* game disc into a PC game,
> on your computer. The first time takes a while (20-60 minutes on a recent PC), mostly waiting.
> After that, you just double-click to play.

**You need**

- **Windows 10 or 11** (64-bit) with about **15 GB** free. (Linux and Mac: see below.)
- Your own **FIFA Street 2** (original Xbox, USA), as a disc image (`.iso`) made from your disc, or its extracted
  files. Other versions are not accepted; the setup checks this for you.

**Steps**

**Easiest: `Setup.exe`.** Open [Releases](https://github.com/batatinhafcbatatinha-pixel/fifa-street-2-pc/releases), download **`Setup.exe`** from the newest one and run it.
It downloads this kit (source code only), shows what your PC already has and what is missing on its
*Dependencies* tab, installs what is missing, asks for your `.iso` and builds the game. Afterwards the same
program (and its desktop shortcut) is your launcher: **Play**, and **PC settings** (the same options as F1 in
the game, before it starts). Windows may say it protected your PC because the program is not signed: click
*More info* > *Run anyway*. The program is built by GitHub from [tools/launcher](tools/launcher); nothing from the game is in it.

**Or step by step:**

1. **Download**, one of these two ways:
   <details open><summary><b>Easiest: the ZIP</b></summary>

   Open [Releases](https://github.com/batatinhafcbatatinha-pixel/fifa-street-2-pc/releases/latest), download **`fifa-street-2-pc-source-kit.zip`** and extract it to a short folder, such as
   `C:\Games\fifa-street-2-pc`.
   </details>
   <details><summary><b>With Git: always the newest version</b></summary>

   ```powershell
   git clone --recursive https://github.com/batatinhafcbatatinha-pixel/fifa-street-2-pc.git C:\Games\fifa-street-2-pc
   ```
   To update later: `git pull --recurse-submodules`, then run `Setup.cmd` again.
   </details>

2. **Double-click `Setup.cmd`.** It asks before installing anything, then:
   - installs Python and Microsoft's C++ Build Tools if you don't have them (free; Windows asks for permission once);
   - downloads the other build tools (about 1 GB) **into this folder only**;
   - opens a window where you **pick your `.iso`**: it checks the version and copies the game files;
   - builds your PC version, and offers a desktop shortcut.

   If anything stops, run `Setup.cmd` again: finished steps are skipped.

3. **Play:** double-click **`Play-FIFA-Street-2.cmd`** (or the desktop shortcut).

**In the game:** **F1** opens the PC settings. Controllers and the keyboard work: see [Controls](#controls).

**Problems?** Open an [issue](https://github.com/batatinhafcbatatinha-pixel/fifa-street-2-pc/issues) with `setup-log.txt` and a screenshot. Never attach game files.

`Setup.cmd` is new: so far it has been tried from a fresh download only with the FIFA Street 3 port. If it stops for you, please open an issue with `setup-log.txt`.

<details><summary><b>Linux and Mac</b></summary>

- **Linux:** works natively (no Wine), but the setup is by hand for now: see [Linux](#linux).
- **Mac:** not playable yet. The Mac build files are an experiment that has never been compiled or run on a Mac. Details in [MACOS.md](MACOS.md); if you have a Mac and want to help test, open an issue.

</details>

<details><summary><b>Questions</b></summary>

- **Is this legal?** This page contains no game, only tools. You use the game you already own.
- **Can I share my built game?** No: it contains the game's code. Keep it on your PC.
- **Will it run on my PC?** Any graphics card with DirectX 11 should work. Measured: matches run at 60 fps (the game's own rate) up to 4K on a Ryzen 5 3600 + RTX 3050 Laptop.
- **How do I remove it?** Delete the folder. Python and the Build Tools can be removed in Windows Settings > Apps.

</details>

## Status

- Tested on Windows 11 x64: boot, menus, full matches at 60 fps, goal replays, game over, Rule The Street player creator, and saves (written and loaded back).
- Graphics: Direct3D 8 reimplemented in C on Direct3D 11 (or Vulkan through DXVK). Resolutions up to 4K and ultrawide were tested at 60 fps. Aspect options: the game's own 16:9 widescreen (with bars on wider screens) or **21:9 hor+**, which extends the scenery to the screen edges.
- Audio: DirectSound reimplemented in C with its own mixer, output through XAudio2.
- Input: XInput reimplemented on host pads and the keyboard.
- No NV2A GPU, APU/DSP, AC97 or USB model is started. The Xbox kernel and memory layout remain as compatibility layers.
- Tested on Windows x64; a Linux build path exists. Experimental [macOS](MACOS.md) build files are included; they have not been compiled or run yet.

## Controls

### Controller

Xbox pads use XInput. On Windows, `SDL3.dll` next to the executable adds every other pad SDL knows: DualShock 3/4, DualSense, Switch pads, USB adapters for PS2 and original Xbox pads. `tools/build_local.py` downloads it with `gamecontrollerdb.txt` (both zlib license); `tools/fetch_pad_support.py` does the same for a manual build. Without those files the port behaves as before. No PlayStation pad or adapter has been tried on real hardware yet. **Controller vibration** in the F1 settings turns the pad's vibration off.

### Keyboard and mouse

No controller needed: the keyboard plays as an Xbox controller, with the keys of the default **WASD** layout of EA SPORTS FC 27 ([EA's layout chart](https://help.ea.com/en/articles/ea-sports-fc/mouse-keyboard/)) on the buttons FIFA used on the original Xbox. Only the basic actions are mapped for now.

| Key | In a match | In menus | Controller button |
| --- | --- | --- | --- |
| **W A S D** | Move the player | Navigate | Left stick |
| **Arrows** | | Navigate | D-pad |
| **L** | Pass | Confirm | A |
| **;** (**Ç** on ABNT2 keyboards) | Shoot | Back | B |
| **K** | Lob pass / cross | | X |
| **O** | Through pass | | Y |
| **P** | Sprint | | Right trigger |
| **Enter** | | Confirm, passes *PRESS START* | A + Start |
| **Esc** | Pause | | Start |
| **Backspace** | | Back | B |
| **Tab** | | | Back |

The keyboard drives player 1 and is merged with a controller plugged in there, so both work at the same time. Tricks (right stick), the white/black buttons and the left trigger are not mapped on the keyboard yet. The launcher turns the keyboard on (`RECOMP_KEYBOARD=1`); set `RECOMP_KEYBOARD=0` to turn it off.

### PC settings

Xbox controllers work as on the console. **F1** or **Back + Start** opens the PC settings (display mode, resolution, aspect, VSync, frame limit, graphics API, FXAA, MSAA (off, 2x/4x/8x or the 2x the Xbox game asks for), anisotropic filtering, language, volume). **Alt + Enter** toggles fullscreen.

The PC settings screen is drawn in the game's own style (graffiti title, paper poster, black selection stroke, tan value bands with white arrows) with the game's fonts, FifaStreet_Title Leigh and EAS SansCondensed. They are read from your disc (`data/aptanims/aptfnts.big`) when the game starts and are not part of this repository. On the main menu and the Options screens a **BACK · PC Settings** hint shows that the controller's Back button opens it there. The menu is in English by default; *Menu language* switches it to Portuguese.

## Your game files

Only the **original Xbox USA** revision is accepted. Extract your own disc image into a folder named `game_files` and validate it:

```sh
python tools/extract_iso.py "FIFA Street 2 (USA).iso" --output game_files
python tools/validate_game.py game_files
```

```text
default.xbe  5fabb5b763be0cc8823e814ee343ac5be6950305330f46c5170e08eaadea910b
```

Do not upload the ISO, extracted files, generated code or the built executable.

## Building by hand

`Setup.cmd` ([How to play](#how-to-play)) does all of this on Windows. The steps below are for doing it yourself, or on Linux.

### Windows

Requirements: Python 3.10+ (with `pip install -r requirements.txt`), Visual Studio 2022 Build Tools (C++ workload, Windows SDK), LLVM/Clang (`clang-cl`), CMake and Ninja.

The xboxrecomp toolkit is the `xboxrecomp/` submodule ([batatinhafcbatatinha-pixel/xboxrecomp](https://github.com/batatinhafcbatatinha-pixel/xboxrecomp), branch `ports`): clone with `git clone --recursive`, or run `git submodule update --init` in an existing clone. The source-kit ZIP from Releases includes it; GitHub's **Code → Download ZIP** does not.

```powershell
python -m pip install -r requirements.txt
python tools/build_local.py --game-files game_files --llvm-dir "C:\Program Files\LLVM"
python tools/play.py --game-files game_files
```

`build_local.py`:

1. validates your files;
2. takes the xboxrecomp runtime and translator from the `xboxrecomp/` submodule;
3. disassembles your `default.xbe` with this game's function seeds and translates it into private C under `.local/`;
4. applies the post-passes (interrupt acknowledgement, frame-pointer boundaries, serialized heap code, native API wrappers) and compiles the port.

The first build takes a while. `--reuse-generated` skips the translation on later builds.

### Linux

The same kit builds a native Linux x86-64 executable (no Wine): the window, input and audio are SDL2, Direct3D 11
is DXVK native (a Linux library implementing D3D11 on Vulkan) and the HLSL shaders compile through vkd3d-shader.
Install Python 3.10+, Clang, CMake, Ninja, SDL2, SDL2_ttf, OpenSSL, libepoxy and Vulkan drivers and headers
(Debian/Ubuntu: `clang cmake ninja-build pkg-config libsdl2-dev libsdl2-ttf-dev libssl-dev libepoxy-dev
libvulkan-dev mesa-vulkan-drivers`), plus [DXVK native 3.1.1](https://github.com/doitsujin/dxvk/releases) and
vkd3d-shader 1.19 or newer built with HLSL support.

```sh
python3 -m pip install -r requirements.txt
python3 tools/build_local.py --game-files game_files \
  --dxvk-native-dir /path/to/dxvk-native/usr --vkd3d-dir /path/to/vkd3d-install
python3 tools/play.py --game-files game_files
```

The DXVK prefix must contain `include/dxvk` and `lib/libdxvk_d3d11.so`, `lib/libdxvk_dxgi.so`; the vkd3d prefix
the headers and `lib/libvkd3d-shader.so.1`. Both travel next to the executable in `lib/`. Status: translation and
build verified on Ubuntu 24.04 (Clang 18); gameplay on Linux not yet checked.

### macOS

Experimental and not compiled or run yet: see [MACOS.md](MACOS.md).

## Why a native port instead of an emulator

An emulator such as [xemu](https://github.com/xemu-project/xemu) recreates the original Xbox: it translates the console's CPU code at run time and models the NV2A GPU, the audio DSP and the USB controllers register by register. That is the right approach for the whole library. This port takes a different route for one game:

- **Ahead-of-time code.** The game's x86 code is translated into C and compiled by a modern optimizing compiler.
- **No emulated hardware in the hot path.** Direct3D 8, DirectSound and XInput calls go straight to C implementations on Direct3D 11, XAudio2 and host pads, instead of being lowered to push-buffer commands, DSP microcode and USB packets that are emulated again.
- **Resolution and widescreen beyond the console**, rendered by the PC GPU at any internal resolution.
- **Readable and fixable.** Each hook and native layer is plain C in `street2/project/src`.

The trade-off: it covers only this game and revision, and it is experimental. An emulator is the more general and better-tested option.

## What is in this repository

- `street2/` — this game's part: function seeds, native hooks and HLE tables, post-passes, and the CMake project in `street2/project/`.
- `xboxrecomp/` — the modified [xboxrecomp](https://github.com/batatinhafcbatatinha-pixel/xboxrecomp) toolkit (submodule, branch `ports`): x86 translator, Xbox kernel layer and the native Direct3D 8, DirectSound and XInput implementations.
- `tools/` — `build_local.py` (translates your executable and compiles), `play.py`, `fetch_pad_support.py`, `setup.ps1` / `setup_tools.py` (behind `Setup.cmd`), `audit_publication.py` (the check that keeps game data out of this repository) and the shared game-file installer in `tools/port-installer`.
- `installer.json` and `setup.json` (what `Install-Files.cmd` and `Setup.cmd` accept and do), `requirements.txt` (Capstone for the disassembler), `licenses/` and [NOTICE.md](NOTICE.md).
- `kit.json` — the pinned downloads.

## Credits and licenses

- Port integration and native backend work: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp): **Ned Heller / sp00nz** and contributors (MIT). The modified copy used here is the fork [batatinhafcbatatinha-pixel/xboxrecomp](https://github.com/batatinhafcbatatinha-pixel/xboxrecomp) (branch `ports`), with all its notices and licenses, included as the `xboxrecomp/` submodule; see `street2/project/LICENSE.xboxrecomp`. `tools/translate-ebp-boundaries.py` comes from [fifa-street-pc](https://github.com/batatinhafcbatatinha-pixel/fifa-street-pc).
- [xemu](https://github.com/xemu-project/xemu) contributors for LGPL/GPL runtime components credited in that snapshot's notices (present in the runtime, not started by this port's profile).
- [DXVK](https://github.com/doitsujin/dxvk) (zlib), [vkd3d-shader](https://gitlab.winehq.org/wine/vkd3d) (LGPL) and DejaVu fonts: optional external dependencies; licenses in `licenses/`.
- [Capstone](https://www.capstone-engine.org/): disassembler used by the translator (installed through `requirements.txt`).
- FIFA Street 2, its game code, trademarks and assets belong to Electronic Arts and the respective rights holders. This project is not affiliated with or endorsed by EA or Microsoft.

See [NOTICE.md](NOTICE.md) for the publication boundary and [LICENSE](LICENSE) for the license of the port's own code.

---

<a id="notice"></a>

**THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XBE, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED ORIGINAL-XBOX USA GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS.**
