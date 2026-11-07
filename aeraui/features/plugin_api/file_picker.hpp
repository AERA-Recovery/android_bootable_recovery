/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "protocol.hpp"
#include "../files/selection.hpp"

namespace aeraui::plugin_api {
inline bool ParseFilePickerRequest(const Message& message, FilePickerOptions& options,
                                   std::string& error) {
  options = {};
  if (!StringsValid(message) || message.kind != Kind::kPickFile ||
      !message.request_id || message.value > 1 || message.flags) {
    error = "Invalid file selection request."; return false;
  }
  options.folders = message.value == 1;
  options.directory = message.text;
  if ((!options.directory.empty() && options.directory.front() != '/') ||
      std::any_of(options.directory.begin(), options.directory.end(),
                  [](unsigned char c) { return c < 32 || c == 127; })) {
    error = "The starting folder must be an absolute path."; return false;
  }
  const std::string filters = message.title;
  if (options.folders && !filters.empty()) {
    error = "Folder selection cannot have file filters."; return false;
  }
  for (size_t offset = 0; offset < filters.size();) {
    size_t end = filters.find(',', offset);
    if (end == std::string::npos) end = filters.size();
    std::string extension = filters.substr(offset, end - offset);
    if (extension.size() < 2 || extension.front() != '.' ||
        extension.size() > 32 || options.extensions.size() >= 16 ||
        !std::all_of(extension.begin() + 1, extension.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        }) || (end < filters.size() && end + 1 == filters.size())) {
      error = "Invalid file extension filter."; return false;
    }
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    options.extensions.push_back(std::move(extension));
    offset = end + 1;
  }
  return true;
}
}  // namespace aeraui::plugin_api
