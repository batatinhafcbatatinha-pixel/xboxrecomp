Source kit only: no game, no XBE, no generated code and no executable. Bring your own original-Xbox USA disc.

**v0.2.3**

- **One-click `Setup.cmd`:** asks before installing anything, installs Python and Microsoft's C++ Build Tools through winget if missing, downloads pinned, SHA-256-checked LLVM/Clang, CMake and Ninja, and Capstone in a private Python environment, into `.local/tools` only, fetches missing parts, opens the game-file installer and builds the port; `Play-<game>.cmd` starts it. Run it again after a failure: finished steps are skipped.
- **README:** a *How to play* guide for people new to this, and the same section layout as the other ports.

`Setup.cmd` has not been tried from a fresh download with this game yet (only with FIFA Street 3). Please report problems with `setup-log.txt`.

**v0.2.2**

- **Every pad through SDL3:** PlayStation and generic controllers work in the game and in the F1 settings, not only Xbox pads.
- **Controller vibration** on/off in the F1 settings.
- Texture changes found exactly through page write-watch instead of hashing every frame.
- Native skinning blend, bone matrices and matrix multiply.
- The shared game-data installer (`Install-Files.cmd`) extracts and verifies your disc.
- The installer launcher is now `Install-Files.cmd` (English name); experimental macOS build files (not compiled or run yet).
- Native Linux build path for the kit (not built for this release).

PlayStation pads and vibration have not been tried on real hardware yet. Nobody has played this exact build through.

**Earlier**

- **Keyboard play:** the keyboard plays as an Xbox controller with EA SPORTS FC 27's default WASD layout (WASD move, L pass, ; shoot, K lob/cross, O through ball, P sprint, Esc pause; Enter passes PRESS START).
- Native Direct3D 8 on Direct3D 11/Vulkan, native DirectSound and XInput; no NV2A, APU/DSP, AC97 or USB model started.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify the supported revision by SHA-256.
- `tools/build_local.py`: downloads the pinned xboxrecomp runtime, translates your XBE locally and builds the port.
- Tested on Windows 11 x64: menus, full matches at 60 fps, replays, player creator and saves.
