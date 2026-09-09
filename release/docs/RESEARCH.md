# thWWW-switch research notes

Research was performed before selecting an implementation approach. The target is a distributable homebrew `.nro`, so retail GameMaker runner injection was studied for compatibility lessons but rejected as the final architecture.

## Game identification and data format

- Official page: <https://oligarchomp.itch.io/wonderful-waking-world>
  - Official free Windows release, version 1.0.1, English/Japanese, and original controller bindings.
- Steam page: <https://store.steampowered.com/app/1901490/__Wonderful_Waking_World/>
  - Confirms Oligarchomp and the 15 June 2022 release.
- PCGamingWiki: <https://www.pcgamingwiki.com/wiki/Touhou_Nemuri_Sekai_~_Wonderful_Waking_World>
  - Identifies GameMaker, a 60 FPS timing dependency, the Windows save location, and installation size.
- Local inspection of the official archive:
  - `data.win`: 12,054,952 bytes.
  - WAD version 17; GameMaker version 2022.5.0.0.
  - Eight rooms and 1,123 code entries.
  - Official archive SHA-256: `f81d50d17bd337c059016e57e77e21be3827b20027ab82dc2b69242d65062477`.
  - `data.win` SHA-256: `200dc6a8f5b1e0de8d76d531001bcb886f7c2cc74859a30f3d324f6f5bc782e3`.

## Existing GameMaker Switch-port methods

- GBAtemp, “Play/Port your GameMaker games on Nintendo Switch,” pages 1–7:
  <https://gbatemp.net/threads/play-port-your-gamemaker-games-on-nintendoswitch.519660/>
  - The historical technique injects `game.win` into a compatible retail Switch runner.
  - Exact GameMaker runner compatibility matters, external audio/data must accompany the game, and save-data mounts need explicit commit behavior.
  - Useful compatibility reference, but it does not produce a standalone open-source NRO.
- ZeusNX: <https://github.com/SoraStream/ZeusNX>
  - Automates newer runner-based packages but requires a legally sourced proprietary GameMaker Switch runtime and packaging tools.
  - Rejected because that architecture is not a self-contained homebrew NRO and cannot ship its required retail runtime.
- Ralcactus GameMaker NX ports: <https://github.com/Ralcactus/Gamemaker-NX-Ports>
  - Demonstrates real game-specific adaptations and calls out save commit handling after `ini_close()` when using Switch save-data mounts.
- SwitchPorts examples: <https://github.com/robzilla10001/SwitchPorts>
  - Supports the accepted packaging pattern used here: distribute the compatibility runtime, require users to supply original data.

## Open-source alternatives

- GameMaker Anywhere: <https://github.com/Ralcactus/GameMaker-Anywhere>
- GBAtemp announcement: <https://gbatemp.net/threads/gamemaker-anywhere-an-open-source-compiler-and-runtime-for-gms2.679710/>
  - Promising open-source GML-to-C/C++ work, but its documented targets are Wii, GameCube, and 3DS rather than Switch.
- WAD-version background: <https://casrielasriel.github.io/3ds-cinnamon-summary-thingy-or-what-the-fuck-ever/>
  - WAD 17 corresponds to modern GMS2 data and reinforces that runner/version compatibility is a substantive issue.
- Butterscotch: <https://github.com/ButterscotchRunner/Butterscotch>
  - Open source, WAD 17-capable, already has libnx/Switch support, accepts external `data.win`, and emits an NRO.
  - Selected as the base because it best fits the technical and legal requirements. The branch is based on commit `fa8491009cf49d7b7fc3cc35d6fc4d193bf626f6`.
- Known-good upstream Switch CI artifact reference:
  <https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-switch.zip>

## Toolchain and API references

- switchbrew/devkitPro setup: <https://switchbrew.org/wiki/Setting_up_Development_Environment>
- devkitPro pacman releases: <https://github.com/devkitPro/pacman/releases>
  - The port was built with devkitA64 GCC 15.2.0, libnx, Switch SDL2/OpenGL ES/OpenAL, and the devkitPro Switch CMake wrapper.
- GameMaker `audio_create_buffer_sound` manual:
  <https://manual.yoyogames.com/GameMaker_Language/GML_Reference/Asset_Management/Audio/Audio_Buffers/audio_create_buffer_sound.htm>
  - Defines buffer, sample format, sample rate, byte offset/length, channel mode, and explicit free semantics implemented by the runner.
- UndertaleModTool 0.9.2.0: <https://github.com/UnderminersTeam/UndertaleModTool/releases/tag/0.9.2.0>
  - Used only for analysis/decompilation to map game behavior and missing VM calls; no generated decompilation is shipped.

## Community/Discord search

Public community links found during research:

- Butterscotch / GameMaker ports: <https://discord.gg/cc2YA2pChU>
- Older GameMaker Anywhere community: <https://discord.gg/rNVK5NdY2r>

Discord message history was not publicly indexable from the available environment, so no private or inaccessible claims are represented as research results. Repository documentation and public forum discussions were used instead.

## Saekaze control and repository precedent

- Touhou 7 Switch release thread: <https://gbatemp.net/threads/touhou-7-switch-port.683780/>
- Source repository: <https://github.com/Saekaze/th07-switch>
- Controller implementation: <https://raw.githubusercontent.com/saekaze/th07-switch/main/src/Controller.cpp>
  - On Switch, Saekaze bypasses configurable mappings and uses fixed physical actions.
  - B is shoot/confirm, A is bomb/cancel, L is focus, Plus is pause, and R sets a separate held `TH_BUTTON_SKIP` action equivalent to keyboard Ctrl.
  - This is why thWWW-switch does not implement R by globally aliasing it to shoot. thWWW's own dialogue bytecode was inspected and its held-skip path was bridged only while `obj_dialogue` reads `global.shot_down`.

## Architecture decision

The selected design is a customized native AArch64 build of the open-source Butterscotch VM:

1. The NRO contains the runner, libnx platform code, GLES renderer, OpenAL backend, and thWWW-specific compatibility work.
2. The user places the official free release data at `/switch/thwww/`.
3. Runtime writes go to `/switch/thwww/save/`; shipped data remains unchanged.
4. No proprietary GameMaker Switch runtime, NSP, LayeredFS patch, or Windows executable is distributed.
