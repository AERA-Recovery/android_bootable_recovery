// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <dlfcn.h>

namespace aeraui {

struct NativeBufferLibrary {
  void* handle = nullptr;
  const char* path = nullptr;
  void* allocate = nullptr;
  void* release = nullptr;
  void* describe = nullptr;
  void* get_native_handle = nullptr;
  void* to_native_window = nullptr;
};

// A null reason reports the selected provider; otherwise it describes why
// that candidate was rejected. The caller owns the returned dlopen handle.
using NativeBufferLibraryLog = void (*)(const char* path, const char* reason);

inline NativeBufferLibrary LoadNativeBufferLibrary(
    const char* const* paths, size_t count, NativeBufferLibraryLog log) {
  for (size_t i = 0; i < count; ++i) {
    NativeBufferLibrary candidate;
    candidate.handle = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
    if (candidate.handle == nullptr) {
      if (log != nullptr) log(paths[i], dlerror());
      continue;
    }

    const char* names[] = {
        "AHardwareBuffer_allocate",
        "AHardwareBuffer_release",
        "AHardwareBuffer_describe",
        "AHardwareBuffer_getNativeHandle",
        "_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEPK15AHardwareBuffer",
    };
    void** functions[] = {
        &candidate.allocate, &candidate.release, &candidate.describe,
        &candidate.get_native_handle, &candidate.to_native_window,
    };
    const char* missing = nullptr;
    for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); ++j) {
      *functions[j] = dlsym(candidate.handle, names[j]);
      if (*functions[j] == nullptr) {
        missing = names[j];
        break;
      }
    }
    if (missing != nullptr) {
      if (log != nullptr) log(paths[i], missing);
      dlclose(candidate.handle);
      continue;
    }

    candidate.path = paths[i];
    if (log != nullptr) log(candidate.path, nullptr);
    return candidate;
  }
  return {};
}

inline NativeBufferLibrary LoadNativeBufferLibrary(NativeBufferLibraryLog log) {
  // Prefer the recovery-built buffer runtime. Some device trees supply only
  // a vendor provider; its dependencies must still be compatible with recovery.
  static constexpr const char* paths[] = {
      "/system/lib64/libnativewindow.so",
      "/vendor/lib64/libnativewindow.so",
  };
  return LoadNativeBufferLibrary(paths, sizeof(paths) / sizeof(paths[0]), log);
}

}  // namespace aeraui
