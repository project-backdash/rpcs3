#!/usr/bin/env python3
"""Writes SOURCE.md: what this fork changes against upstream RPCS3, read from git, plus the build notes.

Run from anywhere inside the repository:

    python tools/pbd_source_md.py [--rev HEAD | --staged] [--build TAG --exe-sha256 HASH] [--out SOURCE.md]

The file lists are generated from `git diff <base> <rev>` (or the staged tree with --staged, so that SOURCE.md can be
committed together with the changes it describes), so SOURCE.md always matches the tree it describes. Run it
again whenever files are added, removed or renamed, and at release time with the build tag and the SHA-256 of the
shipped rpcs3.exe.
"""
import argparse
import fnmatch
import subprocess
from pathlib import Path

BASE = "028d1e8f4a42df2292cdbcd35e540c9e71fc47aa"

# Corresponding source includes the complete committed tree, including this inventory and the native tests.
PATCH_EXCLUDES = []

# Module groups for the files the fork adds. The first group whose pattern matches a path takes it.
MODULES = [
    ("Frame tags and presentation",
     "Tags that tie each presented frame to the tick that built it, holding back frames that a correction is about to "
     "replace, and the pacing of catch-up frames.",
     ["rpcs3/Emu/RSX/pbd_frame_tags.h", "rpcs3/Emu/RSX/pbd_frame_presentation.h", "rpcs3/Emu/RSX/pbd_replay_pacing.h"]),
    ("Diagnostics",
     "Probe, write-watch and timeline helpers; each subsystem owns its diagnostic controls.",
     ["rpcs3/Emu/Cell/Modules/pbd_input_trace.h", "rpcs3/Emu/Cell/pbd_function_hook_config.h", "rpcs3/Emu/Cell/pbd_write_watch.cpp",
      "rpcs3/Emu/Cell/pbd_owner_timeline.h", "rpcs3/Emu/CPU/pbd_ordinary_metrics.*"]),
    ("Session, rollback and peer link",
     "The game's frame boundary, input delay, checkpoints of guest state, restore and re-execution of frames (the "
     "rollback itself), the peer protocol over UDP, and the CPU, memory and lv2 support a restore needs.",
     ["*/pbd_rollback_session.*", "*/pbd_rollback_session_*", "rpcs3/Emu/Cell/pbd_update_script.h",
      "rpcs3/Emu/Cell/pbd_peer_endpoint.h", "rpcs3/Emu/Cell/pbd_session_wait.h", "rpcs3/Emu/pbd_session_mode.h",
      "rpcs3/Emu/Cell/pbd_skip_guard.h", "rpcs3/Emu/Cell/pbd_fn_hook.*", "rpcs3/Emu/Cell/pbd_spu_checkpoint.h",
      "rpcs3/Emu/Cell/pbd_hle_pinned_order.inc", "rpcs3/*/pbd_session_history.h",
      "rpcs3/*/pbd_session_start.h",
      "rpcs3/*/pbd_t5dr_memory.h", "rpcs3/*/pbd_guest_call.h", "rpcs3/*/pbd_env*.h", "rpcs3/*/pbd_peer_message.h"]),
    ("Input",
     "Scripted and live controller input, controller takeover, and the hidden character select that a lobby set uses.",
     ["rpcs3/Emu/Cell/Modules/pbd_input_live.h", "rpcs3/Emu/Cell/Modules/pbd_input_script.h",
      "rpcs3/Emu/Cell/Modules/pbd_input_script_runtime.h", "rpcs3/Emu/Cell/pbd_controller_takeover.h",
      "rpcs3/Emu/Cell/pbd_hidden_selection.*", "rpcs3/Input/pbd_pad_*",
      "rpcs3/Input/pbd_controller_ports.*", "rpcs3/Emu/Cell/Modules/pbd_pad_*"]),
    ("Lobby sets",
     "The channel to the launcher for a set of games, the select countdowns, VS cards, the agreement between the two "
     "PCs at each game start and the direct start of a match.",
     ["rpcs3/Emu/Cell/pbd_set_channel.h", "rpcs3/Emu/Cell/pbd_lobby_set_driver.h", "rpcs3/Emu/Cell/pbd_select_band.h",
      "rpcs3/Emu/Cell/pbd_select_timer.h", "rpcs3/Emu/Cell/pbd_vs_cards.h", "rpcs3/Emu/Cell/pbd_setup_digest.h",
      "rpcs3/Emu/Cell/pbd_game_lifecycle.h", "rpcs3/Emu/Cell/pbd_direct_match.*", "rpcs3/Emu/Cell/pbd_boot_request.*",
      "rpcs3/*/pbd_rematch*", "rpcs3/*/pbd_character_rule*", "rpcs3/*/pbd_stage_rule*",
      "rpcs3/*/pbd_json.h"]),
    ("Practice",
     "Practice mode: the dummy's recordings and movement, wall positions, the Practice menus and their controls.",
     ["rpcs3/Emu/Cell/pbd_practice*", "rpcs3/*/pbd_practice_*"]),
    ("Replays",
     "Recording games as input and replaying them with pause, frame step, speed and takeover.",
     ["rpcs3/Emu/Cell/pbd_replay_game_file.h", "rpcs3/Emu/Cell/pbd_replay_viewer.h", "rpcs3/Emu/Cell/pbd_live_replay.*",
      "rpcs3/Emu/Cell/pbd_match_recording.h"]),
    ("Spectating",
     "Watching a lobby game from its confirmed inputs, with the watcher's own screen and chat.",
     ["rpcs3/Emu/Cell/pbd_spectator_channel.h", "rpcs3/Emu/Cell/pbd_spectator_ui.h", "rpcs3/Emu/Cell/pbd_corner_chat.h"]),
    ("Game window",
     "The game window hosted inside the launcher's window: the emulator draws into a child window, reads the keyboard "
     "as raw input, takes the launcher's window requests over a local pipe and records how frames and keys travelled "
     "for the run summary (see `docs/design/game-hosting.md`).",
     ["rpcs3/rpcs3qt/pbd_game_host*", "rpcs3/rpcs3qt/pbd_game_window*", "rpcs3/rpcs3qt/pbd_raw_keyboard.h", "rpcs3/Emu/PBD/pbd_presentation_*"]),
    ("Overlays, settings and sound",
     "The in-game menus, the netplay stats block, in-game settings, the player's volume, sound options, and the fonts and "
     "glyphs they draw with.",
     ["rpcs3/Emu/Cell/pbd_game_overlay.h", "rpcs3/Emu/Cell/pbd_settings_menu.h", "rpcs3/Emu/Cell/pbd_live_stats.h",
      "rpcs3/Emu/Cell/pbd_player_volume.h", "rpcs3/Emu/Cell/pbd_sound_options.*", "rpcs3/Emu/Cell/pbd_stage_names.h",
      "rpcs3/Emu/RSX/Overlays/*", "*/pbd_render_scale.h"]),
    ("Unit tests",
     "GoogleTest tests of the pieces that need no running emulator, built into upstream's `rpcs3_test` program with the "
     "rest of the solution (`pbd_tests.props` and `pbd_tests.cmake` list them). `rpcs3_test --gtest_filter=Pbd*` runs "
     "them with the normal GoogleTest results.",
     ["rpcs3/tests/pbd/*"]),
    ("Documentation, build scripts and generators",
     "The fork's own pages, design notes, the build scripts and dependency lock, the source checks, "
     "this file's generator and the generators of checked-in tables.",
     ["docs/*", "tools/*", "Utilities/*.mjs", "README*", "ARCHITECTURE.md", "KNOWN-DEBT.md", "CODE-BAR.md",
      ".clang-format-ignore", ".github/workflows/pbd-*"]),
]

