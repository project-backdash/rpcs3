PROJECT BACKDASH emulator
=========================

This is the emulator half of PROJECT BACKDASH (PBD, https://backdash.gg): rollback netplay, Practice, replays and
spectating for one PlayStation 3 fighting game (title ID NPUB30009), built on RPCS3, the open-source PlayStation 3
emulator.

**This is not an RPCS3 project.** It is a modified copy of RPCS3, the RPCS3 team does not support it, and nothing here
is submitted to RPCS3. Please report problems with it to PROJECT BACKDASH, not to RPCS3. Upstream's own README,
including its contribution rules, is [README.rpcs3.md](README.rpcs3.md).

## What the fork adds

- **Rollback netplay:** a fixed frame boundary in the game's update, input delay, checkpoints of guest state each
  frame, restore and re-execution of frames when a remote input arrives late, and a peer protocol over UDP.
- **Lobby sets:** a channel to the PBD launcher for a set of games, with the game's own character and stage select
  under a countdown, and a check at every game start that both PCs agree.
- **Practice:** a dummy with recordings and movement loops, and Practice menus drawn over the game.
- **Replays and spectating:** games recorded as input, replayed with pause, frame step, speed and takeover, and watched
  live from the players' confirmed inputs.
- **Overlays and settings:** in-game menus, a connection line and an in-game settings window.

Everything else is upstream RPCS3 at commit
[`028d1e8f4a42df2292cdbcd35e540c9e71fc47aa`](https://github.com/RPCS3/rpcs3/commit/028d1e8f4a42df2292cdbcd35e540c9e71fc47aa).
[ARCHITECTURE.md](ARCHITECTURE.md) is the map of what the fork adds and where it hooks into RPCS3, and
[SOURCE.md](SOURCE.md) lists every file the fork adds or changes. The design notes are in [docs/design](docs/design).

## Building

Windows x64 only. You need Visual Studio 2026 with the C++ x64 tools, Git and Python 3 on PATH, the Vulkan SDK
1.4.341.1, and about 22 GB free on the clone's drive. Clone with Git: a ZIP download cannot build, because the scripts
check the upstream base commit in the history and fetch the submodules.

```
git clone <this repository's URL> project-backdash-rpcs3
cd project-backdash-rpcs3
powershell -ExecutionPolicy Bypass -File tools\pbd\setup-dependencies.ps1
powershell -ExecutionPolicy Bypass -File tools\pbd\build-rpcs3.ps1 -CheckOnly
powershell -ExecutionPolicy Bypass -File tools\pbd\build-rpcs3.ps1
```

The first script fetches the pinned dependencies into the clone and checks each against its hash. Install the Vulkan
SDK into `build\deps\VulkanSDK\1.4.341.1`, or pass its folder with `-VulkanSdk`. The result is `bin\rpcs3.exe`.
[BUILDING.md](BUILDING.md) and [SOURCE.md](SOURCE.md) have the details.

## Tests

`build\lib\Release-x64\rpcs3_test.exe --gtest_filter=Pbd*` runs the unit tests in `rpcs3/tests/pbd`. Formatting of
the code the fork adds: `python -m pip install clang-format==20.1.8`, then `python tools\pbd_check_format.py`.

## How changes are checked

The PBD workflow checks formatting. Eligible inherited pull-request jobs build and run unit tests; their event
conditions do not run a native test build on every fork push. Online matches compare confirmed-frame state hashes
on both peers and stop on a mismatch. Native behavior changes need that online check as well as the touched tests.

[CODE-BAR.md](CODE-BAR.md) lists the executable checks and review guidance. [KNOWN-DEBT.md](KNOWN-DEBT.md) records open
structural work; [SOURCE.md](SOURCE.md) gives current file and patch counts.

## Contributing

Report problems with PBD to PBD: use Report a problem in the launcher's Help, open an issue here, or write to the
address below. [CONTRIBUTING.md](.github/CONTRIBUTING.md) explains how to report a problem and how to change the code.

## Licence

The emulator, with PROJECT BACKDASH's changes, is free software under the GNU General Public License, version 2 only,
as upstream RPCS3 ([LICENSE](LICENSE)). Some files carry other licences:

| Path | Licence |
|---|---|
| `3rdparty/` and the submodules | each library's own licence, in its folder |
| `rpcs3/Emu/RSX/Overlays/PbdFonts/` | SIL Open Font License 1.1 (`OFL-PBD.txt`, `OFL-Barlow.txt`) |
| `rpcs3/Input/pbd_gamecontrollerdb.h` | zlib, from SDL_GameControllerDB (`pbd_gamecontrollerdb.LICENSE`) |

No game, firmware, save data or savestate is part of this repository. To play you need your own legally obtained copy
of the game and your own PS3 firmware.

## Contact

https://backdash.gg, deesam@backdash.gg
