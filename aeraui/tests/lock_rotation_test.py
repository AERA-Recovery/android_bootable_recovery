"""Exercise the engine's rotation dispatch and lock rebuild without a display."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def definition(source, name):
    begin = source.index(name)
    opening = source.index('{', begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('g++'), 'Requires Linux C++')
class LockRotationTests(unittest.TestCase):
    def test_rotation_rebuilds_existing_lock_and_keeps_ambient_state(self):
        source = (ROOT / 'aeraui/core/engine.cpp').read_text(encoding='utf-8')
        rebuild = definition(source, 'void RebuildLockScreen(')
        rotation = definition(source, 'if (action == Action::kToggleRotation ||')
        program = r'''
#include <cassert>
enum class Action { kToggleRotation, kToggleVideoRotation };
constexpr int LV_DISPLAY_ROTATION_90=90, LV_DISPLAY_ROTATION_0=0, ANDROID_LOG_INFO=1;
constexpr char kLogTag[]="test";
struct Object {};
Object previous, replacement;
int shades_closed=0, deleted=0;
void CloseStatusBarShades() { ++shades_closed; }
void lv_obj_delete_async(Object* object) { assert(object==&previous); ++deleted; }
void lv_indev_reset(void*, void*) {}
void lv_display_set_rotation(void*, int) {}
void __android_log_print(int, const char*, const char*, const char*) {}
struct Engine {
  Object* lock_overlay_=nullptr;
  bool lock_ambient_=false, landscape_=false;
  void* pointer_device_=nullptr;
  void* display_=nullptr;
  struct { bool pressed=true; } pointer_;
  int homes=0, builds=0;
  void DismissRecents() {}
  void ShowHome() { ++homes; }
  void ShowLockScreen(bool ambient) {
    assert(lock_overlay_==nullptr);
    assert(ambient==lock_ambient_);
    lock_overlay_=&replacement;
    ++builds;
  }
''' + rebuild + r'''
  void Rotate(Action action) { auto* self=this;
''' + rotation + r'''
  }
};
int main() {
  for (auto action : {Action::kToggleRotation, Action::kToggleVideoRotation}) {
    for (bool ambient : {false,true}) {
      Engine engine;
      engine.lock_overlay_=&previous;
      engine.lock_ambient_=ambient;
      engine.Rotate(action);
      assert(engine.lock_overlay_==&replacement && engine.builds==1);
      assert(engine.lock_ambient_==ambient && !engine.pointer_.pressed);
      assert(engine.homes==(action==Action::kToggleRotation));
    }
  }
  assert(shades_closed==4 && deleted==4);
  Engine unlocked;
  unlocked.Rotate(Action::kToggleRotation);
  assert(unlocked.lock_overlay_==nullptr && unlocked.builds==0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp, binary = Path(directory)/'test.cpp', Path(directory)/'test'
            cpp.write_text('#include <initializer_list>\n'+program, encoding='utf-8')
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
