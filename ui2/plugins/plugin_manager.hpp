/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace recovery_ui2::plugins {

constexpr uint32_t kHostApi = 1;

enum class Location {
  kNone = 0,
  kStorage,
  kMemory,
};

enum class Job {
  kRefresh = 0,
  kInstallStorage,
  kInstallMemory,
  kRemove,
};

struct Plugin {
  std::string id;
  std::string name;
  std::string version;
  std::string description;
  std::string type;
  std::string entry;
  std::string manifest_url;
  std::string signature_url;
  std::string payload_url;
  std::string payload_name;
  std::string payload_sha256;
  uint64_t payload_size = 0;
  uint64_t expanded_size = 0;
  std::string expanded_sha256;
  uint32_t member_count = 0;
  uint32_t min_host_api = 0;
  Location location = Location::kNone;
};

struct Request {
  Job job = Job::kRefresh;
  std::string id;
};

struct Progress {
  std::atomic<unsigned> value{0};
  std::atomic<uint64_t> downloaded_bytes{0};
  std::atomic<uint64_t> total_bytes{0};
  std::atomic<bool> cancel{false};
  std::string status;
  std::string error;
};

std::vector<Plugin> Catalog();
std::vector<Plugin> Installed();
bool FindInstalled(const std::string &id, Plugin &plugin);
bool Run(const Request &request, Progress &progress);

// Revalidates the signed manifest and returns the payload path. The large
// payload hash is checked by its consumer immediately before extraction/use.
bool ResolvePayload(const std::string &id, Plugin &plugin, std::string &path,
                    std::string &error);

const char *LocationLabel(Location location);

}  // namespace recovery_ui2::plugins