STATIC_TAIL = """\
## Building (Windows, what we ship)

We build and test only the Visual Studio solution build for Windows x64. Upstream's CMake files are updated for the
new sources, but that path is not built or tested.

You need:

- Visual Studio 2026 with the C++ x64 tools (the build uses its MSBuild, CMake and Ninja components; release builds
  use MSVC 14.51);
- Git and Python 3 on `PATH`;
- Vulkan SDK 1.4.341.1 (LunarG).

Everything else is pinned with its hash in `tools/pbd/dependencies.lock.json`: Qt 6.11.2 for MSVC 2022 x64 (qtbase,
qtdeclarative, qttools, qtmultimedia, qtsvg, qttranslations), RPCS3's prebuilt LLVM 22.1.8, 7-Zip's `7zr.exe` and the
GoogleTest 1.8.1.8 NuGet package. The lock follows upstream's own `.ci/setup-windows.sh` at the base commit. All of it
goes inside the clone, under `build\\` and `packages\\`, which git ignores. Every path the scripts use is relative to
the repository.

1. In a source snapshot, run `python prepare.py --output C:\\pbd-emulator`, then enter that checkout. Preparation
   verifies the patch digest and reconstructed Git tree against the supplied source record. In a repository checkout
   instead, select the desired source revision in Git; no preparation is needed.
2. Fetch the dependencies, checking each against its hash, and the submodules. As in upstream's Windows CI, LLVM comes
   prebuilt and FAudio and Feral GameMode are left out:

   ```
   powershell -ExecutionPolicy Bypass -File tools\\pbd\\setup-dependencies.ps1
   ```

3. Install the Vulkan SDK to `build\\deps\\VulkanSDK\\1.4.341.1`, or pass its folder to the build script with
   `-VulkanSdk`.
4. Check the setup, then build Release x64:

   ```
   powershell -ExecutionPolicy Bypass -File tools\\pbd\\build-rpcs3.ps1 -CheckOnly
   powershell -ExecutionPolicy Bypass -File tools\\pbd\\build-rpcs3.ps1 -Jobs 4
   ```

   The result is `bin\\rpcs3.exe`, and the build log is under `build\\pbd-logs`. Both scripts stop before they start if
   the drive is too full for the job. The toolchain embeds time stamps and debug-file identifiers, so a rebuild matches
   a release in code, not in every byte.

The exported snapshot contains a sanitized source tree plus the verified patch, without private repository history.
Preparation fetches a shallow public upstream base. BUILDING.md separates this route from a repository checkout and
retains the inherited generic RPCS3 build reference below the fork recipe.

## Naming

Source files, namespaces, functions, log channels and environment variables use `pbd` / `PBD`
(`rpcs3/Emu/PBD/pbd_env.h` reads the `PBD_*` switches). A few strings keep the older spelling (`dr2026`, `DR2026`)
because players' installs and caches hold them: the PPU object-name prefix, the JIT link names, the tape and command
format lines, the first-run record schemas and the GDB capability.

## Licence

RPCS3 is free software under the GNU General Public License, version 2 (`LICENSE`, unchanged from upstream). The
PROJECT BACKDASH changes in this repository are under the same licence. The fonts in
`rpcs3/Emu/RSX/Overlays/PbdFonts` are under the SIL Open Font License 1.1 (`OFL-Barlow.txt`, `OFL-PBD.txt`).

## What this repository does not contain

No game, game update, firmware, licence file, save data, savestate, memory dump or checkpoint. To play you need your
own legally obtained copy of the game and your own PS3 firmware. The code does refer to the game: it checks the title
ID before enabling netplay features, and it holds guest memory addresses, SHA-256 digests of guest memory and short
sequences of the game's instruction words, which the emulator compares with the loaded game before it arms a hook.

## Contact

https://backdash.gg, deesam@backdash.gg
"""


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True, text=True,
                          encoding="utf-8").stdout


