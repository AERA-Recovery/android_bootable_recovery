/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "protocol.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace aeraui::plugin_api {
struct Activity {
  std::string plugin, name, title, detail, storage;
  uint32_t id = 0, progress = kProgressUnknown, flags = 0;
  uint64_t updated_ms = 0;
  bool cancelling = false;
  std::string result_file;
  bool Active() const { return (flags & kActivityActive) != 0; }
};
bool SetActivity(const std::string &plugin, const std::string &name,
                 const std::string &storage, const Message &message,
                 std::string &error);
std::vector<Activity> Activities();
void RemoveActivity(const std::string &plugin);
void MarkActivityCancelling(const std::string &plugin);
bool PluginStorageBusy();
bool IsReviewablePackage(const std::string &path);
bool SetActivityFile(const std::string &plugin, uint32_t id,
                     const std::string &path, std::string &error);

// The outermost storage operation owns the lease; recursive native workflows
// on the same worker thread reuse it. Registration and lease acquisition share
// one mutex, preventing a download/start-format race.
class HostStorageLease final {
 public:
  HostStorageLease();
  ~HostStorageLease();
  explicit operator bool() const { return acquired_; }
  HostStorageLease(const HostStorageLease &) = delete;
  HostStorageLease &operator=(const HostStorageLease &) = delete;
 private:
  bool acquired_ = false;
};
}  // namespace aeraui::plugin_api
