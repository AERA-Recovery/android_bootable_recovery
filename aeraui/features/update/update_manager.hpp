/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aeraui::update {

enum class Channel {
  kStable,
  kNightly,
};

inline const char* ChannelBuildType(Channel channel) {
  return channel == Channel::kNightly ? "Nightly" : "Stable";
}

enum class Phase {
  kIdle,
  kChecking,
  kAvailable,
  kUpToDate,
  kDownloading,
  kVerifying,
  kReady,
  kError,
};

struct Release {
  std::string version;
  std::string build_type;
  uint64_t build_time = 0;
  std::string filename;
  uint64_t size = 0;
  std::string sha256;
  std::string url;
  std::vector<std::string> changelog;
};

struct Snapshot {
  Phase phase = Phase::kIdle;
  std::string device;
  uint64_t local_build_time = 0;
  std::string local_build_type;
  Channel channel = Channel::kStable;
  Release release;
  std::string package_path;
  std::string message;
  uint64_t downloaded = 0;
  uint64_t total = 0;
  unsigned progress = 0;
  bool checked = false;
  // Notifications only describe newer builds. Older/equal releases are only
  // installable when switching away from the installed build's channel.
  bool available = false;
  bool installable = false;

  bool IsChannelSwitch() const {
    if (local_build_type == "Nightly") return channel != Channel::kNightly;
    // Published Alpha/Beta releases and normal /aera test builds belong to
    // the stable OTA channel; build maturity is not a separate update channel.
    if (local_build_type == "Stable" || local_build_type == "Beta" ||
        local_build_type == "Alpha") return channel != Channel::kStable;
    return false;
  }

  bool CanDownload() const {
    return checked && installable && release.build_time != 0 && release.size != 0 &&
        (release.build_time > local_build_time || IsChannelSwitch());
  }
};

const char *CatalogUrl();
Channel GetChannel();
bool SetChannel(Channel channel);
Snapshot GetSnapshot();
bool Check();
bool PrepareDownload();
bool Download();
void SetOffline();
void Cancel();
void MarkInstalled();

}  // namespace aeraui::update
