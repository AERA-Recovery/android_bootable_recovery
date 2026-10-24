/* SPDX-License-Identifier: Apache-2.0 */
#include "font_library.hpp"
#include "../../localization/fonts/custom_font_storage.hpp"
#include <json/json.h>
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace aeraui::fonts {
namespace {
bool Plain(const std::string& text, size_t limit, bool empty = false) {
  return (empty || !text.empty()) && text.size() <= limit &&
      std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool Valid(const SavedFont& font) {
  return Plain(font.family, 160) && Plain(font.provider_id, 100, true) &&
      Plain(font.category, 32, true) && Plain(font.license, 128, true) &&
      Plain(font.subset, 48) && (font.style == "normal" || font.style == "italic") &&
      font.weight >= 1 && font.weight <= 1000 && Plain(font.url, 1024, true);
}
}  // namespace

bool FontLibrary::Find(const std::string& id, SavedFont& font) const {
  struct stat directory_info{};
  if (lstat(directory_.c_str(), &directory_info) != 0 || !S_ISDIR(directory_info.st_mode)) return false;
  CustomFontStorage storage(directory_);
  if (!storage.Has(id)) return false;
  const std::string path = directory_ + "/" + id + ".json";
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat info{};
  bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 8192;
  std::string bytes;
  if (ok) {
    bytes.resize(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = read(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { ok = false; break; }
      offset += count;
    }
  }
  close(fd);
  if (!ok) return false;
  Json::CharReaderBuilder builder;
  builder["rejectDupKeys"] = true;
  builder["failIfExtra"] = true;
  builder["stackLimit"] = 8;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string error;
  if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &root, &error) || !root.isObject()) return false;
  for (const auto* key : {"id", "family", "provider", "category", "license", "subset", "style", "url"})
    if (!root[key].isString()) return false;
  if (!root["schema"].isInt() || root["schema"].asInt() != 1 ||
      root["id"].asString() != id || !root["weight"].isInt()) return false;
  SavedFont result;
  result.id = id; result.path = storage.Path(id);
  result.family = root["family"].asString(); result.provider_id = root["provider"].asString();
  result.category = root["category"].asString(); result.license = root["license"].asString();
  result.subset = root["subset"].asString(); result.style = root["style"].asString();
  result.url = root["url"].asString(); result.weight = root["weight"].asInt();
  if (!Valid(result)) return false;
  font = std::move(result);
  return true;
}

std::vector<SavedFont> FontLibrary::List() const {
  std::vector<SavedFont> fonts;
  DIR* directory = opendir(directory_.c_str());
  if (!directory) return fonts;
  while (auto* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.size() != 68 || name.compare(63, 5, ".json")) continue;
    SavedFont font;
    if (Find(name.substr(0, 63), font)) fonts.push_back(std::move(font));
  }
  closedir(directory);
  std::sort(fonts.begin(), fonts.end(), [](const SavedFont& a, const SavedFont& b) {
    if (a.family != b.family) return a.family < b.family;
    if (a.weight != b.weight) return a.weight < b.weight;
    return a.style != b.style ? a.style < b.style : a.id < b.id;
  });
  return fonts;
}

bool FontLibrary::Save(const std::string& source, const SavedFont& metadata,
                       std::string& id, std::string* error) const {
  id.clear();
  auto fail = [error] { if (error) *error = "Could not save the custom font."; return false; };
  if (!Valid(metadata)) return fail();
  CustomFontStorage storage(directory_);
  const auto existing = storage.List();
  const auto imported = storage.Import(source, error);
  if (imported.empty()) return false;
  SavedFont previous;
  if (Find(imported, previous)) { id = imported; return true; }
  Json::Value root;
  root["schema"] = 1; root["id"] = imported;
  root["family"] = metadata.family; root["provider"] = metadata.provider_id;
  root["category"] = metadata.category; root["license"] = metadata.license;
  root["subset"] = metadata.subset; root["style"] = metadata.style;
  root["url"] = metadata.url; root["weight"] = metadata.weight;
  const std::string bytes = Json::writeString(Json::StreamWriterBuilder{}, root);
  std::string temporary = directory_ + "/.metadata-XXXXXX";
  const int fd = mkstemp(temporary.data());
  bool ok = fd >= 0;
  if (fd >= 0) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { ok = false; break; }
      offset += count;
    }
    if (fchmod(fd, 0660) != 0 || fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(temporary.c_str(), (directory_ + "/" + imported + ".json").c_str()) != 0) ok = false;
  }
  if (!ok) {
    unlink(temporary.c_str());
    if (std::find(existing.begin(), existing.end(), imported) == existing.end()) unlink(storage.Path(imported).c_str());
    return fail();
  }
  id = imported;
  return true;
}

bool FontLibrary::Remove(const std::string& id, std::string* error) const {
  SavedFont font;
  if (!Find(id, font) || unlink(font.path.c_str()) != 0) {
    if (error) *error = "Could not delete the font.";
    return false;
  }
  unlink((directory_ + "/" + id + ".json").c_str());
  return true;
}
}
