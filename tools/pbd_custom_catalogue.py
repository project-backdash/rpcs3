#!/usr/bin/env python3
"""Decode the game's custom-look catalogue: which items and colours each costume row can wear.

The decrypted EBOOT's archive index holds a record per packed file of T55DATA1.BIN (sector, stored size, zlib size,
checksum, 0, XOR key, 0; big-endian). The record at file offset 0x4c5670 is the item catalogue: a pack of one PAC per row
of the costume row table (0x4403c0), which is what the shop reads through 0x151d1c (pack index = the row, 0x59270).
Each PAC holds, by name:
  <code>_parts.pac/<code>_{heads,faces,uppers,lowers}.pac/<code>_NNNN_<part>.iif  one item each ("PANTERA" record)
  <code>_col.sel                                                                 the three colour lists
  <fighter>_itemname.dat                                                         the fighter's item names
PAC entry headers are XOR-ed with ff f0 00 ff; their contents are plain little-endian data. The EBOOT's English build
renames a few items per fighter (table 0x100764d8, item id -> name).

Writes a JSON file with every row's items (ids, names, the costumes each serves) and colours, and the C++ header that
validates looks (rpcs3/Emu/PBD/pbd_custom_catalogue.h): numbers only, no game text.

Usage: python tools/pbd_custom_catalogue.py <exec.elf> <T55DATA1.BIN> --json <out.json> [--header <out.h>]
"""

import argparse
import hashlib
import json
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "rpcs3", "Emu", "PBD", "pbd_custom_catalogue.h")

ELF_SHA256 = "b56e25e9210ed3c4fc19f3dfae686b7246aef386cdd4f88c80e0b89141ff887d"
CATALOGUE_RECORD = 0x4C5670
ROW_TABLE, ROW_SIZE, ROWS = 0x4403C0, 0x24, 86
NAME_OVERRIDES, FIGHTERS = 0x100764D8, 35
PAC_KEY = bytes((0xFF, 0xF0, 0x00, 0xFF))
PARTS = ("head", "face", "upper", "lower")
# The part bit an item record names (+0x0c) and the folder it sits in.
PART_BITS = {1: 0, 2: 1, 4: 2, 8: 3}
FOLDERS = {"heads": 0, "faces": 1, "uppers": 2, "lowers": 3}
# Item ids the shop compares by their low byte only (0x29e754, 0x29e7a4); the header stores 128-bit sets.
MAX_ITEM_ID = 127
COLOUR_PARTS = 3
# The fighter loader dresses costumes 0 and 1, and costume 2 only for Xiaoyu (7) and Asuka (24) (0x14d698-0x14d7ac).
THIRD_COSTUME_FIGHTERS = (7, 24)
FIGHTER_NAMES = ("Paul", "Law", "Lei", "King", "Yoshimitsu", "Nina", "Hwoarang", "Xiaoyu", "Christie", "Jin", "Julia",
                 "Kuma", "Bryan", "Heihachi", "Kazuya", "Lee", "Steve", "Marduk", "Mokujin", "Jack-5", "Roger Jr.", "Anna",
                 "Wang", "Ganryu", "Asuka", "Bruce", "Baek", "Devil Jin", "Raven", "Feng", "Armor King", "Lili", "Dragunov",
                 "Eddy", "Jinpachi")


class Elf:
    """The loadable segments of a big-endian 64-bit ELF, read by guest address."""

    def __init__(self, data):
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
            raise ValueError("not a big-endian 64-bit ELF")
        phoff, = struct.unpack_from(">Q", data, 0x20)
        size, count = struct.unpack_from(">HH", data, 0x36)
        self.data = data
        self.segments = []
        for i in range(count):
            kind, _, offset, vaddr, _, filesz = struct.unpack_from(">IIQQQQ", data, phoff + i * size)
            if kind == 1 and filesz:
                self.segments.append((vaddr, offset, filesz))

    def read(self, address, length):
        for vaddr, offset, filesz in self.segments:
            if vaddr <= address and address + length <= vaddr + filesz:
                return self.data[offset + address - vaddr:offset + address - vaddr + length]
        raise ValueError(f"0x{address:x} is outside the ELF's loaded data")

    def word(self, address):
        return struct.unpack(">I", self.read(address, 4))[0]

    def text(self, address, limit=64):
        raw = self.read(address, limit)
        return raw.split(b"\0")[0].decode("latin-1")


def xor_stream(data, key):
    """The archive's XOR stream: little-endian words of key, key * 5 + 3, ..., restarting every 2048-byte sector."""
    sector = bytearray()
    for _ in range(512):
        sector += key.to_bytes(4, "little")
        key = (key * 5 + 3) & 0xFFFFFFFF
    out = bytearray(data)
    for i in range(len(out)):
        out[i] ^= sector[i & 2047]
    return bytes(out)


