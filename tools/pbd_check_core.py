#!/usr/bin/env python3
"""Checks that rpcs3/Emu/PBD/core stands without the emulator.

    python tools/pbd_check_core.py              includes and names, read from the sources
    python tools/pbd_check_core.py --compile    also compiles every core header on its own

A file under core may include the C++ standard library, operating system and library headers, other core files, the
RPCS3 headers named in ALLOWED, and third-party sources by a path into 3rdparty/. Code that needs more of RPCS3 (guest
memory, a PPU thread, RSX, the system object, its utilities) belongs under rpcs3/Emu/PBD/emulator; ARCHITECTURE.md
describes the two halves.

An include list is not the whole story. A file can use the emulator through whatever includes it: a "steps" header
that defines a member of the session object, a header that logs through a channel declared elsewhere. So the check
also looks for the emulator's names in core's code. --compile gives the full answer: each core header must compile as
the only include of a source file. It needs MSVC's cl on PATH (a Visual Studio developer prompt) and the submodules.

Exit code: 0 when core is clean, 1 on a finding, 2 when git or the compiler cannot run.
"""
import argparse
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

CORE = "rpcs3/Emu/PBD/core/"
# RPCS3 headers without emulator state that core may use: the fixed-width integer names, and the SHA-256 that RPCS3
# bundles with its helpers.
ALLOWED = {"rpcs3/util/types.hpp", "rpcs3/Crypto/sha256.h", "rpcs3/Crypto/utils.h"}
CPP = re.compile(r"\.(h|hpp|cpp|inc)$")
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*(?:"([^"\n]+)"|<([^>\n]+)>)', re.M)
# Where the compilers look after the including file's own folder.
ROOTS = ("rpcs3", "")

FS_NAME = re.compile(r"(?<![\w:])fs::")
# What only the emulator declares. A core file that uses one of these compiles only inside an emulator source file.
EMULATOR_NAMES = (
    (re.compile(r"\bEmu\."), "the system object (Emu.)"),
    (re.compile(r"\bvm::"), "guest memory (vm::)"),
    (re.compile(r"\bg_(?:cfg|fxo)\b"), "the emulator's configuration or state (g_cfg, g_fxo)"),
    # The spectator UI has a namespace of button constants called pad; these two are the emulator's.
    (re.compile(r"(?<![\w:])pad::(?:g_pad_mutex|get_pad_thread)\b"), "the pad thread (pad::)"),
    (re.compile(r"\brsx::"), "the renderer (rsx::)"),
    (re.compile(r"\b(?:ppu|cpu|spu)_thread\b"), "an emulator thread"),
    (FS_NAME, "RPCS3's file utilities (fs::)"),
    (re.compile(r"\b\w+_log\.(?:error|warning|notice|success|fatal|trace|todo)\("), "an RPCS3 log channel"),
    (re.compile(r"\brollback_session::"), "a member of the rollback session object"),
)
# A file may name the standard library's filesystem `fs` for itself.
OWN_FS = re.compile(r"\bnamespace\s+fs\s*=\s*std::filesystem\b")
# Comments, raw strings, strings and character literals. A quote straight after a digit separates digits (1'000).
NOT_CODE = re.compile(
    r"""//[^\n]*|/\*.*?\*/|R"([^()\s\\]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|(?<![0-9A-Fa-f])'(?:\\.|[^'\\\n])*'""",
    re.S)

# The definitions of buildfiles/msvc/rpcs3_default.props that change what a header sees.
DEFINES = ("NOMINMAX", "_ENABLE_EXTENDED_ALIGNED_STORAGE", "_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING",
           "_HAS_EXCEPTIONS=0")
COMPILER_ERROR = re.compile(r": (?:fatal )?error ")


def resolve(tracked, folder, written, quoted):
    """The tracked file an include names, or the path it reaches when git does not track it (a system or library
    header, or a file inside a submodule)."""
    bases = ((folder,) if quoted else ()) + ROOTS
    for base in bases:
        candidate = posixpath.normpath(posixpath.join(base, written))
        if candidate in tracked:
            return candidate
    return posixpath.normpath(posixpath.join(folder, written)) if quoted and written.startswith(".") else written


