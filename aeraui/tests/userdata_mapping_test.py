"""Exercise the real cleanup policy and POSIX probes without a device.

Commands and sleeps are fake; no mapping or filesystem is modified.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOLS = (
    ('none', [], 'none'),
    ('of-dmctl', ['-DOF_USE_DMCTL=1'], 'dmctl delete userdata'),
    ('tw-dmctl', ['-DTW_USE_DMCTL=1'], 'dmctl delete userdata'),
    ('dmsetup', ['-DAERA_USE_DMSETUP=1'], 'dmsetup remove -f userdata'),
)


DRIVER = r'''
// SPDX-License-Identifier: Apache-2.0
#include "aera_userdata_mapping.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

using aera::userdata_mapping::State;

struct Operations {
  State initial = State::kPresent;
  unsigned queries = 0, commands = 0, sleeps = 0, logs = 0;
  unsigned disappear_after = 0, error_query = 0;
  int command_result = 0;
  bool removed = false;
  State GetState() {
    ++queries;
    if (queries == error_query) return State::kError;
    if (initial != State::kPresent) return initial;
    return removed && sleeps >= disappear_after ? State::kAbsent : State::kPresent;
  }
  int Remove(const char* command) {
    const char* expected = aera::userdata_mapping::kCommand ? aera::userdata_mapping::kCommand : "disabled";
    assert(!strcmp(command, expected));
    ++commands;
    removed = true;
    return command_result;
  }
  void LogResult(const char*, int result) { assert(result == command_result); ++logs; }
  void Sleep(unsigned usec) {
    assert(usec == 50000);
    ++sleeps;
    assert(sleeps <= 40);
  }
};

static void ProbeFilesystem() {
  char directory[] = "/tmp/aera-mapping-probe-XXXXXX";
  assert(mkdtemp(directory));
  const std::string node = std::string(directory) + "/userdata";
  assert(aera::userdata_mapping::Probe(node.c_str()) == State::kAbsent);
  int fd = open(node.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(fd >= 0 && close(fd) == 0);
  assert(aera::userdata_mapping::Probe(node.c_str()) == State::kPresent);
  assert(aera::userdata_mapping::Probe((node + "/child").c_str()) == State::kError);
  assert(unlink(node.c_str()) == 0);
  assert(symlink("missing-dm-device", node.c_str()) == 0);
  assert(aera::userdata_mapping::Probe(node.c_str()) == State::kPresent);
  assert(unlink(node.c_str()) == 0);
  assert(aera::userdata_mapping::Probe(node.c_str()) == State::kAbsent);
  assert(rmdir(directory) == 0);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  if (scenario == "probe") { ProbeFilesystem(); return 0; }
  if (scenario == "command") {
    puts(aera::userdata_mapping::kCommand ? aera::userdata_mapping::kCommand : "none");
    return 0;
  }
  Operations operations;
  bool expected = true;
  unsigned expected_sleeps = 0;
  if (scenario == "absent") operations.initial = State::kAbsent;
  else if (scenario == "immediate") {}
  else if (scenario == "delayed") operations.disappear_after = expected_sleeps = 3;
  else if (scenario == "deadline") operations.disappear_after = expected_sleeps = 40;
  else if (scenario == "too-late" || scenario == "survives") {
    operations.disappear_after = scenario == "too-late" ? 41 : 1000;
    expected = false; expected_sleeps = 40;
  } else if (scenario == "command-failed" || scenario == "tool-missing") {
    operations.command_result = scenario == "tool-missing" ? 127 : -1;
    expected = false;
  } else if (scenario == "initial-error") {
    operations.initial = State::kError; expected = false;
  } else if (scenario == "poll-error") {
    operations.error_query = 2; expected = false;
  } else assert(false);
  const bool result = aera::userdata_mapping::Release(operations);
  if (!aera::userdata_mapping::kCommand) {
    assert(result && operations.queries == 0 && operations.commands == 0);
    assert(operations.sleeps == 0 && operations.logs == 0);
  } else {
    assert(result == expected && operations.sleeps == expected_sleeps);
    const unsigned commands = operations.initial == State::kPresent ? 1 : 0;
    assert(operations.commands == commands && operations.logs == commands);
  }
}
'''


def compile_driver(path, binary, flags):
    result = subprocess.run(['g++', '-std=c++17', '-UNDEBUG', '-Wall', '-Wextra',
                             '-Werror', '-I', str(ROOT), *flags,
                             str(path), '-o', str(binary)],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr)


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('g++'),
                     'Requires Linux and a native C++ compiler')
class UserdataMappingPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        driver = Path(cls.temp.name) / 'driver.cpp'
        driver.write_text(DRIVER)
        cls.binaries = []
        for name, flags, command in TOOLS:
            binary = Path(cls.temp.name) / name
            compile_driver(driver, binary, flags)
            cls.binaries.append((name, binary, command))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check_scenarios(self, *scenarios):
        for name, binary, _ in self.binaries:
            for scenario in scenarios:
                with self.subTest(tool=name, scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=5,
                                   capture_output=True, text=True)

    def test_existing_flags_select_the_original_commands(self):
        for name, binary, expected in self.binaries:
            with self.subTest(tool=name):
                result = subprocess.run([str(binary), 'command'], check=True,
                                        capture_output=True, text=True)
                self.assertEqual(result.stdout.strip(), expected)

    def test_real_probes_distinguish_absence_dangling_links_and_errors(self):
        self.check_scenarios('probe')

    def test_absent_mapping_does_not_execute_or_wait(self):
        self.check_scenarios('absent')

    def test_success_waits_only_as_long_as_removal_needs(self):
        self.check_scenarios('immediate', 'delayed')

    def test_final_deadline_check_and_surviving_mapping(self):
        self.check_scenarios('deadline', 'too-late', 'survives')

    def test_failed_or_missing_tool_stops_cleanup(self):
        self.check_scenarios('command-failed', 'tool-missing')

    def test_initial_and_post_command_probe_errors_stop_cleanup(self):
        self.check_scenarios('initial-error', 'poll-error')


if __name__ == '__main__':
    unittest.main()
