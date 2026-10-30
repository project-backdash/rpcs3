#!/usr/bin/env python3
"""Checks the C++ formatting rule of CODE-BAR.md with clang-format and the repository's .clang-format.

    python tools/pbd_check_format.py [--clang-format PATH] [--changed REV]

Without --changed, every C++ file the fork adds on top of upstream must be formatted; the data tables listed in
.clang-format-ignore are skipped. With --changed REV, only the lines changed since REV are checked, in every C++
file: a finding on an unchanged line is not reported, because upstream's own code is not formatted throughout and a
change can sit inside it.

The formatter is pinned to clang-format 20.1.8, since other versions format the same code differently.
Exit code: 0 when formatted, 1 when something is not, 2 when git or clang-format cannot run.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pbd_source_md import BASE  # noqa: E402

VERSION = "20.1.8"
CPP = re.compile(r"\.(h|hpp|cpp|cc|inc)$")
FINDING = re.compile(r"^(.+?):(\d+):(\d+): (?:warning|error): code should be clang-formatted", re.M)
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", re.M)
CHUNK = 50  # files per clang-format call, to stay under the Windows command-line limit


def run(args, cwd):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")


def git(root, *args):
    out = run(["git", *args], root)
    if out.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {out.stderr.strip()}")
    return out.stdout


def cpp_files(root, *diff_args):
    return [name for name in git(root, "diff", "--name-only", *diff_args).splitlines() if CPP.search(name)]


def changed_ranges(root, rev, path):
    ranges = []
    for m in HUNK.finditer(git(root, "diff", "-U0", rev, "HEAD", "--", path)):
        count = int(m.group(2)) if m.group(2) is not None else 1
        if count:
            ranges.append((int(m.group(1)), int(m.group(1)) + count - 1))
    return ranges


def findings(clang_format, root, paths, extra=()):
    out = run([clang_format, "--dry-run", "--style=file", *extra, *paths], root)
    found = [(m.group(1), int(m.group(2)), int(m.group(3))) for m in FINDING.finditer(out.stderr)]
    diagnostics = [line for line in out.stderr.splitlines() if re.search(r"\b(?:error|warning):", line)]
    unexpected = [line for line in diagnostics if not FINDING.match(line)]
    if unexpected or (out.returncode and (out.returncode != 1 or not found)):
        raise RuntimeError(f"clang-format failed ({out.returncode}): {out.stderr.strip() or out.stdout.strip()}")
    return found


def check_added(clang_format, root):
    paths = cpp_files(root, "--diff-filter=A", BASE, "HEAD")
    found = []
    for k in range(0, len(paths), CHUNK):
        found += findings(clang_format, root, paths[k:k + CHUNK])
    return len(paths), found


def check_changed(clang_format, root, rev):
    paths = cpp_files(root, "--diff-filter=AM", rev, "HEAD")
    found = []
    for path in paths:
        ranges = changed_ranges(root, rev, path)
        if not ranges:
            continue
        lines = {n for a, b in ranges for n in range(a, b + 1)}
        hits = findings(clang_format, root, [path], [f"--lines={a}:{b}" for a, b in ranges])
        found += [hit for hit in hits if hit[1] in lines]
    return len(paths), found


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang-format", default="clang-format", help="the clang-format program (default: on PATH)")
    parser.add_argument("--changed", metavar="REV", help="check only the lines changed since REV")
    args = parser.parse_args(argv)
    try:
        root = git(Path.cwd(), "rev-parse", "--show-toplevel").strip()
        version = run([args.clang_format, "--version"], root).stdout
        if VERSION not in version:
            print(f"clang-format {VERSION} is required, found: {version.strip() or 'none'}", file=sys.stderr)
            return 2
        if args.changed:
            count, found = check_changed(args.clang_format, root, args.changed)
            scope = f"lines changed since {args.changed} in {count} C++ files"
        else:
            count, found = check_added(args.clang_format, root)
            scope = f"{count} C++ files added on top of upstream"
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 2
    for path, line, column in found:
        print(f"{path}:{line}:{column}: not formatted")
    print(f"{scope}: {len(found)} findings")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
