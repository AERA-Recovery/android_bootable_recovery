/* SPDX-License-Identifier: Apache-2.0 */
#include "fontsource.hpp"

#include <json/json.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>

namespace aeraui::fontsource {
namespace {
bool Id(const std::string& text) {
  return !text.empty() && text.size() <= 100 &&
      std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      });
}
bool Text(const Json::Value& value, size_t limit = 160) {
  if (!value.isString()) return false;
  const auto text = value.asString();
  return !text.empty() && text.size() <= limit &&
      std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool Read(const std::string& text, Json::Value& root) {
  if (text.empty() || text.size() > 8 * 1024 * 1024) return false;
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["rejectDupKeys"] = true;
  builder["failIfExtra"] = true;
  builder["stackLimit"] = 24;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errors;
  return reader->parse(text.data(), text.data() + text.size(), &root, &errors);
}
bool ParseFamily(const Json::Value& value, Family& family) {
  if (!value.isObject() || !Text(value["id"], 100) || !Id(value["id"].asString()) ||
      !Text(value["family"]) || !Text(value["category"], 32) ||
      !value["subsets"].isArray() || value["subsets"].size() > 128) return false;
  family = {value["id"].asString(), value["family"].asString(),
            value["category"].asString(), {}};
  for (const auto& subset : value["subsets"])
    if (Text(subset, 48) && Id(subset.asString())) family.subsets.push_back(subset.asString());
  return !family.subsets.empty();
}
std::string Lower(std::string text) {
  for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return text;
}
}  // namespace

