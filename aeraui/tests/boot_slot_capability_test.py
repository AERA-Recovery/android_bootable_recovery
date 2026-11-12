#!/usr/bin/env python3
"""Exercise slot getters/setters with the actual backend compiled for both layouts."""
from pathlib import Path
import subprocess
import argparse
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path,
    default=Path(__file__).resolve().parents[2]/'aeraui/platform/aera_backend.cpp')
args = parser.parse_args()
source = args.source.read_text(encoding='utf-8')
getter = source[source.index('std::string RecoverySlot()'):source.index('std::string RecoveryBootSlot()')]
setter = source[source.index('bool RecoverySetActiveSlot('):source.index('SnapshotCowStatus RecoverySnapshotCowStatus()')]
prefix = r'''
#include <cassert>
#include <string>
#define LOGINFO(...) do {} while(0)
constexpr int MNT_DETACH=2;
int umount2(const char*,int) { assert(false); return -1; }
struct Manager {
  std::string active="A"; int writes=0;
  std::string Get_Active_Slot_Display() { return active; }
  bool Find_Partition_By_Path(const char*) { return false; }
  bool UnMount_By_Path(const char*,bool) { assert(false); return false; }
  void Set_Active_Slot(const std::string &slot) { ++writes; active=slot; }
} PartitionManager;
'''
suffix = r'''
int main() {
#ifdef AB_OTA_UPDATER
  assert(RecoverySlot()=="A");
  assert(RecoverySetActiveSlot("A") && PartitionManager.writes==0);
  assert(RecoverySetActiveSlot("B") && PartitionManager.writes==1);
  assert(RecoverySlot()=="B");
#else
  assert(RecoverySlot().empty());
  assert(!RecoverySetActiveSlot("A") && !RecoverySetActiveSlot("B"));
  assert(PartitionManager.writes==0);
#endif
  assert(!RecoverySetActiveSlot("X"));
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory/'test.cpp').write_text(prefix+getter+setter+suffix)
    for ab in (False, True):
        executable = directory/('ab' if ab else 'non-ab')
        command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', str(directory/'test.cpp'), '-o', str(executable)]
        if ab:
            command.append('-DAB_OTA_UPDATER=1')
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
print('A/B slot switching and non-A/B no-write behavior passed.')
