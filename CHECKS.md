# Checks and contribution guidance

The native fork's automated checks are in `.github/workflows/pbd-checks.yml`. It installs clang-format 20.1.8, checks
C++ files added by the fork, checks changed C++ lines on pull requests, and checks that `rpcs3/Emu/PBD/core` includes
and names nothing of the emulator (the two halves are described in [ARCHITECTURE.md](ARCHITECTURE.md#layout)):

```powershell
python tools/pbd_check_format.py
python tools/pbd_check_format.py --changed <merge-base>
python tools/pbd_check_core.py
```

After a change under `core`, also run `python tools/pbd_check_core.py --compile` from a Visual Studio developer
prompt: it compiles every core header on its own, which the workflow does not do.

The inherited `.github/workflows/rpcs3.yml` has its own build and unit-test jobs. Eligible pull requests run
`rpcs3_test`; repository/event conditions limit fork push builds. No check limits the size of a file or a function:
a file should contain the least code its job needs.

Build instructions are in [BUILDING.md](BUILDING.md). Run the existing tests for the changed code, for example:

```powershell
build\lib\Release-x64\rpcs3_test.exe --gtest_filter=Pbd*
```

For review, keep a module focused on one concern and make ownership, failure and synchronization explicit. Prefer
removing duplicate state and forwarding layers over moving them to more files. Keep network, disk, guest-memory and
IPC validation at their boundaries. Preserve guest-memory writes, tick order, wire bytes and saved formats during
structural changes. Behavior fixes need a regression case through production logic; online execution changes also
need a match with confirmed-frame sync checks on both peers.

Use `pbd` names for new code and `PBD_*` for environment settings. Stored/cache/wire identifiers keep their bytes.
Comments explain why in a few lines; history belongs in commits. Generated tables should identify their generator
and compatibility inputs. Update ARCHITECTURE.md when an entry point or contract changes, and regenerate SOURCE.md
from the staged tree with `python tools/pbd_source_md.py --staged` before committing source inventory changes.

[CONTRIBUTING.md](.github/CONTRIBUTING.md) covers reports and patches. [KNOWN-DEBT.md](KNOWN-DEBT.md) records open
structural work.
