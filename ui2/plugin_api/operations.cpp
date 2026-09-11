/* SPDX-License-Identifier: Apache-2.0 */
#include "operations.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recovery_ui2::plugin_api {
namespace {
constexpr char kAeraDirectory[] = "/data/media/0/AERA";
constexpr char kBackupDirectory[] = "SettingsBackups";
constexpr char kSettingsFile[] = "preferences.conf";
constexpr char kBackupFile[] = "recovery-preferences.conf";
constexpr size_t kMaximumSettingsBytes = 64 * 1024;

class Fd final {
 public:
  explicit Fd(int value = -1) : value_(value) {}
  ~Fd() { if (value_ >= 0) close(value_); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : value_(other.value_) { other.value_ = -1; }
  Fd &operator=(Fd &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) close(value_);
      value_ = other.value_;
      other.value_ = -1;
    }
    return *this;
  }
  int get() const { return value_; }
 private:
  int value_;
};

bool CopyBounded(int source_directory, const char *source_name,
                 int destination_directory, const char *destination_name) {
  Fd source(openat(source_directory, source_name,
                   O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  if (source.get() < 0 || fstat(source.get(), &info) != 0 ||
      !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<uint64_t>(info.st_size) > kMaximumSettingsBytes) return false;
  const std::string temporary = std::string(destination_name) + ".new";
  unlinkat(destination_directory, temporary.c_str(), 0);
  Fd destination(openat(destination_directory, temporary.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0600));
  if (destination.get() < 0) return false;
  std::array<uint8_t, 4096> buffer{};
  uint64_t total = 0;
  while (true) {
    const ssize_t count = read(source.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return false;
    if (count == 0) break;
    total += static_cast<uint64_t>(count);
    if (total > kMaximumSettingsBytes) return false;
    size_t offset = 0;
    while (offset < static_cast<size_t>(count)) {
      const ssize_t written = write(destination.get(), buffer.data() + offset,
                                    static_cast<size_t>(count) - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) return false;
      offset += static_cast<size_t>(written);
    }
  }
  if (fsync(destination.get()) != 0 || total == 0) return false;
  if (renameat(destination_directory, temporary.c_str(), destination_directory,
               destination_name) != 0) {
    unlinkat(destination_directory, temporary.c_str(), 0);
    return false;
  }
  return fsync(destination_directory) == 0;
}

bool OpenDirectories(Fd &aera, Fd &backup) {
  aera = Fd(open(kAeraDirectory,
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (aera.get() < 0) return false;
  if (mkdirat(aera.get(), kBackupDirectory, 0700) != 0 && errno != EEXIST)
    return false;
  backup = Fd(openat(aera.get(), kBackupDirectory,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  return backup.get() >= 0 && fstat(backup.get(), &info) == 0 &&
         S_ISDIR(info.st_mode);
}
}  // namespace

bool OperationAllowed(const plugins::Plugin &plugin, Operation operation) {
  if (!plugins::IsGeneric(plugin)) return false;
  switch (operation) {
    case Operation::kBackupSettings:
      return plugins::HasPermission(plugin, "settings-backup");
    case Operation::kRestoreSettings:
      return plugins::HasPermission(plugin, "settings-restore");
  }
  return false;
}

bool RunOperation(const plugins::Plugin &plugin, Operation operation,
                  std::string &result) {
  if (!OperationAllowed(plugin, operation)) {
    result = "Permission denied by the AERA host.";
    return false;
  }
  Fd aera;
  Fd backup;
  if (!OpenDirectories(aera, backup)) {
    result = "Shared recovery storage is locked or unavailable.";
    return false;
  }
  const bool success = operation == Operation::kBackupSettings
      ? CopyBounded(aera.get(), kSettingsFile, backup.get(), kBackupFile)
      : CopyBounded(backup.get(), kBackupFile, aera.get(), kSettingsFile);
  if (success) {
    result = operation == Operation::kBackupSettings
        ? "Recovery preferences backed up to AERA/SettingsBackups."
        : "Recovery preferences restored. Reopen recovery to reload them.";
  } else {
    result = operation == Operation::kBackupSettings
        ? "Could not create the recovery settings backup."
        : "No valid recovery settings backup was found.";
  }
  return success;
}

const char *OperationTitle(Operation operation) {
  return operation == Operation::kBackupSettings
      ? "Back up recovery settings?" : "Restore recovery settings?";
}
const char *OperationPrompt(Operation operation) {
  return operation == Operation::kBackupSettings
      ? "This isolated plugin is asking AERA to copy recovery preferences into "
        "AERA/SettingsBackups. The plugin cannot read the file itself."
      : "This isolated plugin is asking AERA to replace current recovery "
        "preferences with the saved copy. The plugin cannot write the file itself.";
}
}  // namespace recovery_ui2::plugin_api
