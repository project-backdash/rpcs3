# Project Backdash RPCS3 fork

This is the RPCS3 emulator with Project Backdash's netplay and related changes, based on [RPCS3](https://github.com/RPCS3/rpcs3) commit `028d1e8f4a42df2292cdbcd35e540c9e71fc47aa`. Credit for RPCS3 belongs to its contributors. This fork is independent of the RPCS3 project.

For release source and information, see https://backdash.gg/source/. The emulator is licensed under GPL version 2; see [COPYING](COPYING) and [LICENSE](LICENSE).

## Build on Windows x64

Install Visual Studio 2026 with C++ x64 tools, Git, Python 3, and Vulkan SDK 1.4.341.1. Use a short checkout path and allow at least 22 GiB of free space. From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools\pbd\setup-dependencies.ps1
powershell -ExecutionPolicy Bypass -File tools\pbd\build-rpcs3.ps1 -CheckOnly
powershell -ExecutionPolicy Bypass -File tools\pbd\build-rpcs3.ps1
```

The setup script downloads and verifies pinned dependencies. Install the Vulkan SDK at `build\deps\VulkanSDK\1.4.341.1`, or pass its path via `-VulkanSdk` to the build script. The output is `bin\rpcs3.exe`. See [BUILDING.md](BUILDING.md) for details and [README.rpcs3.md](README.rpcs3.md) for upstream documentation.
