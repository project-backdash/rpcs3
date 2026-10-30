import subprocess
import unittest
from unittest.mock import patch

import pbd_check_format as checker


class FormatterFailure(unittest.TestCase):
    def inspect(self, code, stderr):
        with patch.object(checker, "run", return_value=subprocess.CompletedProcess([], code, "", stderr)):
            return checker.findings("clang-format", ".", ["test.cpp"])

    def test_formatting_violation_is_a_finding(self):
        self.assertEqual(self.inspect(1, "test.cpp:3:2: error: code should be clang-formatted [-Wclang-format-violations]"),
                         [("test.cpp", 3, 2)])

    def test_execution_error_is_not_clean(self):
        for stderr in ("clang-format: error: unknown argument --invalid", "invalid configuration", ""):
            with self.subTest(stderr=stderr), self.assertRaises(RuntimeError):
                self.inspect(1, stderr)

    def test_error_alongside_formatting_is_not_just_a_finding(self):
        with self.assertRaises(RuntimeError):
            self.inspect(1, "test.cpp:3:2: warning: code should be clang-formatted\nclang-format: error: bad configuration")


if __name__ == "__main__":
    unittest.main()