def archive_file(elf_data, archive, record):
    sector, stored, unpacked, _, _, key, _ = struct.unpack_from(">7I", elf_data, record)
    archive.seek(sector * 2048)
    raw = archive.read(stored)
    if len(raw) != stored:
        raise ValueError("the archive ends inside the catalogue")
    data = zlib.decompress(xor_stream(raw, key))
    if len(data) != unpacked:
        raise ValueError("the catalogue's unpacked size differs from its index record")
    return data


def pack(data):
    """A pack: u32 count, then (offset, size) per part."""
    count, = struct.unpack_from("<I", data, 0)
    parts = []
    for i in range(count):
        offset, size = struct.unpack_from("<II", data, 4 + i * 8)
        if offset + size > len(data):
            raise ValueError(f"pack part {i} runs past the end")
        parts.append(data[offset:offset + size])
    return parts


def unxor(data):
    return bytes(b ^ PAC_KEY[i & 3] for i, b in enumerate(data))


def pac(data):
    """A PAC: "PAC\\x01", u32, u32 header size, u32 count, then per entry (XOR-ed) u32 length, name, offset, size."""
    if data[:4] != b"PAC\x01":
        raise ValueError("not a PAC")
    _, header, count = struct.unpack_from("<III", data, 4)
    entries, at = [], 16
    for _ in range(count):
        length, = struct.unpack("<I", unxor(data[at:at + 4]))
        body = unxor(data[at + 4:at + 4 + length])
        if length < 8 or at + 4 + length > header:
            raise ValueError("PAC entry outside its header")
        name = body[:length - 8].split(b"\0")[0].decode("ascii")
        offset, size = struct.unpack_from("<II", body, length - 8)
        if offset + size > len(data):
            raise ValueError(f"PAC entry {name} runs past the end")
        entries.append((name, data[offset:offset + size]))
        at += 4 + length
    return entries


def files(data, prefix=""):
    """Every file in a PAC and the PACs inside it, by path."""
    out = {}
    for name, body in pac(data):
        if body[:4] == b"PAC\x01":
            out.update(files(body, prefix + name + "/"))
        else:
            out[prefix + name] = body
    return out


def item(data):
    """An item record: "PANTERA", id, part bit, costume mask, price, ..., then one word per costume it serves."""
    if data[:8] != b"PANTERA\0" or len(data) < 0x98 or len(data) % 4:
        raise ValueError("not an item record")
    ident, bit, costumes, price = struct.unpack_from("<IIII", data, 8)
    if bit not in PART_BITS or not 0 < ident <= MAX_ITEM_ID or not 0 < costumes < 8:
        raise ValueError(f"item record {ident} has part bit {bit} and costume mask {costumes}")
    models = list(struct.unpack_from(f"<{(len(data) - 0x98) // 4}i", data, 0x98))
    if len(models) != bin(costumes).count("1"):
        raise ValueError(f"item {ident} lists {len(models)} models for costume mask {costumes}")
    return {"id": ident, "part": PART_BITS[bit], "costumes": [c for c in range(3) if costumes >> c & 1], "price": price,
            "models": models}


def colours(data):
    """The colour lists: u8 part count and three more bytes, then per part u32 lead, u32 count, prices, 32-byte names."""
    count, at, parts = data[0], 4, []
    for _ in range(count):
        lead, n = struct.unpack_from("<II", data, at)
        at += 8
        prices = list(struct.unpack_from(f"<{n}I", data, at))
        at += 4 * n
        names = [data[at + 32 * i:at + 32 * (i + 1)].split(b"\0")[0].decode("ascii") for i in range(n)]
        at += 32 * n
        parts.append({"lead": lead, "names": names, "prices": prices})
    if at != len(data) or count != COLOUR_PARTS:
        raise ValueError("colour lists do not fill their file")
    return parts


def item_names(data):
    """Item names: three words, u32 count, then per entry u32 id and 28 name bytes, ended by id -1."""
    count, = struct.unpack_from("<I", data, 12)
    names = {}
    for i in range(count):
        ident, = struct.unpack_from("<i", data, 16 + 32 * i)
        if ident < 0:
            break
        names[ident] = data[20 + 32 * i:48 + 32 * i].split(b"\0")[0].decode("latin-1")
    return names


def name_overrides(elf, fighter):
    """The English build's renamed items for a fighter: pairs of (id, name pointer) ended by a negative id."""
    table = elf.word(NAME_OVERRIDES + 4 * fighter)
    out = {}
    while table:
        ident, pointer = struct.unpack(">iI", elf.read(table, 8))
        if ident < 0:
            break
        out[ident] = elf.text(pointer)
        table += 8
    return out


def dressable_costume(fighter, costume):
    return costume < 2 or (costume == 2 and fighter in THIRD_COSTUME_FIGHTERS)


