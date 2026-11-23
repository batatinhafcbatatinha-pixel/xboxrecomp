# Native macOS build

Untested. This profile requires x86-64 Python and libraries, DXMT and
vkd3d-shader. Apple Silicon requires Rosetta 2. See
[the platform notes](../../docs/LINUX_MACOS.md).

Run `bash build-macos.sh`, then `bash Play.sh`. This profile needs private
generated sources in `project/src/recomp/gen/`; no disc preparation pipeline
is supplied.
