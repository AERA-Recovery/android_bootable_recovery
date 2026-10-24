/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>
#include <utility>

namespace aeraui::fonts {

inline bool CustomFontId(const std::string& id) {
  return id.size() == 63 && id.compare(0, 7, "custom-") == 0 &&
      std::all_of(id.begin() + 7, id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
}

class CustomFontStorage {
 public:
  static constexpr off_t kMaximumBytes = 32 * 1024 * 1024;
  explicit CustomFontStorage(std::string directory) : directory_(std::move(directory)) {}

  std::string Path(const std::string& id) const {
    return CustomFontId(id) ? directory_ + "/" + id + ".ttf" : std::string{};
  }

  bool Has(const std::string& id) const {
    const auto path = Path(id);
    struct stat info{};
    return !path.empty() && lstat(path.c_str(), &info) == 0 &&
        S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= kMaximumBytes;
  }

  std::vector<std::string> List() const {
    std::vector<std::string> ids;
    DIR* directory = opendir(directory_.c_str());
    if (!directory) return ids;
    while (auto* item = readdir(directory)) {
      const std::string name = item->d_name;
      if (name.size() != 67 || name.compare(63, 4, ".ttf") != 0) continue;
      const std::string id = name.substr(0, 63);
      if (Has(id)) ids.push_back(id);
    }
    closedir(directory);
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  // Import first, activate/save outside this class, then prune older imports.
  std::string Import(const std::string& source, std::string* error) const {
    auto fail = [error](const char* message) {
      if (error) *error = message;
      return std::string{};
    };
    const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) return fail("Could not read the font file.");
    struct stat before{};
    if (fstat(input, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 || before.st_size > kMaximumBytes) {
      close(input);
      return fail("Choose a valid TTF/OTF file no larger than 32 MB.");
    }
    struct stat directory_info{};
    struct statvfs capacity{};
    if (lstat(directory_.c_str(), &directory_info) != 0 ||
        !S_ISDIR(directory_info.st_mode) ||
        statvfs(directory_.c_str(), &capacity) != 0 ||
        static_cast<uint64_t>(capacity.f_bavail) * capacity.f_frsize <
            static_cast<uint64_t>(before.st_size) + 1024 * 1024) {
      close(input);
      return fail("Font storage is unavailable or full.");
    }
    std::string temporary = directory_ + "/.font-XXXXXX";
    const int output = mkstemp(temporary.data());
    if (output < 0) { close(input); return fail("Could not save the custom font."); }
    fcntl(output, F_SETFD, FD_CLOEXEC);
    EVP_MD_CTX* hash = EVP_MD_CTX_new();
    bool ok = hash && EVP_DigestInit_ex(hash, EVP_sha256(), nullptr) == 1;
    off_t remaining = before.st_size;
    std::array<unsigned char, 16384> buffer{};
    while (ok && remaining > 0) {
      const ssize_t count = read(input, buffer.data(),
          std::min<off_t>(remaining, buffer.size()));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { ok = false; break; }
      if (EVP_DigestUpdate(hash, buffer.data(), count) != 1) { ok = false; break; }
      for (ssize_t offset = 0; offset < count;) {
        const ssize_t written = write(output, buffer.data() + offset, count - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        offset += written;
      }
      remaining -= count;
    }
    struct stat after{};
    ok = ok && fstat(input, &after) == 0 && before.st_size == after.st_size &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    close(input);
    if (ok && fchmod(output, 0660) != 0) ok = false;
    if (ok && fsync(output) != 0) ok = false;
    if (close(output) != 0) ok = false;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned digest_size = 0;
    if (hash && EVP_DigestFinal_ex(hash, digest, &digest_size) != 1) ok = false;
    EVP_MD_CTX_free(hash);
    ok = ok && digest_size == 32;
    std::string id = "custom-";
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 28; ++i) {
      id += hex[digest[i] >> 4];
      id += hex[digest[i] & 15];
    }
    const auto destination = Path(id);
    if (!ok || rename(temporary.c_str(), destination.c_str()) != 0) {
      unlink(temporary.c_str());
      return fail("Could not save the custom font.");
    }
    return id;
  }

  void Prune(const std::string& keep) const {
    DIR* directory = opendir(directory_.c_str());
    if (!directory) return;
    const int fd = dirfd(directory);
    while (auto* item = readdir(directory)) {
      const std::string name = item->d_name;
      if (name.size() != 67 || name.compare(63, 4, ".ttf") != 0) continue;
      const auto id = name.substr(0, 63);
      if (CustomFontId(id) && id != keep) unlinkat(fd, name.c_str(), 0);
    }
    closedir(directory);
  }

 private:
  std::string directory_;
};

}  // namespace aeraui::fonts