def findings(tracked, path, text):
    """(line, include as written) for every include of `path` that leaves core."""
    found = []
    for match in INCLUDE.finditer(text):
        written = match.group(1) or match.group(2)
        target = resolve(tracked, posixpath.dirname(path), written, quoted=match.group(1) is not None)
        if target.startswith(CORE) or target in ALLOWED or target.startswith("3rdparty/") or target not in tracked:
            continue
        found.append((text.count("\n", 0, match.start()) + 1, written))
    return found


def code_only(text):
    """`text` with its comments and its string and character literals blanked. Every line stays on its line."""
    return NOT_CODE.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)


def uses(text):
    """(line, what it is) for each of the emulator's names that the code of `text` uses, at its first use."""
    code = code_only(text)
    own_fs = OWN_FS.search(code) is not None
    found = []
    for pattern, what in EMULATOR_NAMES:
        match = None if own_fs and pattern is FS_NAME else pattern.search(code)
        if match:
            found.append((code.count("\n", 0, match.start()) + 1, what))
    return sorted(found)


def unit(header):
    """The source file that includes `header` and nothing else, the way the emulator's own sources name it."""
    return f'#include "{posixpath.relpath(header, "rpcs3")}"\n'


def compile_alone(root, headers, jobs):
    """(header, the compiler's first errors) for every header that does not compile as a source file's only include.
    Nothing is written into the tree: cl parses and stops (/Zs)."""
    compiler = shutil.which("cl")
    if compiler is None:
        raise OSError("cl is not on PATH: run --compile from a Visual Studio developer prompt")
    command = [compiler, "/nologo", "/Zs", "/std:c++latest", "/utf-8", "/EHsc", "/W0", "/constexpr:steps16777216",
               *(f"/D{name}" for name in DEFINES), f"/I{root / 'rpcs3'}", f"/I{root}"]

    with tempfile.TemporaryDirectory() as folder:
        def attempt(numbered):
            index, header = numbered
            source = Path(folder) / f"{index}.cpp"
            source.write_text(unit(header), encoding="utf-8")
            done = subprocess.run([*command, str(source)], cwd=folder, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", check=False)
            if done.returncode == 0:
                return None
            errors = [line.strip().replace(f"{root}{os.sep}", "") for line in done.stdout.splitlines()
                      if COMPILER_ERROR.search(line)]
            return header, errors[:3]

        with ThreadPoolExecutor(max_workers=jobs) as pool:
            return [failed for failed in pool.map(attempt, enumerate(headers)) if failed]


def main():
    parser = argparse.ArgumentParser(description="Checks that rpcs3/Emu/PBD/core stands without the emulator.")
    parser.add_argument("--compile", action="store_true", help="also compile every core header on its own (needs cl)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="compiles to run at once")
    arguments = parser.parse_args()
    try:
        root = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], check=True, capture_output=True, text=True,
                                   encoding="utf-8").stdout.strip())
        listed = subprocess.run(["git", "-C", str(root), "ls-files"], check=True, capture_output=True, text=True,
                                encoding="utf-8").stdout
    except (OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 2
    tracked = set(listed.splitlines())
    core = sorted(name for name in tracked if name.startswith(CORE) and CPP.search(name))
    total = 0
    for path in core:
        text = (root / path).read_text(encoding="utf-8", errors="replace")
        for line, written in findings(tracked, path, text):
            print(f"{path}:{line}: includes {written}, which is outside core")
            total += 1
        for line, what in uses(text):
            print(f"{path}:{line}: uses {what}, which only the emulator declares")
            total += 1
    if arguments.compile:
        headers = [path for path in core if path.endswith((".h", ".hpp"))]
        try:
            failed = compile_alone(root, headers, max(1, arguments.jobs))
        except OSError as error:
            print(error, file=sys.stderr)
            return 2
        for header, errors in failed:
            print(f"{header}: does not compile on its own")
            for line in errors:
                print(f"    {line}")
        print(f"{len(headers)} core headers compiled alone: {len(failed)} failed")
        total += len(failed)
    print(f"{len(core)} files under {CORE}: {total} findings")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
