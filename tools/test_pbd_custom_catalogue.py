import io
import struct
import unittest
import zlib

import pbd_custom_catalogue as catalogue


def entry(name, offset, size):
    raw = name.encode("ascii") + b"\0"
    raw += b"\0" * (-len(raw) % 4)
    body = struct.pack("<I", len(raw) + 8) + raw + struct.pack("<II", offset, size)
    return catalogue.unxor(body)


def make_pac(named):
    heads = [entry(name, 0, 0) for name, _ in named]
    header = 16 + sum(len(h) for h in heads)
    out, offset, table = bytearray(), header, bytearray()
    for name, data in named:
        table += entry(name, offset, len(data))
        out += data
        offset += len(data)
    return b"PAC\x01" + struct.pack("<III", 15, header, len(named)) + bytes(table) + bytes(out)


def make_item(ident, bit, costumes, price=0):
    models = [ident + c for c in range(bin(costumes).count("1"))]
    record = b"PANTERA\0" + struct.pack("<IIII", ident, bit, costumes, price)
    record += b"\0" * (0x98 - len(record))
    return record + struct.pack(f"<{len(models)}i", *models)


def make_colours(lead=(0, 0, 1), names=("silver", "black")):
    out = bytearray(b"\x03\x01\x00\x03")
    for value in lead:
        out += struct.pack("<II", value, len(names)) + struct.pack(f"<{len(names)}I", *([10000] * len(names)))
        out += b"".join(n.encode("ascii").ljust(32, b"\0") for n in names)
    return bytes(out)


def make_names(pairs):
    out = bytearray(struct.pack("<IIII", 0, 0, 0, len(pairs) + 1))
    for ident, name in list(pairs) + [(-1, "------")]:
        out += struct.pack("<i", ident) + name.encode("latin-1").ljust(28, b"\0")
    return bytes(out)


class PacTests(unittest.TestCase):
    def test_entry_headers_are_xored_and_contents_are_plain(self):
        inner = make_pac([("ab_0061_h.iif", make_item(61, 1, 3, 50000))])
        outer = make_pac([("ab1_parts.pac", make_pac([("ab1_heads.pac", inner)])), ("ab1_col.sel", make_colours()),
                          ("ab_itemname.dat", make_names([(61, "Dizzy")]))])
        self.assertNotIn(b"ab1_parts", outer[:64])
        found = catalogue.files(outer)
        self.assertEqual(sorted(found), ["ab1_col.sel", "ab1_parts.pac/ab1_heads.pac/ab_0061_h.iif", "ab_itemname.dat"])
        self.assertEqual(found["ab1_col.sel"], make_colours())

    def test_refuses_entries_outside_the_file(self):
        good = make_pac([("a.dat", b"1234")])
        self.assertRaises(ValueError, catalogue.pac, b"PAK\x01" + good[4:])
        self.assertRaises(ValueError, catalogue.pac, good[:-1])


class RecordTests(unittest.TestCase):
    def test_item_records_name_their_part_costumes_and_models(self):
        record = catalogue.item(make_item(68, 1, 3, 400000))
        self.assertEqual(record, {"id": 68, "part": 0, "costumes": [0, 1], "price": 400000, "models": [68, 69]})
        self.assertEqual(catalogue.item(make_item(16, 8, 7))["costumes"], [0, 1, 2])
        self.assertEqual(catalogue.item(make_item(77, 4, 1))["part"], 2)

    def test_refuses_items_the_shop_could_not_hold(self):
        self.assertRaises(ValueError, catalogue.item, make_item(61, 3, 1))
        self.assertRaises(ValueError, catalogue.item, make_item(0, 1, 1))
        self.assertRaises(ValueError, catalogue.item, make_item(128, 1, 1))
        self.assertRaises(ValueError, catalogue.item, make_item(61, 1, 3)[:-4])

    def test_colour_lists_and_names(self):
        parts = catalogue.colours(make_colours((0, 2, 1), ("silver", "black", "red")))
        self.assertEqual([p["lead"] for p in parts], [0, 2, 1])
        self.assertEqual(parts[1]["names"], ["silver", "black", "red"])
        self.assertRaises(ValueError, catalogue.colours, make_colours() + b"\0")
        self.assertEqual(catalogue.item_names(make_names([(61, "Dizzy"), (62, "Pinned-up Hair")])),
                         {61: "Dizzy", 62: "Pinned-up Hair"})

    def test_only_costumes_the_loader_dresses(self):
        self.assertTrue(catalogue.dressable_costume(0, 0) and catalogue.dressable_costume(0, 1))
        self.assertFalse(catalogue.dressable_costume(0, 2))
        self.assertTrue(catalogue.dressable_costume(7, 2) and catalogue.dressable_costume(24, 2))
        self.assertFalse(catalogue.dressable_costume(24, 3))


class ArchiveTests(unittest.TestCase):
    def test_xor_stream_restarts_each_sector_and_undoes_itself(self):
        data = bytes(range(256)) * 20
        coded = catalogue.xor_stream(data, 0x2BA23909)
        self.assertEqual(catalogue.xor_stream(coded, 0x2BA23909), data)
        self.assertEqual(coded[:4], bytes(a ^ b for a, b in zip(data[:4], (0x2BA23909).to_bytes(4, "little"))))
        self.assertEqual(bytes(a ^ b for a, b in zip(coded[2048:2052], data[2048:2052])), (0x2BA23909).to_bytes(4, "little"))

    def test_reads_a_packed_file_through_its_index_record(self):
        content = struct.pack("<I", 2) + struct.pack("<IIII", 20, 3, 23, 2) + b"abcde"
        packed = catalogue.xor_stream(zlib.compress(content), 0x1234)
        archive = io.BytesIO(b"\0" * 4096 + packed)
        record = struct.pack(">7I", 2, len(packed), len(content), 0, 0, 0x1234, 0)
        self.assertEqual(catalogue.pack(catalogue.archive_file(record, archive, 0)), [b"abc", b"de"])
        wrong = struct.pack(">7I", 2, len(packed), len(content) + 1, 0, 0, 0x1234, 0)
        self.assertRaises(ValueError, catalogue.archive_file, wrong, archive, 0)

    def test_refuses_another_eboot(self):
        self.assertRaises(ValueError, catalogue.decode, b"\x7fELF\x02\x02" + b"\0" * 64, io.BytesIO())


class HeaderTests(unittest.TestCase):
    def test_item_sets_are_128_bit_masks_per_part(self):
        row = {"fighter": 31, "costume": 0, "dressable": True,
               "parts": [[{"id": 61}, {"id": 1}], [], [{"id": 77}], [{"id": 64}, {"id": 127}]],
               "colours": [{"names": ["a"] * 12}] * 3}
        text = catalogue.header([row])
        self.assertIn("{31, 0, true, {{{0x2000000000000002ull, 0x0ull}, {0x0ull, 0x0ull}, {0x0ull, 0x2000ull}, "
                      "{0x0ull, 0x8000000000000001ull}}}, {12, 12, 12}},", text)
        self.assertIn("std::array<row, 1> rows", text)


if __name__ == "__main__":
    unittest.main()
