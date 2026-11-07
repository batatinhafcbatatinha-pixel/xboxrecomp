# Game profiles

Each folder holds one original-Xbox game's port: recompiler configuration, the game's own native code
(hooks, entry point, game-specific replacements) and the scripts that generate and build it from **your own disc**.
No game content (disc image, executable, data or generated code) is stored here.

The runtime is this repository (`../..`). The Direct3D 8 / DirectSound / input HLE that the ports share lives in
[`hle/d3d8`](../hle/d3d8/): each game supplies only its `hle_game.h` (the guest addresses and recompiled-function
names of its copy of the Xbox libraries) and its library tables (`hle_d3d8_table.h`, `hle_dsound_table.h`).

| Game | Disc | State |
| --- | --- | --- |
| [FIFA Street](street1/) | Xbox, USA and Europe | playable |
| [FIFA Street 2](street2/) | Xbox, USA | playable |
| [Black](black/) | Xbox, USA | playable |
| [Mortal Kombat: Armageddon](mka/) | Xbox, USA | early work: reaches fights |

To play one, use the launcher: see [How to play](../README.md#how-to-play-a-game). By hand: each folder keeps its own
instructions (`README.md`, `setup.json`). Game files are extracted with the shared installer in
[`tools/port-installer`](../tools/) (`git submodule update --init`).

The games, their code, trademarks and assets belong to their publishers and rights holders. These ports are not
affiliated with or endorsed by them or by Microsoft.
