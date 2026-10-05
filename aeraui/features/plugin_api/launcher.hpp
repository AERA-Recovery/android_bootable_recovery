/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace aeraui::plugin_api {
// Host API 3 additions. The defaults launch a Host API 2 plugin exactly as
// before.
struct LaunchOptions {
  uint32_t host_api = 2;
  // Host API 3's pixel surface, handed to the plugin as fd 3.
  int surface_fd = -1;
  // A persistent private directory, exported as AERA_PLUGIN_DATA.
  std::string data_dir;
  // data_dir is in RAM (storage was not mounted): exported as
  // AERA_PLUGIN_DATA_VOLATILE=1, so an app can warn that nothing it saves
  // survives a reboot.
  bool data_volatile = false;
};

class Process final {
 public:
  ~Process() { Stop(); }
  Process() = default;
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;
  bool Start(const std::string &runtime, int &control_fd, std::string &error,
             const LaunchOptions &options = {});
  bool Running();
  void Stop();
 private:
  pid_t pid_ = -1;
};
}  // namespace aeraui::plugin_api
