import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('g++'),
                     'Requires a native Linux C++ compiler')
class RebootHandoffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        source = cls.root / 'source'
        shutil.copytree(ROOT / 'fixtures/early-ui', source)
        driver = ROOT / 'reboot_handoff_driver.cpp'

        def compile(name):
            binary = cls.root / name
            subprocess.run(['g++', '-std=c++17', '-pthread', '-I', str(source),
                            str(driver), '-o', str(binary)], check=True,
                           capture_output=True, text=True)
            return binary

        cls.original = compile('original')
        actual = (ROOT.parent / 'core/runner.cpp').read_text()
        scaffold = (source / 'aeraui/core/runner.cpp').read_text()
        prefix = scaffold[:scaffold.index('std::atomic<bool> gBackendReady')]
        globals_start = actual.index('std::atomic<bool> gBackendReady')
        globals_end = actual.index('DisplayMetrics gDisplayMetrics{};') + len('DisplayMetrics gDisplayMetrics{};')
        terminal_start = actual.index('bool IsTerminalResult(')
        terminal_end = actual.index('RunResult ToRunResult(', terminal_start)
        loop = scaffold[scaffold.index('RunResult RunLoop(bool fastboot,'):scaffold.index('void StartAeraUiEarly(')]
        functions = actual[actual.index('static RunResult RunInlineUi('):]
        (source / 'aeraui/core/runner.cpp').write_text(
            prefix + actual[globals_start:globals_end] + '\n' +
            actual[terminal_start:terminal_end] + '\n}  // namespace\n' + loop + functions)
        shutil.copyfile(ROOT.parent / 'include/aeraui/runner.hpp',
                        source / 'aeraui/include/aeraui/runner.hpp')
        cls.fixed = compile('fixed')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, binary, result, scenario='normal'):
        return subprocess.run([str(binary), str(result), scenario], timeout=2).returncode

    def test_original_waits_for_vendor_tls_cleanup_before_power_request(self):
        for result in range(2, 7):
            with self.subTest(result=result):
                self.assertEqual(self.run_case(self.original, result), 20)

    def test_all_terminal_destinations_handoff_without_exiting_graphics_thread(self):
        for result in range(2, 7):
            for scenario in ('normal', 'precompleted', 'decrypt'):
                with self.subTest(result=result, scenario=scenario):
                    self.assertEqual(self.run_case(self.fixed, result, scenario), 0)

    def test_unexpected_exit_still_joins_and_cleans_up(self):
        self.assertEqual(self.run_case(self.fixed, 1), 0)

    def test_terminal_action_survives_later_startup_page_calls(self):
        for result in range(2, 7):
            with self.subTest(result=result):
                self.assertEqual(self.run_case(self.fixed, result, 'repeat'), 0)

    def test_inline_startup_is_preserved(self):
        self.assertEqual(self.run_case(self.fixed, 1, 'inline'), 0)


if __name__ == '__main__':
    unittest.main()
