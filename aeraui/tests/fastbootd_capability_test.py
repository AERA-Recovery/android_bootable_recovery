import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    begin = text.index(signature)
    body = text.index('{', begin)
    depth, end = 1, body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('g++'),
                     'Requires a native Linux C++ compiler')
class FastbootdCapabilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.source = ROOT

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_disabled_and_enabled_builds_select_correct_destinations(self):
        startup = (self.source / 'twrp.cpp').read_text()
        support = function(startup, 'bool RecoverySupportsFastbootd()')
        scene = (self.source / 'aeraui/scenes/reboot_scene.cpp').read_text()
        menu = scene[scene.index('  struct Destination'):scene.index('  auto *list = Scroll')]
        driver = '''#include <cassert>
#include <vector>
static bool dynamic_partitions, daemon_available;
static unsigned probes;
constexpr int X_OK = 1;
int access(const char*, int) { ++probes; return daemon_available ? 0 : -1; }
namespace android { namespace base {
bool GetBoolProperty(const char*, bool) { ++probes; return dynamic_partitions; }
}}
enum class Action { kRebootSystem, kRebootRecovery, kRebootBootloader, kRebootFastbootd, kPowerOff };
constexpr auto LV_SYMBOL_HOME="home", LV_SYMBOL_REFRESH="refresh", LV_SYMBOL_SETTINGS="settings", LV_SYMBOL_USB="usb", LV_SYMBOL_POWER="power";
''' + support + '\nstd::vector<Action> Menu() {\n' + menu + '''
  std::vector<Action> result;
  for (const auto& d : destinations) result.push_back(d.action);
  return result;
}
int main() {
  for (bool dynamic : {false, true}) for (bool daemon : {false, true}) {
    dynamic_partitions=dynamic; daemon_available=daemon; probes=0;
#ifdef TW_INCLUDE_FASTBOOTD
    bool available=dynamic && daemon;
#else
    bool available=false;
#endif
    assert(RecoverySupportsFastbootd() == available);
    auto menu=Menu();
    assert(menu.size() == (available ? 5 : 4));
    assert(menu[0] == Action::kRebootSystem);
    assert(menu[1] == Action::kRebootRecovery);
    assert(menu[2] == Action::kRebootBootloader);
    assert(menu.back() == Action::kPowerOff);
    if (available) assert(menu[3] == Action::kRebootFastbootd);
#ifndef TW_INCLUDE_FASTBOOTD
    assert(probes == 0);
#endif
  }
}
'''
        path = self.root / 'capability.cpp'
        path.write_text(driver)
        for enabled in (False, True):
            binary = self.root / ('enabled' if enabled else 'disabled')
            flags = ['-DTW_INCLUDE_FASTBOOTD'] if enabled else []
            subprocess.run(['g++', '-std=c++17', *flags, str(path), '-o', str(binary)],
                           check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_entry_guards_precede_usb_and_partition_changes(self):
        backend = (self.source / 'aeraui/platform/aera_backend.cpp').read_text()
        self.assertIn('bool RecoveryEnterFastbootd() {\n  if (!RecoverySupportsFastbootd())', backend)
        startup = (self.source / 'twrp.cpp').read_text()
        self.assertIn('static void process_fastbootd_mode() {\n        if (!aeraui::RecoverySupportsFastbootd())', startup)
        self.assertIn('if (fastboot_mode && !aeraui::RecoverySupportsFastbootd())', startup)
        args = (self.source / 'startupArgs.cpp').read_text()
        self.assertLess(args.index('if (!aeraui::RecoverySupportsFastbootd())'), args.index('fastboot_mode = true;'))
        self.assertLess(args.index('return true;'), args.index('SetProperty("sys.usb.config"'))
        host = (self.source / 'aeraui/platform/aera_ui_host.cpp').read_text()
        self.assertEqual(host.count('const bool fastboot = aeraui::RecoverySupportsFastbootd() &&'), 2)
        power = (self.source / 'twrp-functions.cpp').read_text()
        self.assertLess(power.index('command == rb_fastboot && !aeraui::RecoverySupportsFastbootd()'),
                        power.index('DataManager::Flush();'))


if __name__ == '__main__':
    unittest.main()
