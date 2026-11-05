"""Read custom-look cards and actor pointer candidates from complete DRMD001 memory dumps.

This is an observation, not a determinism verdict. Missing pages, truncated dumps and duplicate pages are errors.
Pointer candidates are reported without excluding any bytes from the game's combat hash.
"""
import argparse
import json
from pathlib import Path
import struct

PAGE_SIZE = 4096
ACTORS = (0x10090040, 0x10090910)
STRIDE = 0x8D0
CARDS = (0x106C9934, 0x106C9B28)
KNOWN_POINTERS = {0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0xBC, 0xC0, 0xC4,
                  0x2BC, 0x300, 0x320, 0x324, 0x530, 0x580, 0x5D0, 0x7E4,
                  0x818, 0x81C, 0x828, 0x82C, 0x83C, 0x840, 0x850, 0x854,
                  0x864, 0x868, 0x878, 0x87C, 0x88C, 0x890, 0x8A0, 0x8A4,
                  0x8A8, 0x8AC, 0x8C0}


class Dump:
    def __init__(self, data):
        if len(data) < 12 or data[:8] != b"DRMD001\0":
            raise ValueError("Not a DRMD001 dump")
        count, = struct.unpack_from("<I", data, 8)
        if count == 0 or count > 131072 or len(data) != 12 + count * (4 + PAGE_SIZE):
            raise ValueError("Incomplete or oversized DRMD001 dump")
        self.data, self.pages = data, {}
        for offset in range(12, len(data), 4 + PAGE_SIZE):
            address, = struct.unpack_from("<I", data, offset)
            if address % PAGE_SIZE or address in self.pages:
                raise ValueError("Duplicate or unaligned dump page")
            self.pages[address] = offset + 4

    def read(self, address, size):
        if address < 0 or size < 0 or address + size > 2**32:
            raise ValueError("Guest read outside address space")
        parts = []
        while size:
            page = address & ~(PAGE_SIZE - 1)
            if page not in self.pages:
                raise ValueError(f"Required page {page:#x} is absent")
            take = min(size, PAGE_SIZE - (address - page))
            offset = self.pages[page] + address - page
            parts.append(self.data[offset:offset + take])
            address, size = address + take, size - take
        return b"".join(parts)

    def word(self, address, size=4):
        return int.from_bytes(self.read(address, size), "big")


def observe(dump):
    sides = []
    for side, actor in enumerate(ACTORS):
        blocks = []
        for costume in range(3):
            block = dump.read(CARDS[side] + costume * 0x58, 0x58)
            blocks.append({"items": list(struct.unpack_from(">4H", block, 0x40)),
                           "colours": list(struct.unpack_from(">3b", block, 0x54))})
        pointers = []
        for offset in range(0, STRIDE, 4):
            word = dump.word(actor + offset)
            # A mapped, aligned word is only a candidate; numeric fields can coincidentally name mapped memory.
            if word and word % 4 == 0 and (word & ~0xFFF) in dump.pages:
                pointers.append({"offset": hex(offset), "value": hex(word), "known": offset in KNOWN_POINTERS})
        sides.append({"fighter": dump.word(actor + 0x40, 2), "row": dump.word(actor + 0x42, 2),
                      "model": hex(dump.word(actor + 0x8A4)), "blocks": blocks,
                      "pointer_candidates": pointers,
                      "unknown_pointer_candidates": [p for p in pointers if not p["known"]]})
    return {"mode": dump.word(0x1008EBAC), "scene": dump.word(0x1008F070),
            "pending": dump.word(0x1008F074), "humans": dump.word(0x1008F0B0), "sides": sides}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dumps", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = {"schema": "pbd-custom-memory-observations-1", "determinism_proven": False, "observations": []}
    for path in args.dumps:
        report["observations"].append({"path": str(path), **observe(Dump(path.read_bytes()))})
    text = json.dumps(report, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
