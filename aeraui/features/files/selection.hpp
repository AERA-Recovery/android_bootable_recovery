/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace aeraui {
struct FilePickerOptions {
  std::string directory;
  std::vector<std::string> extensions;
  bool folders = false;
};
enum class FilePickerStatus { kSelected, kCancelled };
struct FilePickerResult {
  FilePickerStatus status = FilePickerStatus::kCancelled;
  std::string path;
};
using FilePickerCallback = std::function<void(const FilePickerResult&)>;

inline bool FilePickerMatches(const FilePickerOptions& options, const std::string& path) {
  if (options.folders) return false;
  if (options.extensions.empty()) return true;
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto& extension : options.extensions)
    if (lower.size() >= extension.size() &&
        lower.compare(lower.size() - extension.size(), extension.size(), extension) == 0)
      return true;
  return false;
}
}  // namespace aeraui
