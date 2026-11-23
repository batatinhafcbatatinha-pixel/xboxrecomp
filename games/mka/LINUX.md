# Native Linux build

Untested. Use x86-64 Clang, CMake, Ninja, Python, SDL2, SDL2_ttf, OpenSSL,
libepoxy, DXVK native and vkd3d-shader. Package lists and dependency prefixes
are described in [the platform notes](../../docs/LINUX_MACOS.md).

With your own disc installed in `game_files/`, run `bash build-linux.sh`,
then `bash Play.sh`. `JOBS=2` is the default. No Wine or CPU emulator is used.

This profile requires private generated sources in `project/src/recomp/gen/`.
There is no reproducible disc preparation pipeline or launcher installer yet.
