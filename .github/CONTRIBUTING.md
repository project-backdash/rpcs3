# Contributing to the PROJECT BACKDASH emulator

This repository is the emulator half of PROJECT BACKDASH (PBD). It is a modified copy of RPCS3, not an RPCS3 project.
Please never report PBD problems to RPCS3 or ask for PBD help in RPCS3's channels: the RPCS3 team does not support this
fork.

## Reporting a problem

- In the PBD launcher, use **Report a problem** in Help. You review the recent logs and PC details before they are sent,
  and they are what we need to find the cause.
- Or open an issue here, or write to deesam@backdash.gg. Say which PBD version you run, what you were doing (an online
  set, Practice, a replay or spectating), what happened and what you expected.
- Never attach game files, firmware, save data or savestates.

## Changing the code

Start with [ARCHITECTURE.md](../ARCHITECTURE.md), the map of what the fork adds and where it hooks into RPCS3.

- [CODE-BAR.md](../CODE-BAR.md) describes the formatting and test commands that run, and the separate review
  guidance. [KNOWN-DEBT.md](../KNOWN-DEBT.md) records open structural work without implying that CI enforces it.
- Build with the steps in [SOURCE.md](../SOURCE.md#building-windows-what-we-ship).
  `tools\pbd\build-rpcs3.ps1 -CheckOnly` tells you what is missing without building.
- The unit tests are GoogleTest tests in `rpcs3\tests\pbd`, one `<module>_test.cpp` each, listed in
  `pbd_tests.props` and `pbd_tests.cmake` there. The solution build makes them into
  `build\lib\Release-x64\rpcs3_test.exe`; `rpcs3_test --gtest_filter=Pbd*` runs them. Use `check` from `pbd_test.h`,
  which also works in release builds. Run the tests of the code you touched, and add one for a bug fix that fails
  without the fix.
- Changes to the netcode, the session code or the emulator core also need a set played between two PCs. During online
  play both PCs compare their game state every frame, so a difference shows up as soon as it happens.
- Commits: a subject of 72 characters or less that says what changed, a body that says why and carries the evidence,
  and one logical change per commit.
- Comments say why the code is the way it is. They never name people, tools or the history of a change; git keeps the
  history.

## Upstream RPCS3

A fix here that would also help RPCS3 is offered to RPCS3 under its own rules (see [README.rpcs3.md](../README.rpcs3.md)),
by a person who owns and understands the change.

## Licence

By contributing, you agree that your contribution is licensed under the GNU General Public License, version 2 only,
like the rest of RPCS3 ([LICENSE](../LICENSE)).
