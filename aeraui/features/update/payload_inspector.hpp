/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aeraui::payload {

struct Partition {
  std::string name;
  uint64_t bytes = 0;
  uint64_t operations = 0;
};

struct Info {
  bool is_payload = false;
  bool valid = false;
  bool incremental = false;
  bool partial = false;
  bool dynamic_partitions = false;
  bool snapshots = false;
  bool virtual_ab_compression = false;
  uint64_t payload_bytes = 0;
  uint64_t expanded_bytes = 0;
  uint64_t operations = 0;
  uint64_t max_timestamp = 0;
  uint32_t format_version = 0;
  uint32_t minor_version = 0;
  std::string target_device;
  std::string target_build;
  std::string build_id;
  std::string device_codename;
  bool name_from_filename = false;
  std::string system_fingerprint;
  std::string target_sdk;
  std::string security_patch;
  std::string operation_types;
  std::string error;
  std::vector<Partition> partitions;
};

// Reads only the ZIP directory, OTA metadata and the payload manifest. Payload
// operation data is never read and no device state is changed.
Info InspectZip(const std::string &path);

// Compact, display-ready preflight summary for the install confirmation.
std::string Summary(const Info &info);

}  // namespace aeraui::payload
