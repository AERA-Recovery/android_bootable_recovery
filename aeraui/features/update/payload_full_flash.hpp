/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "payload_inspector.hpp"
#include <aeraui/backend.hpp>
#include <algorithm>
#include <set>

namespace aeraui::payload {
// Whole-payload flashing must never degrade into "all available images".
inline bool PlanFullFlash(const Info& info, const std::vector<PayloadFlashTarget>& targets,
                         std::vector<std::string>& names, std::string& error) {
  names.clear();
  // partial_update describes partition coverage, not a dependency on old data.
  // A partial-coverage package may still contain complete standalone images.
  if (!info.valid || !info.is_payload || info.incremental || info.partitions.empty()) {
    error = "Fast flash requires a full, non-incremental OTA. Use normal installation.";
    return false;
  }
  std::vector<std::string> planned;
  std::set<std::string> unique_names, unique_paths;
  std::set<uint64_t> raw_devices;
  for (const auto& p : info.partitions) {
    if (!p.extractable || !p.bytes || p.sha256.size() != 32 || !unique_names.insert(p.name).second) {
      error = "Fast flash cannot process " + p.name + ". Use normal installation.";
      return false;
    }
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& t) { return t.name == p.name; });
    if (target == targets.end() || target->path.empty() ||
        (target->raw && (p.bytes > target->bytes || !target->device)) ||
        !unique_paths.insert(target->path).second ||
        (target->raw && !raw_devices.insert(target->device).second)) {
      error = "No safe flash target for " + p.name + ". Nothing will be skipped; use normal installation.";
      return false;
    }
    planned.push_back(p.name);
  }
  names = std::move(planned);
  return true;
}
}  // namespace aeraui::payload
