/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "aeraui/features/backup/android_packages_xml.hpp"
#include <dlfcn.h>
#include <cstdint>
#include <sys/stat.h>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace aera::mobile {
namespace android_apn {
inline std::string Read(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  std::string bytes;
  char buffer[4096];
  while (input && bytes.size() <= 1024 * 1024) {
    input.read(buffer, sizeof(buffer));
    bytes.append(buffer, input.gcount());
  }
  return bytes.size() <= 1024 * 1024 ? bytes : std::string{};
}
inline int Number(const char *text) {
  if (!text || !*text) return -1;
  int result = 0;
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9' || result > (INT32_MAX - (*p - '0')) / 10) return -1;
    result = result * 10 + (*p - '0');
  }
  return result;
}
// No new SQLite payload: use the optional library already in recovery. The
// immutable/read-only URI neither creates a journal nor touches Android's DB.
struct Database {
  struct Sqlite;
  void *library = nullptr;
  Sqlite *db = nullptr;
  int (*close)(Sqlite *) = nullptr;
  int (*exec)(Sqlite *, const char *, int (*)(void *, int, char **, char **), void *, char **) = nullptr;
  ~Database() {
    if (db && close) close(db);
    if (library) dlclose(library);
  }
  bool Open(const std::string &path) {
    struct stat wal{};
    // Immutable reads deliberately ignore WAL. Refuse a pending WAL instead
    // of using an out-of-date subscription mapping or writing a shared index.
    if (stat((path + "-wal").c_str(), &wal) == 0 && wal.st_size != 0) return false;
    library = dlopen("libsqlite.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) return false;
    auto open = reinterpret_cast<int (*)(const char *, Sqlite **, int, const char *)>(dlsym(library, "sqlite3_open_v2"));
    close = reinterpret_cast<decltype(close)>(dlsym(library, "sqlite3_close"));
    exec = reinterpret_cast<decltype(exec)>(dlsym(library, "sqlite3_exec"));
    // SQLITE_OPEN_READONLY | SQLITE_OPEN_URI.
    return open && close && exec && open(("file:" + path + "?mode=ro&immutable=1").c_str(), &db, 0x41, nullptr) == 0;
  }
};
}  // namespace android_apn

struct AndroidApn {
  std::string apn;
  int subscription = 0;
};
inline AndroidApn AndroidPreferredApn(int subscription, const std::string &root = "") {
  using namespace android_apn;
  if (subscription < 0 || subscription > 2) return {};
  const std::string base = root + "/data/user_de/0/com.android.providers.telephony/";
  Database database;
  if (!database.Open(base + "databases/telephony.db")) return {};
  std::vector<std::pair<int, int>> active;
  if (database.exec(database.db, "SELECT _id,sim_id FROM siminfo WHERE sim_id>=0", [](void *context, int count, char **values, char **) {
        if (count != 2) return 1;
        const int id = Number(values[0]), slot = Number(values[1]);
        if (id >= 0 && slot >= 0) static_cast<std::vector<std::pair<int, int>> *>(context)->emplace_back(id, slot);
        return 0;
      }, &active, nullptr) != 0) return {};
  int preferred = -1;
  if (subscription == 0) {
    tinyxml2::XMLDocument settings;
    std::string error;
    if (aeraui::appvault::ParseSettingsXml(Read(root + "/data/system/users/0/settings_global.xml"), settings, error)) {
      auto *map = settings.FirstChildElement("settings");
      if (!map) return {};
      for (auto *item = map->FirstChildElement("setting"); item; item = item->NextSiblingElement("setting")) {
        const char *name = item->Attribute("name");
        if (name && std::string(name) == "multi_sim_data_call") preferred = Number(item->Attribute("value"));
      }
    }
  }
  int selected = -1, selected_slot = -1;
  for (const auto &sim : active) {
    const bool matches = subscription ? sim.second == subscription - 1 : preferred >= 0 ? sim.first == preferred : active.size() == 1;
    if (matches) {
      if (selected >= 0) return {};  // Ambiguous: leave selection to the user/modem.
      selected = sim.first;
      selected_slot = sim.second;
    }
  }
  if (selected < 0) return {};
  tinyxml2::XMLDocument preferences;
  std::string error;
  if (!aeraui::appvault::ParsePackagesXml(Read(base + "shared_prefs/preferred-full-apn.xml"), preferences, error)) return {};
  const std::string key = "apn" + std::to_string(selected);
  auto *map = preferences.RootElement();
  if (!map || std::string(map->Name()) != "map") return {};
  for (auto *item = map->FirstChildElement("string"); item; item = item->NextSiblingElement("string")) {
    const char *name = item->Attribute("name"), *text = item->GetText();
    if (!name || name != key || !text) continue;
    const std::string apn(text);
    if (!apn.empty() && apn.size() <= 100 && apn.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") == std::string::npos &&
        selected_slot >= 0 && selected_slot < 2) return {apn, selected_slot + 1};
  }
  return {};
}
}  // namespace aera::mobile
