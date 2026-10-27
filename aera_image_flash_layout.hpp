/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <liblp/builder.h>
#include <set>
#include <string>
#include <vector>

namespace aera {
struct LogicalImageSize {
  std::string name;
  uint64_t bytes;
};

// Plan only: callers publish Export() after every allocation succeeds. Keep
// unselected partitions, group limits and existing extents intact. Shrinking
// all selected partitions first lets a full group exchange space between them.
inline bool PlanImageFlashLayout(android::fs_mgr::MetadataBuilder& builder,
        const std::vector<LogicalImageSize>& images, const std::vector<std::string>& stale_cows,
        std::string& error) {
  std::set<std::string> names;
  for (const auto& image : images) {
    if (!image.bytes || image.bytes % 512 || !names.insert(image.name).second ||
        !builder.FindPartition(image.name)) {
      error = "Invalid or duplicate logical target: " + image.name;
      return false;
    }
  }
  for (const auto& name : stale_cows) {
    const auto* part = builder.FindPartition(name);
    if (!part || part->group_name() != "cow" || name.size() <= 4 ||
        name.compare(name.size() - 4, 4, "-cow") || names.count(name)) {
      error = "Invalid stale snapshot COW: " + name;
      return false;
    }
  }
  for (const auto& name : stale_cows) builder.RemovePartition(name);
  for (bool shrink : {true, false}) {
    for (const auto& image : images) {
      auto* part = builder.FindPartition(image.name);
      if (part->size() == image.bytes || (image.bytes < part->size()) != shrink) continue;
      if (!builder.ResizePartition(part, image.bytes)) {
        error = "Cannot fit " + image.name + " into its Super group after selected shrinks and stale-COW cleanup";
        return false;
      }
    }
  }
  return true;
}
} // namespace aera
