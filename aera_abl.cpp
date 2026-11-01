// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "aera_abl.hpp"
#include "aera_efisp.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <openssl/sha.h>
#if defined(AERA_ENABLE_EFISP) && AERA_ENABLE_EFISP == 1
#include <LzmaDec.h>
#include <7zAlloc.h>
#endif

namespace aera_efisp {
namespace {
constexpr size_t kMaxInput = 32 * 1024 * 1024;
constexpr size_t kMaxDecoded = 16 * 1024 * 1024;
uint64_t Le64(const unsigned char* p) { return Le32(p) | uint64_t(Le32(p + 4)) << 32; }
struct Range { size_t offset = 0, size = 0; };
// Respect ELF program-header bounds, including the signed authentication segment.
// Ignore partition padding so the common case reads about 280 KiB, not the disk.
bool ElfExtent(const unsigned char* header, size_t bytes, size_t capacity,
               Range* table, size_t* stride, size_t* count, bool* wide) {
  if (bytes < 64 || std::memcmp(header, "\x7f" "ELF", 4) || header[5] != 1 ||
      (header[4] != 1 && header[4] != 2)) return false;
  *wide = header[4] == 2;
  const uint64_t offset = *wide ? Le64(header + 32) : Le32(header + 28);
  *stride = Le16(header + (*wide ? 54 : 42));
  *count = Le16(header + (*wide ? 56 : 44));
  const size_t minimum = *wide ? 56 : 32;
  if (*stride < minimum || *stride > 256 || !*count || *count > 128 ||
      offset > capacity || *count * *stride > capacity - offset ||
      offset + *count * *stride > kMaxInput) return false;
  *table = {static_cast<size_t>(offset), *count * *stride};
  return true;
}
bool Program(const unsigned char* p, bool wide, size_t capacity, Range* out) {
  const uint64_t offset = wide ? Le64(p + 8) : Le32(p + 4);
  const uint64_t bytes = wide ? Le64(p + 32) : Le32(p + 16);
  if (offset > capacity || bytes > capacity - offset) return false;
  *out = {static_cast<size_t>(offset), static_cast<size_t>(bytes)};
  return true;
}
size_t PeSize(const unsigned char* data, size_t bytes) {
  if (bytes < 64 || data[0] != 'M' || data[1] != 'Z') return 0;
  const size_t pe = Le32(data + 60);
  if (pe < 64 || pe > bytes - 24 || std::memcmp(data + pe, "PE\0\0", 4) ||
      Le16(data + pe + 4) != 0xaa64) return 0;
  const size_t optional = Le16(data + pe + 20), count = Le16(data + pe + 6);
  if (optional < 112 || optional > bytes - pe - 24 || !count || count > 96 ||
      Le16(data + pe + 24) != 0x20b || Le16(data + pe + 92) != 10) return 0;
  const size_t table = pe + 24 + optional;
  if (count * 40 > bytes - table) return 0;
  size_t extent = Le32(data + pe + 84);
  if (extent < table + count * 40 || extent > bytes) return 0;
  for (size_t i = 0; i < count; ++i) {
    const auto* section = data + table + i * 40;
    const size_t raw = Le32(section + 20), size = Le32(section + 16);
    if (raw > bytes || size > bytes - raw) return 0;
    extent = std::max(extent, raw + size);
  }
  return extent;
}
struct Scan {
  size_t best = 0, attempts = 0, total = 0;
  AblStatus status = AblStatus::Unknown;
  bool exhausted = false;
  void Visit(const unsigned char* data, size_t bytes, unsigned depth) {
    if (depth > 4 || bytes > 64 * 1024 * 1024 - total) { exhausted = true; return; }
    total += bytes;
    // Same UTF-16 partition-name discriminator as the upstream Canoe checker:
    // 1vivy/gbl_root_canoe, 2934f623f177c2131c14c4ab5add137bb98dc932,
    // submodules/patcher/src/patchs/core.c:patch_abl_gbl. Read-only adaptation;
    // no AVB, DICE or bootloader patching code is imported or executed.
    static const unsigned char needle[] = {'e',0,'f',0,'i',0,'s',0,'p',0,0,0};
    for (size_t i = 0; i + 64 <= bytes; ++i) {
      if (data[i] != 'M' || data[i + 1] != 'Z') continue;
      const size_t size = PeSize(data + i, bytes - i);
      if (size <= best) continue;
      best = size;
      const bool present = std::search(data + i, data + i + size,
          std::begin(needle), std::end(needle)) != data + i + size;
      status = present ? AblStatus::EfispLoader : AblStatus::NoEfispLoader;
    }
#if defined(AERA_ENABLE_EFISP) && AERA_ENABLE_EFISP == 1
    // Parse the known UEFI LZMA GUID-defined section rather than guessing at
    // every 0x5d byte in the image. Bound both compressed and expanded data.
    static const unsigned char guid[] = {0x98,0x58,0x4e,0xee,0x14,0x39,0x59,0x42,
                                         0x9d,0x6e,0xdc,0x7b,0xd7,0x94,0x03,0xcf};
    for (size_t at = 4; at + 20 <= bytes; ++at) {
      if (std::memcmp(data + at, guid, sizeof(guid))) continue;
      const size_t section = at - 4;
      const size_t size = data[section] | size_t(data[section+1]) << 8 | size_t(data[section+2]) << 16;
      const size_t offset = Le16(data + at + 16);
      if (data[section + 3] != 2 || size < 24 || size > bytes - section ||
          offset < 24 || offset >= size) { exhausted = true; continue; }
      if (++attempts > 8) { exhausted = true; return; }
      const auto* compressed = data + section + offset;
      const size_t length = size - offset;
      if (length < 6) { exhausted = true; continue; }
      CLzmaProps props{};
      if (LzmaProps_Decode(&props, compressed, 5) != SZ_OK || props.dicSize > kMaxDecoded) {
        exhausted = true; continue;
      }
      bool decoded = false;
      for (size_t header : {size_t(13), size_t(5)}) {
        if (length <= header) continue;
        const uint64_t declared = header == 13 ? Le64(compressed + 5) : UINT64_MAX;
        if (declared != UINT64_MAX && (declared < 64 || declared > kMaxDecoded)) continue;
        const size_t limit = declared == UINT64_MAX ? kMaxDecoded : declared;
        std::vector<unsigned char> output(limit);
        SizeT output_size = output.size(), input_size = length - header;
        ELzmaStatus result_status = LZMA_STATUS_NOT_SPECIFIED;
        ISzAlloc allocator = {SzAlloc, SzFree};
        const SRes result = LzmaDecode(output.data(), &output_size, compressed + header,
            &input_size, compressed, 5, LZMA_FINISH_END, &result_status, &allocator);
        const bool complete = result_status == LZMA_STATUS_FINISHED_WITH_MARK ||
            (declared != UINT64_MAX && output_size == declared &&
             result_status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK);
        if (result != SZ_OK || !complete || output_size < 64) continue;
        Visit(output.data(), output_size, depth + 1);
        decoded = true;
        break;
      }
      if (!decoded) exhausted = true;
    }
#else
    (void)depth;
#endif
  }
};
std::string Hash(const std::vector<unsigned char>& bytes) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(bytes.data(), bytes.size(), digest);
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
  return out;
}
std::vector<AblSample> DiscoverSamples(const std::vector<AblSample>& cache) {
  std::vector<AblSample> samples;
  for (const char* name : {"abl_a", "abl_b", "abl"}) {
    aera_frp::Target found;
    bool ambiguous = false;
    for (const char* root : {"/dev/block/by-name/", "/dev/block/bootdevice/by-name/"}) {
      aera_frp::Target candidate;
      if (!aera_frp::Inspect(std::string(root) + name, &candidate)) continue;
      if (!found.path.empty() && found.device != candidate.device) ambiguous = true;
      found = candidate;
    }
    if (!found.path.empty() && !ambiguous) samples.push_back(ReadAbl(found.path, name, cache));
  }
  return samples;
}
std::mutex guard_mutex;
AblSession session;
std::vector<AblSample> baseline_cache;
std::string acknowledged;
bool initialized = false;
} // namespace

