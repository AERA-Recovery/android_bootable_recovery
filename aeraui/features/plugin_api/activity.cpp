/* SPDX-License-Identifier: Apache-2.0 */
#include "activity.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <time.h>

namespace aeraui::plugin_api {
namespace {
std::mutex gMutex;
std::map<std::string, Activity> gActivities;
bool gStorageLease = false;
thread_local unsigned gStorageDepth = 0;
uint64_t NowMs() {
  timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
bool UsesStorage(const Activity &activity) {
  return activity.Active() && !activity.storage.empty() &&
      activity.storage != "/tmp" && activity.storage.compare(0, 5, "/tmp/") != 0;
}
bool StorageBusyLocked() {
  return std::any_of(gActivities.begin(), gActivities.end(),
      [](const auto &entry) { return UsesStorage(entry.second); });
}
}
bool SetActivity(const std::string &plugin, const std::string &name,
                 const std::string &storage, const Message &message,
                 std::string &error) {
  constexpr uint32_t mask = kActivityActive | kActivityCancellable |
      kActivityComplete | kActivityFailed;
  const uint32_t state = message.flags &
      (kActivityActive | kActivityComplete | kActivityFailed);
  if (!message.request_id || !message.title[0] || (message.flags & ~mask) ||
      !state || (state & (state - 1)) ||
      ((message.flags & kActivityCancellable) && state != kActivityActive) ||
      (message.value > 10000 && message.value != kProgressUnknown) ||
      (!storage.empty() && (storage[0] != '/' || storage.find('\n') != std::string::npos))) {
    error = "Invalid plugin activity"; return false;
  }
  std::lock_guard<std::mutex> lock(gMutex);
  auto found = gActivities.find(plugin);
  const bool existing = found != gActivities.end() && found->second.Active();
  if (existing && (found->second.id != message.request_id ||
                  found->second.storage != storage)) {
    error = "Finish the current plugin activity first"; return false;
  }
  Activity next{plugin, name, message.title, message.text, storage,
      message.request_id, message.value, message.flags, NowMs(),
      existing && state == kActivityActive && found->second.cancelling, {}};
  if (found != gActivities.end() && found->second.id == message.request_id)
    next.result_file = found->second.result_file;
  if (UsesStorage(next) && gStorageLease) {
    error = "A recovery operation is using storage"; return false;
  }
  if (found == gActivities.end() && gActivities.size() >= 8) {
    error = "Too many plugin activities"; return false;
  }
  gActivities[plugin] = std::move(next);
  error.clear(); return true;
}
std::vector<Activity> Activities() {
  std::lock_guard<std::mutex> lock(gMutex);
  const uint64_t now = NowMs();
  for (auto i = gActivities.begin(); i != gActivities.end();) {
    if (!i->second.Active() && i->second.result_file.empty() && now - i->second.updated_ms > 8000)
      i = gActivities.erase(i);
    else ++i;
  }
  std::vector<Activity> result;
  for (const auto &entry : gActivities) result.push_back(entry.second);
  return result;
}
void RemoveActivity(const std::string &plugin) {
  std::lock_guard<std::mutex> lock(gMutex); gActivities.erase(plugin);
}
void MarkActivityCancelling(const std::string &plugin) {
  std::lock_guard<std::mutex> lock(gMutex);
  auto found = gActivities.find(plugin);
  if (found != gActivities.end()) found->second.cancelling = true;
}
bool PluginStorageBusy() {
  std::lock_guard<std::mutex> lock(gMutex); return StorageBusyLocked();
}
bool IsReviewablePackage(const std::string &path) {
  if (path.empty() || path[0] != '/' || path.find('\n') != std::string::npos || path.size() < 5)
    return false;
  std::string suffix = path.substr(path.size() - 4);
  std::transform(suffix.begin(), suffix.end(), suffix.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  struct stat info{};
  return suffix == ".zip" && lstat(path.c_str(), &info) == 0 &&
      S_ISREG(info.st_mode) && info.st_size > 0;
}
bool SetActivityFile(const std::string &plugin, uint32_t id,
                     const std::string &path, std::string &error) {
  if (!IsReviewablePackage(path)) { error = "The downloaded ZIP is unavailable"; return false; }
  std::lock_guard<std::mutex> lock(gMutex);
  auto found = gActivities.find(plugin);
  if (found == gActivities.end() || found->second.id != id || !found->second.Active()) {
    error = "No matching active plugin task"; return false;
  }
  char root[PATH_MAX], file[PATH_MAX];
  if (found->second.storage.empty() || !realpath(found->second.storage.c_str(), root) ||
      !realpath(path.c_str(), file) || std::string(file).compare(0,
          std::string(root).size() + 1, std::string(root) + "/") != 0) {
    error = "The downloaded ZIP is outside the task storage"; return false;
  }
  found->second.result_file = path;
  error.clear(); return true;
}
HostStorageLease::HostStorageLease() {
  if (gStorageDepth) { ++gStorageDepth; acquired_ = true; return; }
  std::lock_guard<std::mutex> lock(gMutex);
  if (gStorageLease || StorageBusyLocked()) return;
  gStorageLease = true; gStorageDepth = 1; acquired_ = true;
}
HostStorageLease::~HostStorageLease() {
  if (!acquired_ || --gStorageDepth) return;
  std::lock_guard<std::mutex> lock(gMutex); gStorageLease = false;
}
}  // namespace aeraui::plugin_api
