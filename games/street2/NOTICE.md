# Redistribution and provenance

This is a source and tooling distribution. It contains no original game assets, no game executable (XBE), no generated translation units, no disassembly and no compiled game executable. The HLE tables and seed files list functional addresses and calling conventions that the translator and the native layers need; they describe where functions are, not what they contain.

The independently written native integration (Direct3D 8 on Direct3D 11/Vulkan, DirectSound, XInput, PC settings) and build tooling are offered under the root MIT license. Most native files started as copies of the FIFA Street (2005) port with this game's addresses; existing attribution and upstream licenses take precedence for imported files. This license grants no rights to Electronic Arts game content.

The xboxrecomp runtime and translator are the `xboxrecomp/` submodule: the fork [batatinhafcbatatinha-pixel/xboxrecomp](https://github.com/batatinhafcbatatinha-pixel/xboxrecomp), branch `ports`, a modified version of sp00nznet/xboxrecomp published with its MIT license and the LGPL/GPL notices of its xemu-derived components. Those emulation components exist in the runtime for diagnostics; this port's launch profile does not start them.

DXVK, vkd3d-shader, DejaVu fonts, Capstone and Microsoft tools are external dependencies; license texts are in `licenses/`, binaries are not included.

The publication audit (`tools/audit_publication.py`, run on every push) uses an allowlist and rejects executables, images, audio, archives, game resources, generated code and oversized files. It is an engineering safeguard, not a legal guarantee for every jurisdiction or use. Do not submit game content or private build artifacts in pull requests or issues.
