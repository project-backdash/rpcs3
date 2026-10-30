import json
from pathlib import Path
import unittest

from pbd_hle_order import convert


class HleOrderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.include = (Path(__file__).resolve().parents[1] / "rpcs3/Emu/Cell/pbd_hle_pinned_order.inc").read_text()
        cls.keys = [json.loads(line[:-1]) for line in cls.include.splitlines() if line.startswith('"')]
        cls.dump = "".join(f"{i}\t{key}\n" for i, key in enumerate(cls.keys)).encode("ascii")

    def test_reproduces_existing_table_and_accepts_windows_lines(self):
        self.assertEqual(convert(self.dump), self.include)
        self.assertEqual(convert(self.dump.replace(b"\n", b"\r\n")), self.include)

    def test_rejects_changed_identity_before_emitting_a_table(self):
        mutations = [
            self.dump.replace(b"0\tbuiltin:0", b"1\tbuiltin:0", 1),
            self.dump.replace(b"1\tbuiltin:1", b"1\tbuiltin:0", 1),
            self.dump.replace(b"0\tbuiltin:0", b"0\tbuiltin:x", 1),
            self.dump.rsplit(b"\n", 2)[0] + b"\n",
            self.dump + b"3875\tnew:slot\n",
        ]
        for raw in mutations:
            with self.subTest(raw=raw[:30]), self.assertRaises(ValueError):
                convert(raw)


if __name__ == "__main__":
    unittest.main()