const char* AblStatusName(AblStatus status) {
  switch (status) {
    case AblStatus::EfispLoader: return "EFISP loader present";
    case AblStatus::NoEfispLoader: return "EFISP loader absent";
    default: return "unknown";
  }
}
AblStatus InspectAbl(const std::vector<unsigned char>& image) {
  if (image.size() < 64 || image.size() > kMaxInput) return AblStatus::Unknown;
  Scan scan;
  Range table;
  size_t stride = 0, count = 0;
  bool wide = false;
  if (ElfExtent(image.data(), image.size(), image.size(), &table, &stride, &count, &wide)) {
    if (table.offset + table.size > image.size()) return AblStatus::Unknown;
    for (size_t i = 0; i < count; ++i) {
      const auto* header = image.data() + table.offset + stride * i;
      Range part;
      if (!Program(header, wide, image.size(), &part)) return AblStatus::Unknown;
      if (Le32(header) == 1 && part.size) scan.Visit(image.data() + part.offset, part.size, 0);
    }
  } else if (image[0] == 'M' && image[1] == 'Z') {
    scan.Visit(image.data(), image.size(), 0);
  } else return AblStatus::Unknown;
  return scan.exhausted ? AblStatus::Unknown : scan.status;
}
AblSample ReadAbl(const std::string& path, const std::string& name, const std::vector<AblSample>& cache) {
  AblSample result{name, "", AblStatus::Unknown};
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return result;
  struct Close { int fd; ~Close() { close(fd); } } close_fd{fd};
  struct stat st{};
  uint64_t capacity = 0;
  if (fstat(fd, &st)) return result;
  if (S_ISBLK(st.st_mode)) {
    if (ioctl(fd, BLKGETSIZE64, &capacity)) return result;
  } else if (S_ISREG(st.st_mode) && st.st_size > 0) capacity = st.st_size;
  else return result;
  if (capacity < 64 || capacity > kMaxInput) return result;
  std::vector<unsigned char> bytes(64);
  if (!ReadAt(fd, bytes.data(), bytes.size(), 0)) return result;
  Range table;
  size_t stride = 0, count = 0, extent = capacity;
  bool wide = false;
  if (ElfExtent(bytes.data(), bytes.size(), capacity, &table, &stride, &count, &wide)) {
    bytes.resize(std::max(size_t(64), table.offset + table.size));
    if (!ReadAt(fd, bytes.data(), bytes.size(), 0)) return result;
    extent = bytes.size();
    for (size_t i = 0; i < count; ++i) {
      Range program;
      if (!Program(bytes.data() + table.offset + stride * i, wide, capacity, &program)) return result;
      if (program.size) extent = std::max(extent, program.offset + program.size);
    }
  }
  if (extent > kMaxInput) return result;
  bytes.resize(extent);
  if (!ReadAt(fd, bytes.data(), bytes.size(), 0)) return result;
  result.digest = Hash(bytes);
  for (const auto& previous : cache) {
    if (previous.name == name && previous.digest == result.digest) {
      result.status = previous.status;
      return result; // Unchanged image: no decompression or PE scan.
    }
  }
  result.status = InspectAbl(bytes);
  return result;
}
void AblSession::Capture(const std::vector<AblSample>& samples) {
  if (captured_) return;
  captured_ = true;
  baseline_ = samples;
}
AblWarning AblSession::Check(const std::vector<AblSample>& samples) const {
  AblWarning warning;
  for (const auto& before : baseline_) {
    if (before.status != AblStatus::EfispLoader) continue;
    const auto now = std::find_if(samples.begin(), samples.end(), [&](const AblSample& item) {
      return item.name == before.name;
    });
    if (now != samples.end() && now->digest == before.digest && !now->digest.empty()) continue;
    if (now != samples.end() && now->status == AblStatus::EfispLoader) continue;
    const bool absent = now != samples.end() && now->status == AblStatus::NoEfispLoader;
    warning.text += before.name + (absent
        ? ": the vulnerable ABL has been overwritten. The EFISP loader is no longer present.\n"
        : ": ABL has changed or cannot be read. EFISP loader support could not be verified.\n");
    warning.token += before.name + ":" + (now == samples.end() ? "unreadable" : now->digest) + ":" +
                     (absent ? "absent" : "unknown") + ";";
  }
  if (!warning.text.empty()) warning.text +=
      "\nAn installed EFISP mod may no longer boot, and Android may fail to start. "
      "Cancel to restore a compatible vulnerable ABL before leaving recovery, "
      "or explicitly continue if this change was intentional.";
  return warning;
}
std::string CaptureAblBaseline() {
  if (!Enabled()) return {};
  std::lock_guard<std::mutex> lock(guard_mutex);
  if (initialized) return {};
  initialized = true;
  baseline_cache = DiscoverSamples({});
  session.Capture(baseline_cache);
  std::string summary;
  for (const auto& sample : baseline_cache)
    summary += "EFISP ABL baseline: " + sample.name + ": " + AblStatusName(sample.status) + "\n";
  if (summary.empty()) summary = "EFISP ABL baseline: no readable ABL partitions found\n";
  return summary;
}
std::vector<AblSample> CurrentAblSamples() {
  if (!Enabled()) return {};
  std::lock_guard<std::mutex> lock(guard_mutex);
  if (!initialized) return {};
  baseline_cache = DiscoverSamples(baseline_cache);
  return baseline_cache;
}
AblWarning AblExitWarning() {
  if (!Enabled()) return {};
  std::lock_guard<std::mutex> lock(guard_mutex);
  // Never establish the boot baseline late, after an installer could overwrite it.
  if (!initialized) return {};
  auto current = DiscoverSamples(baseline_cache);
  auto warning = session.Check(current);
  baseline_cache = std::move(current);
  if (!warning.token.empty() && warning.token == acknowledged) return {};
  return warning;
}
void AcknowledgeAblExit(const std::string& token) {
  std::lock_guard<std::mutex> lock(guard_mutex);
  acknowledged = token;
}
void ClearAblExitAcknowledgement() {
  std::lock_guard<std::mutex> lock(guard_mutex);
  acknowledged.clear();
}
} // namespace aera_efisp
