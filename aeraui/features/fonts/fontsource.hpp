/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace aeraui::fontsource {
struct Family {
  std::string id, name, category;
  std::vector<std::string> subsets;
};
struct Face {
  int weight = 400;
  std::string style, subset, url;
};
struct Details {
  Family family;
  std::string license;
  std::vector<Face> faces;
};
using Fetch = std::function<bool(const std::string&, size_t, std::atomic<bool>&,
                                std::string&, std::string&)>;
bool Download(const std::string& url, size_t limit, std::atomic<bool>& cancel,
              std::string& bytes, std::string& error);
bool ParseCatalog(const std::string& json, std::vector<Family>& families, std::string& error);
bool ParseDetails(const std::string& json, const std::string& id, Details& details,
                  std::string& error);
std::vector<Face> FacesForLanguage(const Details& details, const std::string& language);
std::vector<size_t> Filter(const std::vector<Family>& families, const std::string& query,
                           const std::string& category);
bool FontUrl(const std::string& url);
bool TtfBytes(const std::string& bytes);
std::string Sample(const std::string& subset);
}  // namespace aeraui::fontsource
