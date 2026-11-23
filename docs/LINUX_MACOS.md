# Linux and macOS

Source-only, experimental support. No build or play test was run on either
system for these changes. Windows keeps its existing scripts and settings.

The launcher has per-system dependency lists, executable paths and bash
build/play steps. Tools use `.local/tools` as in the shared installer. Each
profile has `build-linux.sh`, `build-macos.sh`, `Play.sh` and Linux notes.
Run scripts with `bash` when a source archive has no executable mode bits.

## Profiles

Profiles use x86-64 Clang and Python, CMake 3.25+, Ninja, SDL2, SDL2_ttf,
OpenSSL and libepoxy. Install Python requirements from the profile's
`requirements.txt` where present. The launcher handles those requirements.
Linux package lists for apt, dnf, pacman and zypper are in `launcher.json`.

Linux additionally needs an installed DXVK native development prefix
(headers plus `lib/libdxvk_d3d11.so` and `lib/libdxvk_dxgi.so`) and a
vkd3d-shader development prefix. Set `DXVK_NATIVE_DIR` and `VKD3D_DIR`.
Defaults are `.local/native-deps/dxvk-native` and `.local/native-deps/vkd3d`.
The installed versioned shared libraries must also exist for CMake staging.
A working Vulkan driver is required. No Wine process is started.

macOS profiles use DXMT and vkd3d-shader. Set `DXMT_DIR` and `VKD3D_DIR`, or
`DEPS` to their parent (default `~/.local/share/native-ports-macos`). These
scripts consume existing development installations; they do not download
third-party source or binaries. Existing per-profile macOS dependency
instructions are still experimental. Their old download helpers are not
invoked automatically and are not newly hash-verified by this work.

On Apple Silicon the profile scripts use Rosetta 2 and x86-64 Python in
`/usr/local/bin/python3`. Install x86-64 SDL2, SDL2_ttf, libepoxy and OpenSSL
under `/usr/local`; the launcher's native Homebrew packages alone do not
supply those x86-64 libraries. DXMT needs the Metal toolchain described in
the existing profile notes. No arm64 HLE renderer build is claimed.

Place your own extracted disc in `game_files/`, run `bash build-linux.sh`
or `bash build-macos.sh`, then `bash Play.sh`. `JOBS` defaults to 2. Build
scripts accept `--jobs N`; profiles using `tools/build_local.py` also accept
its existing options, including `--reuse-generated`. The profile at
`games/mka/` has no reproducible disc preparation or installer metadata:
its scripts require private generated sources already present. It is not
added to the launcher's automatic installation list.

## Generic discs and runtime

The generic launcher path selects native Clang, links POSIX runtime
libraries, launches `build/out/bin/game`, uses POSIX disc paths and keeps
SDL's event loop on the process main thread. It does not fetch Windows
DLLs or use the Unicorn CPU fallback on POSIX. Missing translated code
still prevents successful play. Command-line use:

```sh
bash tools/xboxport/build-linux.sh --port /path/to/private-port --jobs 2
bash tools/xboxport/Play.sh --port /path/to/private-port
```

Use `build-macos.sh` on macOS. This generic route retains the existing
NV2A/APU/OHCI hardware models; it is not an entirely hardware-model-free
port. Removing those models requires native replacements beyond build
portability. On Intel macOS its APU/AC97 trapped-MMIO path is still absent
in `src/kernel/xbox_memory_layout.c`; generic play there is incomplete.
The POSIX FMV player and graphics features also retain existing limitations.
Windows-only DLSS and graphics API choices do not gain POSIX implementations.

CI in `.github/workflows/native-posix.yml` builds the runtime and standalone
C tests on Ubuntu and macOS 15, then runs existing C/Python tool tests.
It consumes no game files and builds no generated game code. CI has not
been run for this change. It does not validate DXMT, DXVK native, the shared
HLE renderer, profile linking, Rosetta or play. Those remain unverified.
