/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aera::flashlog {
constexpr size_t kManualLogcatLimit = 128 * 1024 * 1024;
constexpr size_t kManualRecoveryLimit = 32 * 1024 * 1024;
constexpr size_t kManualKernelLimit = 8 * 1024 * 1024;
constexpr size_t kManualStateLimit = 4 * 1024 * 1024;
struct Options {
  bool manual = false;
  std::string directory;
  std::string archive_directory;
  std::string recovery_log = "/tmp/recovery.log";
  std::string summary;
  std::function<std::string()> final_summary;
  std::vector<std::string> logcat, kernel;
  std::vector<std::pair<std::string, std::vector<std::string>>> diagnostics;
  size_t logcat_limit = 16 * 1024 * 1024;
  size_t recovery_limit = 8 * 1024 * 1024;
  size_t kernel_limit = 2 * 1024 * 1024;
  std::function<bool(const std::string &, const std::string &)> archive;
  std::function<void(const std::string &)> permissions;
};
Options PlatformOptions(const std::string &file, const std::string &method,
                        const std::string &details, bool manual = false);
std::vector<std::string> PlatformDirectories(bool manual = false);

// One outer recording covers nested ZIP/image calls and all their cleanup.
class Recording {
 public:
  Recording(const std::string &file, const std::string &method,
            const std::string &details = "", bool manual = false);
  ~Recording();
  int Finish(int result);
  bool Started() const;
  const std::string &ArchivePath() const { return archive_path_; }
  Recording(const Recording &) = delete;
  Recording &operator=(const Recording &) = delete;
 private:
  struct Session;
  std::unique_ptr<Session> session_;
  std::string archive_path_;
};

struct Report {
  std::string name, path;
  uint64_t bytes = 0;
  int64_t timestamp = 0;
  int32_t nanoseconds = 0;
};
std::vector<Report> Reports(bool manual = false);
// Only generated, regular ZIPs from the configured log roots may be opened.
int OpenReport(const std::string &name, uint64_t *bytes, bool manual = false);
enum class ManualState { kIdle, kStarting, kRecording, kSaving, kFailed };
struct ManualStatus {
  ManualState state = ManualState::kIdle;
  uint64_t elapsed_seconds = 0;
  std::string path, error;
};
ManualStatus GetManualStatus();
void ToggleManual();
bool SensitiveSetting(const std::string &key);
std::string SingleLine(const std::string &value);
}  // namespace aera::flashlog
