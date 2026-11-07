// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace aera {

// Size probes must not cover recovery's RAM-backed runtime at /vendor or
// /system. Use a fresh, read-only mount of the actual logical block device.
inline bool ReadOnlyPartitionStats(const char* block, const char* filesystem,
                                   struct statfs* result, std::string* error) {
  auto fail = [&](const char* operation, const std::string& path, int code) {
    if (error) *error = std::string(operation) + " " + path + ": " + strerror(code);
    errno = code;
    return false;
  };
  if (!block || !filesystem || !result)
    return fail("Invalid partition-size arguments", "", EINVAL);
  if (strcmp(filesystem, "ext4") && strcmp(filesystem, "erofs"))
    return fail("Unsupported partition-size filesystem", filesystem, EINVAL);
  struct stat device {};
  if (stat(block, &device)) return fail("Cannot inspect size source", block, errno);
  if (!S_ISBLK(device.st_mode)) return fail("Size source is not a block device", block, EINVAL);

  char target[] = "/tmp/aera-partition-size-XXXXXX";
  if (!mkdtemp(target)) return fail("Cannot create size mount", target, errno);
  const char* options = !strcmp(filesystem, "ext4") ? "noload" : nullptr;
  if (mount(block, target, filesystem,
            MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME, options)) {
    const int saved = errno;
    rmdir(target);
    return fail("Cannot mount size source read-only", block, saved);
  }
  int saved = 0;
  if (mount(nullptr, target, nullptr, MS_PRIVATE, nullptr) || statfs(target, result))
    saved = errno;
  // Never hide a busy mount with lazy detach. A failed cleanup is reported,
  // with its private target, rather than pretending the source was released.
  int unmounted;
  do {
    unmounted = umount(target);
  } while (unmounted && errno == EINTR);
  if (unmounted) return fail("Cannot release size mount", target, errno);
  if (rmdir(target)) return fail("Cannot remove size mount directory", target, errno);
  if (saved) return fail("Cannot read isolated partition size", block, saved);
  return true;
}

}  // namespace aera
