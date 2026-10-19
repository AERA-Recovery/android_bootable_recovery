/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace aeraui::plugin_api {

inline int CloseDescriptorRange(unsigned first, unsigned last, unsigned flags) {
  return static_cast<int>(syscall(SYS_close_range, first, last, flags));
}

inline bool CloseUnrelatedDescriptors(
    int (*close_range)(unsigned, unsigned, unsigned) = CloseDescriptorRange) {
  if (close_range(5U, ~0U, 0) == 0) return true;
  if (errno != ENOSYS) return false;

  const int directory = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) return false;
  struct Entry {
    uint64_t inode;
    int64_t offset;
    unsigned short length;
    unsigned char type;
    char name[1];
  };
  // This runs after fork in a multithreaded host; avoid allocating DIR state.
  alignas(uint64_t) char buffer[4096];
  bool success = true;
  while (success) {
    const long count = syscall(SYS_getdents64, directory, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { success = count == 0; break; }
    for (size_t pos = 0; pos < static_cast<size_t>(count);) {
      const size_t remaining = static_cast<size_t>(count) - pos;
      if (remaining < offsetof(Entry, name) + 1) { success = false; break; }
      const auto *entry = reinterpret_cast<const Entry *>(buffer + pos);
      if (entry->length < offsetof(Entry, name) + 1 || entry->length > remaining) {
        success = false; break;
      }
      const size_t limit = entry->length - offsetof(Entry, name);
      const char *name = buffer + pos + offsetof(Entry, name);
      unsigned descriptor = 0;
      size_t digit = 0;
      for (; digit < limit && name[digit] >= '0' && name[digit] <= '9'; ++digit) {
        const unsigned value = static_cast<unsigned>(name[digit] - '0');
        if (descriptor > (INT_MAX - value) / 10U) break;
        descriptor = descriptor * 10U + value;
      }
      if (digit && digit < limit && name[digit] == '\0' &&
          descriptor >= 5 && descriptor != static_cast<unsigned>(directory))
        close(static_cast<int>(descriptor));
      pos += entry->length;
    }
  }
  close(directory);
  return success;
}

}  // namespace aeraui::plugin_api
