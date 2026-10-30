#!/usr/bin/env python3
"""Reproduce the checkpoint-compatible HLE include from PBD_HLE_ORDER_DUMP output."""
import argparse
import hashlib
import json
from pathlib import Path

SLOTS = 3875
DUMP_SHA256 = "a30d55cccc2651b886cf71b542be42e8199151abe6e0c0c829195a9706df410a"
SLOT_DIGEST = "3677ef020c937593"
HEADER = (
    "// PBD pinned HLE slot order: dumped from a build whose natural slot order matches the slot addresses stored in the\n"
    "// checkpoints. One key per slot index. Do not edit by hand; regenerate only with a savestate-compatible build.\n"
)


def convert(raw: bytes) -> str:
    text = raw.decode("ascii").replace("\r\n", "\n")
    keys = []
    for index, line in enumerate(text.splitlines()):
        slot, separator, key = line.partition("\t")
        if slot != str(index) or separator != "\t" or not key or any(ord(c) < 32 or ord(c) > 126 for c in key):
            raise ValueError(f"invalid slot record at index {index}")
        keys.append(key)
    if len(keys) != SLOTS or len(set(keys)) != SLOTS:
        raise ValueError("slot count or key uniqueness differs from the checkpoint table")
    canonical = "".join(f"{index}\t{key}\n" for index, key in enumerate(keys)).encode("ascii")
    if hashlib.sha256(canonical).hexdigest() != DUMP_SHA256:
        raise ValueError("dump identity differs from the pinned checkpoint input")
    digest = 0xCBF29CE484222325
    for key in keys:
        for byte in (key + "\n").encode("ascii"):
            digest = ((digest ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    if f"{digest:016x}" != SLOT_DIGEST:
        raise ValueError("HLE slot digest differs from the checkpoint table")
    return HEADER + "".join(json.dumps(key, ensure_ascii=True) + ",\n" for key in keys)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "rpcs3/Emu/Cell/pbd_hle_pinned_order.inc")
    parser.add_argument("--check", action="store_true", help="compare without writing the include")
    args = parser.parse_args()
    try:
        result = convert(args.dump.read_bytes()).encode("ascii")
        if args.check:
            if args.output.read_bytes().replace(b"\r\n", b"\n") != result:
                raise ValueError("checked-in include differs from the compatible dump")
        else:
            args.output.write_bytes(result)
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"{SLOTS} compatible slots; digest {SLOT_DIGEST}; include {'matches' if args.check else 'written'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
