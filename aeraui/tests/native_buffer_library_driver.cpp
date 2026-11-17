// SPDX-License-Identifier: Apache-2.0
#include "../core/native_buffer_library.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::vector<std::string> rejected;
std::vector<std::string> selected;
void Log(const char* path, const char* reason) {
  (reason == nullptr ? selected : rejected).emplace_back(path);
}

void AssertUnloaded(const char* path) {
  void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
  assert(handle == nullptr);
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 4);
  const char* paths[] = {argv[1], argv[2]};
  const int expected = std::atoi(argv[3]);
  const auto library = aeraui::LoadNativeBufferLibrary(paths, 2, Log);
  if (expected == 0) {
    assert(library.handle == nullptr && library.path == nullptr);
    assert(library.allocate == nullptr && library.release == nullptr &&
           library.describe == nullptr && library.get_native_handle == nullptr &&
           library.to_native_window == nullptr);
    assert(selected.empty() && rejected.size() == 2);
  } else {
    assert(library.handle != nullptr);
    assert(std::strcmp(library.path, paths[expected - 1]) == 0);
    assert(selected.size() == 1 && selected[0] == library.path);
    assert(rejected.size() == static_cast<size_t>(expected - 1));
    const void* functions[] = {
        library.allocate, library.release, library.describe,
        library.get_native_handle, library.to_native_window,
    };
    for (const void* function : functions) {
      // Fixtures return their provider identity. All functions must come from
      // the selected library, never a mixture of the two candidates.
      assert(reinterpret_cast<int (*)()>(const_cast<void*>(function))() == expected);
    }
    assert(dlclose(library.handle) == 0);
  }
  AssertUnloaded(paths[0]);
  AssertUnloaded(paths[1]);

  // The caller may omit diagnostic logging without changing selection.
  const auto silent = aeraui::LoadNativeBufferLibrary(paths, 2, nullptr);
  assert((silent.handle != nullptr) == (expected != 0));
  if (silent.handle != nullptr) assert(dlclose(silent.handle) == 0);
  AssertUnloaded(paths[0]);
  AssertUnloaded(paths[1]);
}