def module_of(path):
    for index, (_, _, patterns) in enumerate(MODULES):
        if any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns):
            return index
    return None


def changes(root, rev):
    """(status, path, added, removed) for every path that differs from the base, SOURCE.md left out."""
    stats = {}
    target = ["--cached", BASE] if rev is None else [BASE, rev]
    for line in git(root, "diff", "--numstat", "-M", *target).splitlines():
        added, removed, path = line.split("\t", 2)
        if " => " in path:
            path = path.split(" => ")[-1].replace("}", "").strip()
        stats[path] = (added, removed)
    rows = []
    for line in git(root, "diff", "--name-status", "-M", *target).splitlines():
        fields = line.split("\t")
        status, path = fields[0][0], fields[-1]
        if path == "SOURCE.md":
            continue
        added, removed = stats.get(path, ("-", "-"))
        rows.append((status, path, added, removed))
    return rows


def count(value):
    return "binary" if value == "-" else value


def render(root, rev, build, exe_sha256):
    base_line = git(root, "log", "-1", "--date=short", "--format=%ad, \"%s\"", BASE).strip()
    rows = changes(root, rev)
    added_lines = sum(int(a) for _, _, a, _ in rows if a != "-")
    removed_lines = sum(int(r) for _, _, _, r in rows if r != "-")
    by_status = {s: sum(1 for row in rows if row[0] == s) for s in "AMDR"}
    out = [
        "# PROJECT BACKDASH emulator: source",
        "",
        "Generated by `tools/pbd_source_md.py` from `git diff` against the upstream base; do not edit by hand.",
        "",
        "This repository is the emulator half of PROJECT BACKDASH (PBD, https://backdash.gg): rollback netplay for one",
        "PlayStation 3 title (title ID NPUB30009) in RPCS3, the open-source PlayStation 3 emulator. It is a modified copy",
        "of RPCS3, not an official RPCS3 project, and the RPCS3 team does not support it. Please report problems with it",
        "to PROJECT BACKDASH, not to RPCS3.",
        "",
        "## Base and size",
        "",
        f"- Upstream: RPCS3 commit [`{BASE}`](https://github.com/RPCS3/rpcs3/commit/{BASE}) ({base_line}), unchanged,",
        "  as a source base; snapshot preparation fetches it shallowly and includes no private repository history.",
        f"- Changes on top of it: {len(rows)} files ({by_status['A']} added, {by_status['M']} changed, "
        f"{by_status['D']} deleted, {by_status['R']} renamed), {added_lines} lines added and {removed_lines} removed,",
        "  not counting this file. Line counts describe the recorded native revision; exported build adapters may differ.",
    ]
    if build:
        out.append(f"- Release build: {build}" + (f", `rpcs3.exe` SHA-256 `{exe_sha256}`." if exe_sha256 else "."))
    else:
        out.append("- Executable build identity: not supplied with this source inventory.")
    excludes = " ".join(f"':(exclude){path}'" for path in PATCH_EXCLUDES)
    out += [
        "",
        "## The GPL source in the installer",
        "",
        "The installer's `SOURCE` folder carries these changes as one unified diff against the base, made with:",
        "",
        "```",
        f"git diff --binary {BASE[:7]} <release commit> -- ." + (f" {excludes}" if excludes else ""),
        "```",
        "",
        "The patch includes the complete committed tree, including this inventory and the native tests.",
        "",
        "## What the fork adds",
        "",
    ]
    grouped = {}
    other = []
    for row in rows:
        if row[0] in "MD":
            continue
        index = module_of(row[1])
        (grouped.setdefault(index, []) if index is not None else other).append(row)
    for index, (title, description, _) in enumerate(MODULES):
        members = grouped.get(index)
        if not members:
            continue
        out += [f"### {title}", "", description, "", "| File | Lines |", "|---|---|"]
        out += [f"| `{path}` | {count(added)} |" for _, path, added, _ in sorted(members, key=lambda row: row[1])]
        out.append("")
    if other:
        out += ["### Other added files", "", "| File | Lines |", "|---|---|"]
        out += [f"| `{path}` | {count(added)} |" for _, path, added, _ in sorted(other, key=lambda row: row[1])]
        out.append("")
    modified = sorted((row for row in rows if row[0] == "M"), key=lambda row: row[1])
    out += [
        "## Upstream files the fork changes",
        "",
        "In source files these are the hooks: the places where upstream code calls into the files above, or where",
        "upstream behaviour changes for a session. `README.md` and `.github/CONTRIBUTING.md` are the fork's own pages;",
        "upstream's README is kept as `README.rpcs3.md`. Every other upstream file is exactly upstream's.",
        "",
        f"PROJECT BACKDASH changed these files from September 2026 on; `git log {BASE[:8]}..HEAD -- <file>` lists each",
        "change with its date.",
        "",
        "| File | Added | Removed |",
        "|---|---|---|",
    ]
    out += [f"| `{path}` | {count(added)} | {count(removed)} |" for _, path, added, removed in modified]
    deleted = sorted(row[1] for row in rows if row[0] == "D")
    if deleted:
        out += [
            "",
            "## Upstream files the fork removes",
            "",
            "These point people to RPCS3's own support, funding and issue tracker, which do not cover this fork.",
            "",
        ]
        out += [f"- `{path}`" for path in deleted]
    out += ["", STATIC_TAIL]
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rev", default="HEAD", help="the commit to describe (default HEAD)")
    parser.add_argument("--staged", action="store_true", help="describe the staged tree instead of a commit")
    parser.add_argument("--build", help="release build tag, for example pbd-1.0.0")
    parser.add_argument("--exe-sha256", help="SHA-256 of the released rpcs3.exe")
    parser.add_argument("--out", help="output path (default SOURCE.md at the repository root)")
    args = parser.parse_args()
    root = Path(git(Path(__file__).resolve().parent, "rev-parse", "--show-toplevel").strip())
    text = render(root, None if args.staged else args.rev, args.build, args.exe_sha256)
    out = Path(args.out) if args.out else root / "SOURCE.md"
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
