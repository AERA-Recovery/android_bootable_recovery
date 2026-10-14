"""Run recovery's actual mount-lifetime helpers against a fake partition backend."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT.parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("g++"),
                     "Requires a native Linux C++ compiler")
class VendorUiMountTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.manager = (SOURCE / "partitionmanager.cpp").read_text(encoding="utf-8")
        cls.main = (SOURCE / "twrp.cpp").read_text(encoding="utf-8")
        cls.backend = (ROOT.parent / "platform/aera_backend.cpp").read_text(encoding="utf-8")
        cls.modules = (SOURCE / "kernel_module_loader.cpp").read_text(encoding="utf-8")
        setup = function(cls.manager, "void TWPartitionManager::Setup_Fstab_Partitions(")
        start = setup.index("#ifndef USE_VENDOR_LIBS")
        end = setup.index("#endif", start) + len("#endif")
        template = (ROOT / "vendor_ui_mount_driver.cpp").read_text(encoding="utf-8")
        source = template.replace("// ACTUAL_PREPARE", function(
            cls.manager, "bool TWPartitionManager::Prepare_Vendor_For_Native_UI("))
        source = source.replace("// ACTUAL_MAIN_CLEANUP", function(
            cls.manager, "void TWPartitionManager::UnMount_Main_Partitions("))
        source = source.replace("// ACTUAL_APEX_CLEANUP", setup[start:end])
        source = source.replace("// ACTUAL_PROPERTY_PROBE", function(
            cls.manager, "static void Update_Encryption_Props_Before_Format("))
        source = source.replace("// ACTUAL_MODULE_BIND", function(
            cls.modules, "int KernelModuleLoader::Try_And_Load_Modules("))
        load_modules = function(cls.modules, "bool KernelModuleLoader::Load_Vendor_Modules(")
        begin = load_modules.index("\nexit:\n") + len("\nexit:\n")
        end = load_modules.index("\tif (ven_dlkm)", begin)
        source = source.replace("// ACTUAL_MODULE_CLEANUP", load_modules[begin:end])
        begin = load_modules.index("\tconst bool vendor_was_mounted =")
        end = load_modules.index(";", begin) + 1
        source = source.replace("// ACTUAL_MODULE_MOUNT_STATE", load_modules[begin:end])
        (cls.root / "driver.cpp").write_text(source, encoding="utf-8")
        cls.binaries = []
        for use_vendor in (False, True):
            binary = cls.root / ("vendor-libs" if use_vendor else "normal")
            command = ["g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror"]
            if use_vendor:
                command.append("-DUSE_VENDOR_LIBS=1")
            result = subprocess.run(command + [str(cls.root / "driver.cpp"), "-o", str(binary)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)
            cls.binaries.append(binary)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, name):
        for binary in self.binaries:
            with self.subTest(build=binary.name):
                subprocess.run([str(binary), name], check=True, timeout=5,
                               capture_output=True, text=True)

    def test_delayed_renderer_keeps_vendor_through_both_cleanup_paths(self):
        self.run_case("delayed")

    def test_normal_cleanup_still_releases_unowned_vendor(self):
        self.run_case("unowned")

    def test_missing_vendor_and_failed_mount_do_not_claim_ownership(self):
        self.run_case("unavailable")

    def test_cold_fastboot_does_not_acquire_a_vendor_mount(self):
        self.run_case("fastboot")

    def test_explicit_unmount_is_not_blocked(self):
        self.run_case("explicit")

    def test_property_probe_preserves_preexisting_mount_only(self):
        self.run_case("probe")

    def test_module_loader_preserves_preexisting_mount_only(self):
        self.run_case("module-cleanup")

    def test_module_bind_cleanup_does_not_resolve_to_parent_vendor(self):
        self.run_case("module-bind")

    def test_mount_ownership_is_acquired_before_starting_early_ui(self):
        prepare = self.main.index("PartitionManager.Prepare_Vendor_For_Native_UI(startup.Get_Fastboot_Mode());")
        self.assertLess(prepare, self.main.index("gui_init();", prepare))

    def test_return_from_fastboot_restores_ownership_before_partition_setup(self):
        recovery = function(self.backend, "bool RecoveryLeaveFastbootd(")
        self.assertLess(recovery.index("Prepare_All_Super_Volumes"),
                        recovery.index("Prepare_Vendor_For_Native_UI(false)"))
        self.assertLess(recovery.index("Prepare_Vendor_For_Native_UI(false)"),
                        recovery.index("Setup_Fstab_Partitions(true)"))

    def test_super_release_clears_ownership_only_after_success(self):
        unmap = function(self.manager, "bool TWPartitionManager::Unmap_Super_Devices(")
        release = unmap.index("native_ui_vendor_ = false;")
        self.assertGreater(release, unmap.rindex("return false;"))
        self.assertLess(release, unmap.rindex("return true;"))
        self.assertIn("QuiesceDynamicPartitionUsers()", unmap)
        self.assertIn("(*iter)->UnMount(false);", unmap)


if __name__ == "__main__":
    unittest.main()
