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

    def test_capability_and_modem_guards_both_block_transition(self):
        # Compile the real guard prefixes from both conflicted entry points.
        # Successful transitions are represented by a counter; no USB, modem,
        # storage or reboot operations run in this host test.
        startup = (self.source / 'twrp.cpp').read_text()
        support = function(startup, 'bool RecoverySupportsFastbootd()')
        backend = (self.source / 'aeraui/platform/aera_backend.cpp').read_text()
        live = function(backend, 'bool RecoveryEnterFastbootd()')
        live = live[:live.index('  // Remember whether device-specific')]
        live += '  ++transitions; return true;\n}\n'
        power = (self.source / 'twrp-functions.cpp').read_text()
        reboot = function(power, 'int TWFunc::tw_reboot(RebootCommand command)')
        reboot = reboot[:reboot.index('DataManager::Flush();')]
        reboot = reboot.replace('int TWFunc::tw_reboot', 'int Reboot')
        reboot += '++transitions; return 0;\n}\n'
        driver = r'''
#include <cassert>
#include <initializer_list>
static bool dynamic_partitions, daemon_available, modem_stopped;
static unsigned leases, releases, transitions;
constexpr int X_OK = 1;
int access(const char*, int) { return daemon_available ? 0 : -1; }
#define LOGERR(...) ((void)0)
namespace android { namespace base {
bool GetBoolProperty(const char*, bool) { return dynamic_partitions; }
}}
namespace aera { namespace mobile {
struct StorageLease {
  StorageLease() { ++leases; }
  ~StorageLease() { ++releases; }
  explicit operator bool() const { return modem_stopped; }
};
}}
enum RebootCommand { rb_system, rb_fastboot };
namespace aeraui {
''' + support + '\n' + live + '}\n' + reboot + r'''
int main() {
  for (bool dynamic : {false, true}) for (bool daemon : {false, true})
  for (bool stopped : {false, true}) {
    dynamic_partitions = dynamic; daemon_available = daemon; modem_stopped = stopped;
#ifdef TW_INCLUDE_FASTBOOTD
    const bool supported = dynamic && daemon;
#else
    const bool supported = false;
#endif
    leases = releases = transitions = 0;
    assert(aeraui::RecoveryEnterFastbootd() == (supported && stopped));
    assert(leases == unsigned(supported) && releases == leases);
    assert(transitions == unsigned(supported && stopped));
    leases = releases = transitions = 0;
    assert(Reboot(rb_fastboot) == ((supported && stopped) ? 0 : -1));
    assert(leases == unsigned(supported) && releases == leases);
    assert(transitions == unsigned(supported && stopped));
    leases = releases = transitions = 0;
    assert(Reboot(rb_system) == (stopped ? 0 : -1));
    assert(leases == 1 && releases == 1 && transitions == unsigned(stopped));
  }
}
'''
        path = self.root / 'entry-guards.cpp'
        path.write_text(driver)
        for enabled in (False, True):
            binary = self.root / ('entry-enabled' if enabled else 'entry-disabled')
            flags = ['-DTW_INCLUDE_FASTBOOTD'] if enabled else []
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     *flags, str(path), '-o', str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
