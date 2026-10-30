# Checks and contribution guidance

The native fork's automated formatting check is `.github/workflows/pbd-bar.yml`. It installs clang-format 20.1.8,
checks C++ files added by the fork, and checks changed C++ lines on pull requests:

```powershell
python tools/pbd_check_format.py
python tools/pbd_check_format.py --changed <merge-base>
```

`.github/workflows/pbd-windows.yml` builds Release x64 and runs PBD and tools tests on pushes to `main` and pull
requests. It uses the upstream Windows CI dependency setup. The inherited `.github/workflows/rpcs3.yml` has its own
build and unit-test jobs with repository/event restrictions. The formatting workflow does not enforce file size,
function size, environment registration, clang-tidy or a new warning policy. Line-count rules and their historical
exception ledger are retired; a file should contain the least code its job needs.

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
structural work; it is not a list of automatic checks or a substitute for reading their output.
