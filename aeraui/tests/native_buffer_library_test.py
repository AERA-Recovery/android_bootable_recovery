"""Exercise provider selection with real shared libraries and dlopen/dlsym."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SYMBOLS = (
    "AHardwareBuffer_allocate",
    "AHardwareBuffer_release",
    "AHardwareBuffer_describe",
    "AHardwareBuffer_getNativeHandle",
    "_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEPK15AHardwareBuffer",
)


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("g++"),
                     "Requires a native Linux C++ compiler")
class NativeBufferLibraryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.driver = cls.root / "loader"
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "native_buffer_library_driver.cpp"), "-ldl",
            "-o", str(cls.driver),
        ], check=True, capture_output=True, text=True)
        cls.system = cls.build_library("system", 1)
        cls.vendor = cls.build_library("vendor", 2)
        cls.incomplete = [cls.build_library(f"missing-{i}", 3, symbol)
                          for i, symbol in enumerate(SYMBOLS)]
        cls.broken = cls.root / "broken.so"
        cls.broken.write_text("not a shared library", encoding="utf-8")
        # RTLD_NOW must reject a provider with an unresolved dependency.
        source = cls.root / "unresolved.cpp"
        source.write_text('extern "C" int missing_dependency();\n'
                          'extern "C" int call_dependency() { return missing_dependency(); }\n',
                          encoding="utf-8")
        cls.unresolved = cls.root / "unresolved.so"
        subprocess.run(["g++", "-shared", "-fPIC", str(source),
                        "-o", str(cls.unresolved)], check=True,
                       capture_output=True, text=True)

    @classmethod
    def build_library(cls, name, identity, missing=None):
        source = cls.root / f"{name}.cpp"
        source.write_text("\n".join(
            f'extern "C" int {symbol}() {{ return {identity}; }}'
            for symbol in SYMBOLS if symbol != missing), encoding="utf-8")
        library = cls.root / f"{name}.so"
        subprocess.run(["g++", "-shared", "-fPIC", str(source),
                        "-o", str(library)], check=True,
                       capture_output=True, text=True)
        return library

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check_selection(self, first, second, expected):
        subprocess.run([str(self.driver), str(first), str(second), str(expected)],
                       check=True, timeout=5, capture_output=True, text=True)

    def test_system_wins_when_both_are_complete(self):
        self.check_selection(self.system, self.vendor, 1)

    def test_missing_system_uses_vendor(self):
        self.check_selection(self.root / "absent.so", self.vendor, 2)

    def test_unloadable_system_uses_vendor(self):
        self.check_selection(self.broken, self.vendor, 2)

    def test_unresolved_dependency_uses_vendor(self):
        self.check_selection(self.unresolved, self.vendor, 2)

    def test_every_required_symbol_is_checked_and_rejected_handle_is_closed(self):
        for missing in self.incomplete:
            with self.subTest(provider=missing.name):
                self.check_selection(missing, self.vendor, 2)

    def test_incomplete_vendor_returns_no_provider(self):
        for missing in self.incomplete:
            with self.subTest(provider=missing.name):
                self.check_selection(self.root / "absent.so", missing, 0)

    def test_neither_provider_available(self):
        self.check_selection(self.root / "absent.so", self.broken, 0)

    def test_no_provider_can_supply_anothers_missing_symbols(self):
        self.check_selection(self.incomplete[0], self.incomplete[1], 0)


if __name__ == "__main__":
    unittest.main()
