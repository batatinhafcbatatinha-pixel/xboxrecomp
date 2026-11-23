# Developing

## Differences from upstream

- `games/`: game profiles (recompiler config, native game code, build and play scripts)
- `hle/d3d8/`: one shared Direct3D 8 / DirectSound / input HLE (native Direct3D 11 renderer, PC settings menu, native audio)
- launcher + one-click install/build/play (`Launcher.cmd`, [port-installer](https://github.com/batatinhafcbatatinha-pixel/port-installer) submodule in `tools/port-installer`)
- NV2A: native-GPU mode for the HLE ports (no FIFO touched, native vblank), flush-kick thread
- APU: DSP from xemu (GP/EP) and the native DirectSound mix of the ports on upstream's period pacing; XAudio2 queue
  with a rate nudge (the ports submit at their own pace)
- kernel: the ports' guest CPU lock (`RECOMP_PORT_GUEST_LOCK=1`, with `RECOMP_GUEST_SMP=1`), beside upstream's
  `RECOMP_GUEST_LOCK` (only one of the two is ever on); title KEVENTs, guest events, interrupt-object locks,
  guest-write hook around NtReadFile, ordered NV2A software-method notifications
- input: non-XInput pads through run-time SDL3; `RECOMP_KEYMAP` over upstream's keyboard map
- recompiler: `--guest-cpu-yield` (the ports' lock hands the CPU over at loop back edges), EBP bridging across calls;
  unresolved direct calls are recorded for the next build (`missing_code.txt`, `tools/xboxport`)
- `tools/xboxport`: the generic path for any disc with no profile

## Building

Windows: Visual Studio 2022 (or Build Tools) with the C++ workload and Windows SDK, LLVM/Clang 20+, CMake 3.25+, Ninja,
Python 3.11+ (the launcher puts LLVM, CMake and Ninja in `.local/tools`).

One game, without the launcher (same steps it runs; `-Source` is your disc image):

```bat
git clone --recurse-submodules https://github.com/batatinhafcbatatinha-pixel/xboxrecomp.git
cd xboxrecomp
powershell -ExecutionPolicy Bypass -File tools\port-installer\setup\setup.ps1 -GameDir games\<game> -ToolsDir .local\tools -Source "D:\ISO\your-disc.iso"
```

Or by hand inside `games/<game>/`: the `steps` of its `setup.json` (each game's `README.md` has the details), then its
`Play-*.cmd`. The toolkit itself builds as the upstream documentation describes (see [Upstream](../README.md#upstream)).