bool FontUrl(const std::string& url) {
  constexpr char prefix[] = "https://cdn.jsdelivr.net/fontsource/fonts/";
  if (url.compare(0, sizeof(prefix) - 1, prefix) || url.size() > 1024 ||
      url.size() < 4 || url.compare(url.size() - 4, 4, ".ttf")) return false;
  return std::none_of(url.begin(), url.end(), [](unsigned char c) {
    return c <= 32 || c >= 127 || c == '\\' || c == '#' || c == '?';
  }) && url.find("/../") == std::string::npos;
}
bool TtfBytes(const std::string& bytes) {
  return bytes.size() >= 12 &&
      (bytes.compare(0, 4, std::string("\0\1\0\0", 4)) == 0 ||
       bytes.compare(0, 4, "OTTO") == 0 || bytes.compare(0, 4, "true") == 0);
}
bool ParseCatalog(const std::string& text, std::vector<Family>& families, std::string& error) {
  Json::Value root;
  if (!Read(text, root) || !root.isArray() || root.size() > 6000) {
    error = "Download failed"; return false;
  }
  std::map<std::string, Family> unique;
  for (const auto& value : root) {
    Family family;
    if (!ParseFamily(value, family)) continue;
    if (value["type"].isString() && value["type"].asString() == "icons") continue;
    unique.emplace(family.id, std::move(family));
  }
  std::vector<Family> result;
  for (auto& item : unique) result.push_back(std::move(item.second));
  std::sort(result.begin(), result.end(), [](const Family& a, const Family& b) {
    return Lower(a.name) < Lower(b.name);
  });
  if (result.empty()) { error = "Download failed"; return false; }
  families = std::move(result);
  return true;
}
bool ParseDetails(const std::string& text, const std::string& id, Details& details,
                  std::string& error) {
  Json::Value root;
  Details result;
  if (!Read(text, root) || !ParseFamily(root, result.family) || result.family.id != id ||
      !root["variants"].isObject()) {
    error = "Download failed"; return false;
  }
  if (Text(root["license"], 128)) result.license = root["license"].asString();
  const auto& variants = root["variants"];
  for (const auto& weight : variants.getMemberNames()) {
    if (weight.empty() || weight.size() > 4 ||
        !std::all_of(weight.begin(), weight.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
    const int number = atoi(weight.c_str());
    if (number < 1 || number > 1000 || !variants[weight].isObject()) continue;
    for (const auto& style : {"normal", "italic"}) {
      const auto& subsets = variants[weight][style];
      if (!subsets.isObject()) continue;
      for (const auto& subset : subsets.getMemberNames()) {
        if (!Id(subset)) continue;
        const auto& record = subsets[subset];
        if (!record.isObject() || !record["url"].isObject() || !record["url"]["ttf"].isString()) continue;
        auto url = record["url"]["ttf"].asString();
        if (!FontUrl(url)) continue;
        // Pin the version reported by this response, rather than a moving latest URL.
        const auto version = root["npmVersion"].isString() ? root["npmVersion"].asString() : std::string{};
        const auto latest = url.find("@latest/");
        if (latest != std::string::npos && !version.empty() && version.size() < 40 &&
            std::all_of(version.begin(), version.end(), [](char c) {
              return (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                  (c >= 'a' && c <= 'z');
            })) url.replace(latest, 8, "@" + version + "/");
        result.faces.push_back({number, style, subset, url});
        if (result.faces.size() > 4096) {
          error = "Download failed"; return false;
        }
      }
    }
  }
  if (result.faces.empty()) { error = "Font unavailable"; return false; }
  details = std::move(result);
  return true;
}

std::vector<Face> FacesForLanguage(const Details& details, const std::string& language) {
  std::string base = Lower(language.substr(0, language.find_first_of("_-")));
  static const std::map<std::string, std::string> scripts{
      {"ru", "cyrillic"}, {"uk", "cyrillic"}, {"bg", "cyrillic"}, {"sr", "cyrillic"},
      {"el", "greek"}, {"ar", "arabic"}, {"fa", "arabic"}, {"he", "hebrew"},
      {"hi", "devanagari"}, {"bn", "bengali"}, {"th", "thai"}, {"vi", "vietnamese"},
      {"ja", "japanese"}, {"ko", "korean"}, {"zh", "chinese-simplified"}};
  std::vector<std::string> preferred;
  const auto script = scripts.find(base);
  if (script != scripts.end()) {
    preferred.push_back(script->second);
    if (script->second == "cyrillic-ext") preferred.push_back("cyrillic");
  }
  preferred.push_back("latin");
  preferred.push_back("latin-ext");
  if (!details.faces.empty()) preferred.push_back(details.faces.front().subset);
  for (const auto& subset : preferred) {
    std::vector<Face> result;
    for (const auto& face : details.faces) if (face.subset == subset) result.push_back(face);
    if (result.empty()) continue;
    std::sort(result.begin(), result.end(), [](const Face& a, const Face& b) {
      const int x = (a.style == "italic" ? 2000 : 0) + std::abs(a.weight - 400);
      const int y = (b.style == "italic" ? 2000 : 0) + std::abs(b.weight - 400);
      return x != y ? x < y : a.weight < b.weight;
    });
    return result;
  }
  return {};
}
std::vector<size_t> Filter(const std::vector<Family>& families, const std::string& query,
                           const std::string& category) {
  std::vector<size_t> result;
  const auto needle = Lower(query);
  for (size_t i = 0; i < families.size(); ++i)
    if ((category.empty() || category == families[i].category) &&
        (Lower(families[i].name).find(needle) != std::string::npos ||
         families[i].id.find(needle) != std::string::npos)) result.push_back(i);
  return result;
}
std::string Sample(const std::string& subset) {
  if (subset.find("cyrillic") == 0) return "Съешь ещё этих мягких французских булок 0123456789";
  if (subset.find("greek") == 0) return "Αλφάβητο και αριθμοί 0123456789";
  if (subset == "arabic") return "أبجدية جميلة 0123456789";
  if (subset == "hebrew") return "אותיות ומספרים 0123456789";
  if (subset == "devanagari") return "सुन्दर अक्षर 0123456789";
  if (subset == "bengali") return "সুন্দর অক্ষর 0123456789";
  if (subset == "thai") return "ตัวอักษรสวยงาม 0123456789";
  if (subset == "vietnamese") return "Chữ viết tiếng Việt 0123456789";
  if (subset == "japanese") return "美しい文字 0123456789";
  if (subset == "korean") return "아름다운 글자 0123456789";
  if (subset.find("chinese") == 0) return "美丽的文字 0123456789";
  return "The quick brown fox 0123456789";
}
}  // namespace aeraui::fontsource