def decode(elf_data, archive):
    if hashlib.sha256(elf_data).hexdigest() != ELF_SHA256:
        raise ValueError("this is not the pinned NPUB30009 EBOOT")
    elf = Elf(elf_data)
    parts = pack(archive_file(elf_data, archive, CATALOGUE_RECORD))
    if len(parts) != ROWS:
        raise ValueError(f"the catalogue holds {len(parts)} rows, not {ROWS}")
    rows = []
    for row, body in enumerate(parts):
        fighter, costume = struct.unpack(">HH", elf.read(ROW_TABLE + row * ROW_SIZE, 4))
        content = files(body)
        code = pac(body)[0][0].split("_")[0]
        entries, colour_lists, names = [], None, {}
        for path, data in content.items():
            leaf = path.split("/")[-1]
            if leaf.endswith(".iif"):
                record = item(data)
                folder = path.split("/")[-2].split("_")[-1].split(".")[0]
                if FOLDERS.get(folder) != record["part"] or costume not in record["costumes"]:
                    raise ValueError(f"row {row}: item {record['id']} sits in {folder} for costumes {record['costumes']}")
                entries.append(record)
            # Rows the shop never dresses carry these files empty.
            elif leaf.endswith("_col.sel") and data:
                colour_lists = colours(data)
            elif leaf.endswith("_itemname.dat") and data:
                names = item_names(data)
        if len({e["id"] for e in entries}) != len(entries):
            raise ValueError(f"row {row} lists an item twice")
        renamed = name_overrides(elf, fighter) if fighter < FIGHTERS else {}
        for e in entries:
            e["name"] = renamed.get(e["id"], names.get(e["id"]))
        entries.sort(key=lambda e: (e["part"], e["id"]))
        rows.append({
            "row": row,
            "fighter": fighter,
            "fighterName": FIGHTER_NAMES[fighter] if fighter < FIGHTERS else None,
            "costume": costume,
            "code": code,
            "dressable": row < ROWS - 1 and dressable_costume(fighter, costume) and colour_lists is not None,
            "parts": [[{k: e[k] for k in ("id", "name", "costumes", "price", "models")} for e in entries if e["part"] == p]
                      for p in range(len(PARTS))],
            "colours": colour_lists or [],
        })
    return rows


def header(rows):
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "// The custom-look catalogue: for each row of the costume row table (0x4403c0), the fighter and costume, whether the game",
        "// dresses it, the items each part can wear (a 128-bit set of item ids per part: head, face, upper, lower) and the",
        "// number of colours in each of the three colour lists. Generated by tools/pbd_custom_catalogue.py from the game's own",
        "// item catalogue; do not edit by hand.",
        "namespace pbd::custom_catalogue",
        "{",
        "\tstruct row",
        "\t{",
        "\t\tstd::uint16_t fighter;",
        "\t\tstd::uint8_t costume;",
        "\t\tbool dressable;",
        "\t\tstd::array<std::array<std::uint64_t, 2>, 4> items;",
        "\t\tstd::array<std::uint8_t, 3> colours;",
        "\t};",
        "",
        f"\tinline constexpr std::array<row, {len(rows)}> rows{{{{",
    ]
    for r in rows:
        sets = []
        for part in r["parts"]:
            words = [0, 0]
            for e in part:
                words[e["id"] >> 6] |= 1 << (e["id"] & 63)
            sets.append("{0x%xull, 0x%xull}" % tuple(words))
        counts = [len(c["names"]) for c in r["colours"]] or [0, 0, 0]
        lines.append("\t\t{%d, %d, %s, {{%s}}, {%s}}," % (r["fighter"], r["costume"], "true" if r["dressable"] else "false",
                                                          ", ".join(sets), ", ".join(map(str, counts))))
    lines += ["\t}};", "} // namespace pbd::custom_catalogue", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf")
    parser.add_argument("archive")
    parser.add_argument("--json", required=True)
    parser.add_argument("--header", default=HEADER)
    args = parser.parse_args()
    with open(args.elf, "rb") as source:
        elf_data = source.read()
    with open(args.archive, "rb") as archive:
        rows = decode(elf_data, archive)
    document = {"schema": "pbd-custom-catalogue-1", "elfSha256": ELF_SHA256, "rows": rows}
    with open(args.json, "w", encoding="utf-8", newline="\n") as out:
        json.dump(document, out, indent=1, ensure_ascii=False)
        out.write("\n")
    with open(args.header, "w", encoding="utf-8", newline="\n") as out:
        out.write(header(rows))
    dressed = sum(r["dressable"] for r in rows)
    items = sum(len(p) for r in rows for p in r["parts"])
    print(f"{len(rows)} rows, {dressed} dressable, {items} item entries -> {args.json}, {args.header}")


if __name__ == "__main__":
    main()
