import unittest

import pbd_check_core as checker

TRACKED = {
    "rpcs3/Emu/PBD/core/peer/pbd_peer_message.h",
    "rpcs3/Emu/PBD/core/peer/pbd_peer_auth.h",
    "rpcs3/Emu/PBD/core/util/pbd_json.h",
    "rpcs3/Emu/PBD/emulator/rollback/pbd_rollback_session.h",
    "rpcs3/Emu/Memory/vm.h",
    "rpcs3/Emu/System.h",
    "rpcs3/util/types.hpp",
    "rpcs3/util/logs.hpp",
    "rpcs3/stdafx.h",
}
PATH = "rpcs3/Emu/PBD/core/peer/pbd_peer_auth.h"


class CoreIncludes(unittest.TestCase):
    def inspect(self, text):
        return checker.findings(TRACKED, PATH, text)

    def test_core_the_standard_library_and_libraries_are_allowed(self):
        self.assertEqual(self.inspect('#include "pbd_peer_message.h"\n'
                                      '#include "Emu/PBD/core/util/pbd_json.h"\n'
                                      '#include "../util/pbd_json.h"\n'
                                      '#include "util/types.hpp"\n'
                                      '#include "../../../../../3rdparty/zstd/zstd/lib/zstd.h"\n'
                                      '#include "png.h"\n'
                                      '#include <vector>\n'
                                      '#include <winsock2.h>\n'
                                      '#include <sys/socket.h>\n'), [])

    def test_emulator_headers_are_findings_with_their_lines(self):
        self.assertEqual(self.inspect('#include <vector>\n'
                                      '#include "Emu/Memory/vm.h"\n'
                                      '\t#  include "stdafx.h"\n'
                                      '#include <Emu/System.h>\n'
                                      '#include "util/logs.hpp"\n'),
                         [(2, "Emu/Memory/vm.h"), (3, "stdafx.h"), (4, "Emu/System.h"), (5, "util/logs.hpp")])

    def test_the_emulator_half_is_outside_core_however_it_is_written(self):
        self.assertEqual(self.inspect('#include "Emu/PBD/emulator/rollback/pbd_rollback_session.h"\n'
                                      '#include "../../emulator/rollback/pbd_rollback_session.h"\n'),
                         [(1, "Emu/PBD/emulator/rollback/pbd_rollback_session.h"),
                          (2, "../../emulator/rollback/pbd_rollback_session.h")])

    def test_an_include_in_a_comment_is_not_an_include(self):
        self.assertEqual(self.inspect('// #include "Emu/System.h"\nconst char* text = "#include \\"Emu/System.h\\"";\n'), [])


class EmulatorNames(unittest.TestCase):
    def test_a_name_of_the_emulator_is_a_finding_at_its_first_use(self):
        self.assertEqual(checker.uses('#include <vector>\n'
                                      'struct pbd::rollback_session::steps : boundary_call\n'
                                      '{\n'
                                      '\tbool run() { return vm::check_addr(0) && Emu.IsRunning() && vm::check_addr(4); }\n'
                                      '};\n'),
                         [(2, "a member of the rollback session object"), (4, "guest memory (vm::)"),
                          (4, "the system object (Emu.)")])
        self.assertEqual(checker.uses('class ppu_thread;\nvoid f() { pbd_session_log.error("x"); fs::file out; }\n'),
                         [(1, "an emulator thread"), (2, "RPCS3's file utilities (fs::)"), (2, "an RPCS3 log channel")])

    def test_a_name_in_a_comment_or_a_string_is_not_a_use(self):
        self.assertEqual(checker.uses('// vm::read32 would need the emulator\n'
                                      '/* Emu.Pause()\n   g_cfg */\n'
                                      'const char* text = "rsx::thread \\" vm::g_base_addr";\n'
                                      'const char* raw = R"x(fs::file)x";\n'), [])

    def test_code_after_a_digit_separator_is_still_read(self):
        self.assertEqual(checker.uses("constexpr int rate = 48'000; auto base = vm::g_base_addr; char c = 'x';\n"),
                         [(1, "guest memory (vm::)")])

    def test_a_namespace_of_the_files_own_is_not_the_emulators(self):
        self.assertEqual(checker.uses('namespace pad { constexpr int circle = 1; }\n'
                                      'int f(int down) { return down & pad::circle; }\n'
                                      'namespace fs = std::filesystem;\n'
                                      'fs::path g();\n'
                                      'std::filesystem::path h();\n'), [])
        self.assertEqual(checker.uses('void f() { std::lock_guard lock(pad::g_pad_mutex); }\n'),
                         [(1, "the pad thread (pad::)")])

    def test_a_blanked_file_keeps_its_lines(self):
        text = 'int a; // one\n/* two\nthree */ int b;\n'
        self.assertEqual(checker.code_only(text).count("\n"), text.count("\n"))
        self.assertEqual(checker.code_only(text).splitlines()[2].strip(), "int b;")


class CompiledAlone(unittest.TestCase):
    def test_a_header_is_included_by_its_path_from_the_source_root(self):
        self.assertEqual(checker.unit("rpcs3/Emu/PBD/core/util/pbd_json.h"), '#include "Emu/PBD/core/util/pbd_json.h"\n')


if __name__ == "__main__":
    unittest.main()
