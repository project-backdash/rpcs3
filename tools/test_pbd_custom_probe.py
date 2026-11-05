import struct
import unittest

from pbd_custom_probe import ACTORS, CARDS, Dump, observe


def packed(pages):
    return b"DRMD001\0" + struct.pack("<I", len(pages)) + b"".join(
        struct.pack("<I", address) + data for address, data in pages)


class CustomProbeTest(unittest.TestCase):
    def test_cross_page_reads(self):
        dump = Dump(packed([(0x1000, b"a" * 4096), (0x2000, b"b" * 4096)]))
        self.assertEqual(dump.read(0x1FFE, 4), b"aabb")
        with self.assertRaises(ValueError):
            dump.read(0x2FFE, 4)

    def test_bad_headers_and_truncation(self):
        valid = packed([(0x1000, bytes(4096))])
        for data in (b"", valid[:10], b"BAD" + valid[3:], valid[:-1], valid + b"x", packed([])):
            with self.assertRaises(ValueError):
                Dump(data)

    def test_duplicate_and_unaligned_pages(self):
        for pages in ([(0x1000, bytes(4096))] * 2, [(0x1001, bytes(4096))]):
            with self.assertRaises(ValueError):
                Dump(packed(pages))

    def test_required_memory_cannot_be_fabricated(self):
        with self.assertRaises(ValueError):
            observe(Dump(packed([(0x1000, bytes(4096))])))

    def test_card_and_pointer_observations(self):
        pages = {address: bytearray(4096) for address in (0x1008E000, 0x1008F000, 0x10090000, 0x10091000, 0x106C9000)}

        def write(address, value):
            pages[address & ~0xFFF][address & 0xFFF:(address & 0xFFF) + len(value)] = value

        write(0x1008EBAC, struct.pack(">I", 10))
        write(0x1008F070, struct.pack(">I", 17))
        write(0x1008F0B0, struct.pack(">I", 1))
        write(ACTORS[0] + 0x40, struct.pack(">HH", 31, 77))
        write(CARDS[0] + 0x40, struct.pack(">4H", 61, 67, 66, 50))
        write(CARDS[0] + 0x54, struct.pack(">3b", -1, 3, 11))
        write(ACTORS[0] + 0x8A4, struct.pack(">I", 0x106C9000))
        write(ACTORS[0] + 0x100, struct.pack(">I", 0x106C9004))
        out = observe(Dump(packed(list(pages.items()))))
        self.assertEqual((out["mode"], out["scene"], out["humans"]), (10, 17, 1))
        self.assertEqual(out["sides"][0]["blocks"][0], {"items": [61, 67, 66, 50], "colours": [-1, 3, 11]})
        self.assertEqual(out["sides"][0]["unknown_pointer_candidates"], [{"offset": "0x100", "value": "0x106c9004", "known": False}])


if __name__ == "__main__":
    unittest.main()
