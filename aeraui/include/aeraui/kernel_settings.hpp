/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace aeraui::kernel {
struct Setting {
  std::string id, group, device, name, path, current, baseline, saved, status;
  std::vector<std::string> choices;
  int64_t minimum = 0, maximum = 0;
  bool writable = false;
};
using Overrides = std::map<std::string, std::string>;
std::string Encode(const Overrides &values);
bool Decode(const std::string &text, Overrides *values);
class Settings {
 public:
  explicit Settings(std::string root = "");
  std::vector<Setting> Discover();
  bool Apply(const std::string &id, const std::string &value, std::string *error);
  void Load(const Overrides &values);
  void Poll();
  const Overrides &Saved() const { return saved_; }
  void Remember(const std::string &id, const std::string &value);
  void SessionOnly(const std::string &id) { session_.insert(id); }
  bool Reset(const std::string &id, std::string *error);
  bool ResetAll(std::string *error);
  void Skip() { skipped_ = true; }
  bool Skipped() const { return skipped_; }
 private:
  std::string Read(const std::string &path) const;
  bool Write(const std::string &path, const std::string &value, std::string *error);
  bool ApplyKnown(const std::vector<Setting> &settings, const std::string &id,
                  const std::string &value, std::string *error);
  std::string root_;
  std::map<std::string, std::string> baselines_, applied_, errors_;
  Overrides saved_;
  std::set<std::string> session_;
  bool skipped_ = false;
};
}  // namespace aeraui::kernel
