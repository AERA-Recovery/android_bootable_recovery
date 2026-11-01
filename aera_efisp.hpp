// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aera_frp_wipe.hpp"
#include <cstring>

namespace aera_efisp {
using Target = aera_frp::Target;
inline constexpr bool Enabled() {
#if defined(AERA_ENABLE_EFISP) && AERA_ENABLE_EFISP == 1
  return true;
#else
  return false;
#endif
}
inline constexpr const char* WipeWarning() {
  return "Wiping EFISP requires Format Data before the device can boot into Android again. "
         "Format Data erases apps and files in internal storage.";
}
inline bool Discover(Target* target) {
  if (!Enabled() || !target) return false;
  Target found;
  for (const char* path : {"/dev/block/by-name/efisp", "/dev/block/bootdevice/by-name/efisp"}) {
    Target candidate;
    if (!aera_frp::Inspect(path, &candidate)) continue;
    if (!found.path.empty() && found.device != candidate.device) return false;
    found = candidate;
  }
  if (found.path.empty()) return false;
  *target = found;
  return true;
}
inline bool ReadAt(int fd, void* buffer, size_t count, uint64_t offset) {
  auto* out = static_cast<unsigned char*>(buffer);
  while (count) {
    const ssize_t n = pread(fd, out, count, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    out += n; count -= n; offset += n;
  }
  return true;
}
inline uint16_t Le16(const unsigned char* p) { return p[0] | uint16_t(p[1]) << 8; }
inline uint32_t Le32(const unsigned char* p) { return Le16(p) | uint32_t(Le16(p + 2)) << 16; }
// EFISP contains a raw executable, not a FAT filesystem. Accept ARM64 EFI
// applications only; a renamed Windows executable or sparse image is not EFI.
inline bool ValidImage(int fd, uint64_t capacity) {
  struct stat st{};
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 64 ||
      uint64_t(st.st_size) > capacity) return false;
  const uint64_t size = st.st_size;
  unsigned char dos[64]{}, pe[24]{}, optional[112]{};
  if (!ReadAt(fd, dos, sizeof(dos), 0) || dos[0] != 'M' || dos[1] != 'Z') return false;
  const uint64_t offset = Le32(dos + 60);
  if (offset < sizeof(dos) || offset > size - sizeof(pe) ||
      !ReadAt(fd, pe, sizeof(pe), offset) || std::memcmp(pe, "PE\0\0", 4) ||
      Le16(pe + 4) != 0xaa64) return false;
  const uint64_t optional_size = Le16(pe + 20), sections = Le16(pe + 6);
  if (optional_size < sizeof(optional) || !sections || sections > 96 ||
      optional_size > size - offset - sizeof(pe)) return false;
  if (!ReadAt(fd, optional, sizeof(optional), offset + sizeof(pe)) ||
      Le16(optional) != 0x20b || Le16(optional + 68) != 10) return false;
  const uint64_t table = offset + sizeof(pe) + optional_size;
  if (sections * 40 > size - table) return false;
  for (uint64_t i = 0; i < sections; ++i) {
    unsigned char section[40]{};
    if (!ReadAt(fd, section, sizeof(section), table + i * sizeof(section))) return false;
    const uint64_t length = Le32(section + 16), start = Le32(section + 20);
    if (length && (start > size || length > size - start)) return false;
  }
  return true;
}
inline bool ValidImage(const std::string& path, uint64_t capacity) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  const bool valid = ValidImage(fd, capacity);
  close(fd);
  return valid;
}
// Readback is bounded to the executable bytes, just like fastboot flash efisp.
// Bytes after the executable are deliberately left untouched.
inline bool CompareWritten(int source, int destination, uint64_t bytes) {
  if (!bytes || bytes > INT64_MAX) return false;
  std::array<unsigned char, 64 * 1024> expected{}, actual{};
  for (uint64_t offset = 0; offset < bytes;) {
    const size_t count = std::min<uint64_t>(expected.size(), bytes - offset);
    if (!ReadAt(source, expected.data(), count, offset) ||
        !ReadAt(destination, actual.data(), count, offset) ||
        std::memcmp(expected.data(), actual.data(), count)) return false;
    offset += count;
  }
  return true;
}
// fd primitive is exercised against disposable files by host tests. Never
// discard, truncate, format or erase the destination before writing the image.
inline bool WriteAndVerify(int source, int destination, uint64_t bytes) {
  if (!bytes || bytes > INT64_MAX) return false;
  std::array<unsigned char, 64 * 1024> buffer{};
  for (uint64_t offset = 0; offset < bytes;) {
    const size_t count = std::min<uint64_t>(buffer.size(), bytes - offset);
    if (!ReadAt(source, buffer.data(), count, offset)) return false;
    size_t written = 0;
    while (written < count) {
      const ssize_t n = pwrite(destination, buffer.data() + written, count - written,
                               static_cast<off_t>(offset + written));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      written += n;
    }
    offset += count;
  }
  return fsync(destination) == 0 && CompareWritten(source, destination, bytes);
}
inline bool Flash(const std::string& path, const Target& target) {
  Target current;
  if (!Discover(&current) || current.device != target.device || current.bytes != target.bytes) return false;
  const int source = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (source < 0) return false;
  struct stat image{};
  if (!ValidImage(source, target.bytes) || fstat(source, &image)) { close(source); return false; }
  const int block = open(current.path.c_str(), O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
  if (block < 0) { close(source); return false; }
  struct stat device{};
  uint64_t capacity = 0;
  const bool valid = fstat(block, &device) == 0 && S_ISBLK(device.st_mode) &&
      device.st_rdev == target.device && ioctl(block, BLKGETSIZE64, &capacity) == 0 && capacity == target.bytes;
  const bool ok = valid && WriteAndVerify(source, block, image.st_size);
  close(source);
  const int result = close(block);
  return ok && result == 0;
}
inline bool Wipe(const std::string& acknowledgement) {
  Target target;
  if (acknowledgement != "wipe-efisp" || !Discover(&target)) return false;
  const int fd = open(target.path.c_str(), O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat st{};
  uint64_t bytes = 0;
  const bool valid = fstat(fd, &st) == 0 && S_ISBLK(st.st_mode) &&
      st.st_rdev == target.device && ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes == target.bytes;
  const bool ok = valid && aera_frp::ZeroAndVerify(fd, bytes);
  const int result = close(fd);
  return ok && result == 0;
}
} // namespace aera_efisp
