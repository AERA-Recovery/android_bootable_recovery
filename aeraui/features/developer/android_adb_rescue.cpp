/* SPDX-License-Identifier: Apache-2.0 */
#include "aeraui/android_adb_rescue.hpp"
#include "../backup/android_packages_xml.hpp"
#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
namespace aeraui::adb_rescue {
namespace {
constexpr size_t kLimit = 32U << 20;
constexpr const char *kBackup = "system/aera-adb-rescue";
std::mutex mutex;
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
void Require(bool ok, const std::string &message) { if (!ok) throw Failure(message); }
struct Fd {
  int n = -1;
  explicit Fd(int fd = -1) : n(fd) {}
  ~Fd() { if (n >= 0) close(n); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};
std::string Hex(const unsigned char *p, size_t n) {
  static const char h[] = "0123456789abcdef"; std::string out;
  for (size_t i = 0; i < n; ++i) { out += h[p[i] >> 4]; out += h[p[i] & 15]; }
  return out;
}
std::string Hash(const std::string &s) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(s.data()), s.size(), digest);
  return Hex(digest, sizeof(digest));
}
std::string Leaf(const std::string &s) { return s.substr(s.find_last_of('/') + 1); }
std::string Parent(const std::string &s) { return s.substr(0, s.find_last_of('/')); }
// Do not follow symlinks in database paths or our rollback journal.
int OpenDir(const std::string &root, const std::string &relative, bool create = false, bool optional = false) {
  Fd current(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  Require(current.n >= 0, "Android data is not mounted or readable.");
  std::istringstream parts(relative); std::string part;
  while (std::getline(parts, part, '/')) {
    Require(!part.empty() && part != "." && part != "..", "Invalid Android database path.");
    int next = openat(current.n, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0 && create && errno == ENOENT) {
      Require(mkdirat(current.n, part.c_str(), 0700) == 0, "Could not create rescue backup directory.");
      Require(fsync(current.n) == 0, "Could not synchronize rescue backup directory.");
      next = openat(current.n, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (next < 0 && optional && errno == ENOENT) return -1;
    Require(next >= 0, "Android database directory is unavailable: " + relative);
    close(current.n); current.n = next;
  }
  int result = current.n; current.n = -1; return result;
}
struct File {
  bool exists = false;
  std::string path, bytes, label;
  uid_t uid = 0; gid_t gid = 0; mode_t mode = 0600;
};
File Read(const std::string &root, const std::string &path, bool optional = false) {
  File out; out.path = path; Fd dir(OpenDir(root, Parent(path), false, optional));
  if (dir.n < 0) return out;
  Fd fd(openat(dir.n, Leaf(path).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.n < 0 && optional && errno == ENOENT) return out;
  Require(fd.n >= 0, "Could not read Android file: " + path);
  struct stat st{};
  Require(fstat(fd.n, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size >= 0 &&
      static_cast<uint64_t>(st.st_size) <= kLimit, "Invalid or oversized Android file: " + path);
  out.exists = true; out.uid = st.st_uid; out.gid = st.st_gid; out.mode = st.st_mode & 07777;
  const ssize_t size = fgetxattr(fd.n, "security.selinux", nullptr, 0);
  if (size > 0) {
    Require(size <= 4096, "Invalid SELinux label."); out.label.resize(size);
    Require(fgetxattr(fd.n, "security.selinux", out.label.data(), size) == size, "Could not read SELinux label.");
  } else Require(size == 0 || errno == ENODATA || errno == ENOTSUP, "Could not read SELinux label.");
  out.bytes.resize(st.st_size); size_t done = 0;
  while (done < out.bytes.size()) {
    const ssize_t n = read(fd.n, out.bytes.data() + done, out.bytes.size() - done);
    if (n < 0 && errno == EINTR) continue;
    Require(n > 0, "Could not completely read Android file: " + path); done += n;
  }
  return out;
}
void Write(const std::string &root, const File &file) {
  Fd dir(OpenDir(root, Parent(file.path))); static unsigned counter = 0;
  const std::string temporary = ".aera-adb-" + std::to_string(getpid()) + "-" + std::to_string(++counter);
  Fd fd(openat(dir.n, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  Require(fd.n >= 0, "Could not stage Android file: " + file.path);
  try {
    size_t done = 0;
    while (done < file.bytes.size()) {
      const ssize_t n = write(fd.n, file.bytes.data() + done, file.bytes.size() - done);
      if (n < 0 && errno == EINTR) continue;
      Require(n > 0, "Could not write Android file: " + file.path); done += n;
    }
    // chown precedes chmod because chown can clear mode bits.
    Require(fchown(fd.n, file.uid, file.gid) == 0 && fchmod(fd.n, file.mode) == 0,
            "Could not preserve Android file ownership or permissions.");
    if (!file.label.empty()) Require(fsetxattr(fd.n, "security.selinux", file.label.data(), file.label.size(), 0) == 0,
                                   "Could not preserve Android SELinux label.");
    Require(fsync(fd.n) == 0, "Could not synchronize Android file.");
    Require(renameat(dir.n, temporary.c_str(), dir.n, Leaf(file.path).c_str()) == 0, "Could not replace Android file.");
    Require(fsync(dir.n) == 0, "Could not synchronize Android file directory.");
  } catch (...) { unlinkat(dir.n, temporary.c_str(), 0); throw; }
}
void Remove(const std::string &root, const std::string &path) {
  Fd dir(OpenDir(root, Parent(path)));
  Require(unlinkat(dir.n, Leaf(path).c_str(), 0) == 0 || errno == ENOENT, "Could not restore missing Android file.");
  Require(fsync(dir.n) == 0, "Could not synchronize Android file directory.");
}
// Keep original ABX scalar types, unknown fields and null attributes. Rebuild
// the intern table: inserting strings must not shift later string references.
struct Token { unsigned event = 0, type = 0; std::string name, value; };
struct Xml {
  bool binary = false;
  tinyxml2::XMLDocument doc;
  std::vector<Token> tokens;
  explicit Xml(const std::string &bytes) {
    std::string error;
    Require(appvault::ParseSettingsXml(bytes, doc, error), "Invalid or unsupported Android settings XML.");
    binary = bytes.compare(0, 4, "ABX\0", 4) == 0;
    if (!binary) return;
    size_t pos = 4; std::vector<std::string> strings;
    auto take = [&](size_t n) { Require(n <= bytes.size() - pos, "Truncated ABX data.");
      auto s = bytes.substr(pos, n); pos += n; return s; };
    auto number = [&](size_t n) { uint64_t x = 0; for (unsigned char b : take(n)) x = (x << 8) | b; return x; };
    auto utf = [&]() { const size_t n = number(2); return take(n); };
    auto intern = [&]() { const auto id = number(2);
      if (id == 65535) { strings.push_back(utf()); return strings.back(); }
      Require(id < strings.size(), "Invalid ABX string reference."); return strings[id]; };
    while (pos < bytes.size()) {
      const unsigned b = number(1); Token t; t.event = b & 15; t.type = b & 240;
      if (t.event == 15) t.name = intern();
      if (t.type == 0x30) t.value = intern();
      else if (t.type == 0x20 || t.type == 0x40 || t.type == 0x50) t.value = utf();
      else if (t.type == 0x60 || t.type == 0x70 || t.type == 0xa0) t.value = take(4);
      else if (t.type == 0x80 || t.type == 0x90 || t.type == 0xb0) t.value = take(8);
      else Require(t.type == 0x10 || t.type == 0xc0 || t.type == 0xd0, "Unsupported ABX value.");
      tokens.push_back(std::move(t));
    }
  }
  static std::string Value(const Token &t) {
    if (t.type == 0x20 || t.type == 0x30) return t.value;
    if (t.type == 0x10) return {};
    if (t.type == 0xc0) return "true";
    if (t.type == 0xd0) return "false";
    uint64_t n = 0; for (unsigned char b : t.value) n = (n << 8) | b;
    return std::to_string(t.value.size() == 4 ? static_cast<int64_t>(static_cast<int32_t>(n)) : static_cast<int64_t>(n));
  }
  using Attributes = std::map<std::string, std::string>;
  void Set(const std::string &root, const std::string &tag, const std::string &match,
           const std::string &value, const Attributes &attributes) {
    auto *parent = doc.FirstChildElement(root.c_str()); Require(parent, "Unexpected Android settings root.");
    tinyxml2::XMLElement *found = nullptr;
    for (auto *e = parent->FirstChildElement(tag.c_str()); e; e = e->NextSiblingElement(tag.c_str())) {
      const char *key = e->Attribute(match.c_str());
      if (key && key == value) { Require(!found, "Duplicate Android settings entry."); found = e; }
    }
    if (!binary) {
      if (!found) { found = doc.NewElement(tag.c_str()); parent->InsertEndChild(found); }
      found->SetAttribute(match.c_str(), value.c_str());
      for (const auto &a : attributes) found->SetAttribute(a.first.c_str(), a.second.c_str());
      return;
    }
    size_t begin = tokens.size(), end = begin, root_end = begin; int depth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
      if (tokens[i].event == 2) {
        if (depth == 1 && tokens[i].value == tag) {
          size_t j = i + 1; bool matches = false;
          for (; j < tokens.size() && tokens[j].event == 15; ++j)
            if (tokens[j].name == match && Value(tokens[j]) == value) matches = true;
          if (matches) { Require(begin == tokens.size(), "Duplicate Android settings entry."); begin = i; end = j; }
        }
        ++depth;
      } else if (tokens[i].event == 3) {
        --depth; if (depth == 0 && tokens[i].value == root) root_end = i;
      }
    }
    Require(root_end < tokens.size(), "Android settings root is missing.");
    if (begin == tokens.size()) {
      std::vector<Token> add{{2, 0x30, {}, tag}, {15, 0x20, match, value}};
      for (const auto &a : attributes) add.push_back({15, 0x20, a.first, a.second});
      add.push_back({3, 0x30, {}, tag}); tokens.insert(tokens.begin() + root_end, add.begin(), add.end());
    } else for (const auto &a : attributes) {
      size_t j = begin + 1; for (; j < end && tokens[j].name != a.first; ++j) {}
      if (j == end) { tokens.insert(tokens.begin() + end, {15, 0x20, a.first, a.second}); ++end; }
      else { tokens[j].type = 0x20; tokens[j].value = a.second; }
    }
  }
  void Erase(const std::string &root, const std::string &tag,
             const std::string &match, const std::string &value) {
    auto *parent = doc.FirstChildElement(root.c_str()); Require(parent, "Unexpected Android XML root.");
    if (!binary) {
      for (auto *e = parent->FirstChildElement(tag.c_str()); e;) {
        auto *next = e->NextSiblingElement(tag.c_str());
        if (e->Attribute(match.c_str(), value.c_str())) parent->DeleteChild(e);
        e = next;
      }
      return;
    }
    int depth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
      if (tokens[i].event == 2) {
        if (depth == 1 && tokens[i].value == tag) {
          bool found = false;
          for (size_t j = i + 1; j < tokens.size() && tokens[j].event == 15; ++j)
            if (tokens[j].name == match && Value(tokens[j]) == value) found = true;
          if (found) {
            size_t end = i + 1; int nested = 1;
            for (; end < tokens.size() && nested; ++end) {
              if (tokens[end].event == 2) ++nested;
              if (tokens[end].event == 3) --nested;
            }
            Require(nested == 0, "Invalid Android XML entry.");
            tokens.erase(tokens.begin() + i, tokens.begin() + end); --i; continue;
          }
        }
        ++depth;
      } else if (tokens[i].event == 3) --depth;
    }
  }
  std::string Serialize() const {
    if (!binary) { tinyxml2::XMLPrinter printer; doc.Print(&printer); return printer.CStr(); }
    std::string out("ABX\0", 4); std::map<std::string, unsigned> pool;
    auto word = [&](unsigned n) { Require(n <= 65535, "Android ABX string is too long."); out += char(n >> 8); out += char(n); };
    auto utf = [&](const std::string &s) { word(s.size()); out += s; };
    auto intern = [&](const std::string &s) {
      auto it = pool.find(s); if (it != pool.end()) { word(it->second); return; }
      word(65535); utf(s); if (pool.size() < 65535) pool.emplace(s, pool.size());
    };
    for (const auto &t : tokens) {
      out += char(t.event | t.type); if (t.event == 15) intern(t.name);
      if (t.type == 0x30) intern(t.value);
      else if (t.type == 0x20 || t.type == 0x40 || t.type == 0x50) utf(t.value);
      else out += t.value;
    }
    tinyxml2::XMLDocument verify; std::string error;
    Require(appvault::ParseSettingsXml(out, verify, error), "Could not verify updated Android ABX data."); return out;
  }
};
std::string EnabledSettings(const std::string &bytes) {
  Xml xml(bytes); auto *root = xml.doc.FirstChildElement("settings");
  Require(root, "This is not the Android global settings database."); uint64_t next_id = 0;
  for (auto *e = root->FirstChildElement("setting"); e; e = e->NextSiblingElement("setting")) {
    uint64_t id = 0; e->QueryUnsigned64Attribute("id", &id); next_id = std::max(next_id, id + 1);
  }
  for (const char *name : {"adb_enabled", "development_settings_enabled"}) {
    bool exists = false;
    for (auto *e = root->FirstChildElement("setting"); e; e = e->NextSiblingElement("setting"))
      if (e->Attribute("name") && std::string(e->Attribute("name")) == name) exists = true;
    Xml::Attributes fields{{"value", "1"}};
    if (!exists) { fields["id"] = std::to_string(next_id++); fields["package"] = "com.android.shell"; }
    xml.Set("settings", "setting", "name", name, fields);
  }
  return xml.Serialize();
}
// Preserve unknown protobuf fields verbatim when updating init's property store.
struct Field { uint64_t number = 0; unsigned wire = 0; std::string value, raw; };
uint64_t Varint(const std::string &s, size_t &p) {
  uint64_t out = 0;
  for (unsigned shift = 0; shift < 70; shift += 7) {
    Require(p < s.size(), "Truncated persistent properties."); unsigned char b = s[p++];
    Require(shift != 63 || b <= 1, "Invalid persistent property varint."); out |= uint64_t(b & 127) << shift;
    if (!(b & 128)) return out;
  }
  throw Failure("Invalid persistent property varint.");
}
std::string EncodeVarint(uint64_t n) {
  std::string out; do { unsigned b = n & 127; n >>= 7; out += char(b | (n ? 128 : 0)); } while (n); return out;
}
std::string LengthField(unsigned n, const std::string &s) { return EncodeVarint((n << 3) | 2) + EncodeVarint(s.size()) + s; }
std::vector<Field> Fields(const std::string &s) {
  std::vector<Field> out; size_t p = 0;
  while (p < s.size()) {
    const size_t start = p; auto tag = Varint(s, p); Field f; f.number = tag >> 3; f.wire = tag & 7;
    Require(f.number > 0, "Invalid persistent properties field.");
    if (f.wire == 0) { const size_t a = p; Varint(s, p); f.value = s.substr(a, p - a); }
    else {
      const uint64_t n = f.wire == 2 ? Varint(s, p) : f.wire == 1 ? 8 : f.wire == 5 ? 4 : kLimit + 1;
      Require(n <= s.size() - p, "Invalid persistent properties field length."); f.value = s.substr(p, n); p += n;
    }
    f.raw = s.substr(start, p - start); out.push_back(std::move(f));
  }
  return out;
}
std::map<std::string, std::string> Properties(const std::string &bytes) {
  std::map<std::string, std::string> out;
  for (const auto &f : Fields(bytes)) if (f.number == 1 && f.wire == 2) {
    std::string name, value; bool has_name = false, has_value = false;
    for (const auto &p : Fields(f.value)) {
      if (p.number == 1 && p.wire == 2) { Require(!has_name, "Duplicate property name field."); name = p.value; has_name = true; }
      if (p.number == 2 && p.wire == 2) { Require(!has_value, "Duplicate property value field."); value = p.value; has_value = true; }
    }
    Require(has_name && (name.rfind("persist.", 0) == 0 || name.rfind("next_boot.", 0) == 0) && out.emplace(name, value).second,
            "Unrecognized or duplicate Android persistent property.");
  }
  return out;
}
std::string PatchProperties(const std::string &bytes, const std::map<std::string, std::string> &changes) {
  auto pending = changes; std::string out;
  for (const auto &f : Fields(bytes)) {
    std::string name;
    if (f.number == 1 && f.wire == 2)
      for (const auto &p : Fields(f.value)) if (p.number == 1 && p.wire == 2) name = p.value;
    auto change = pending.find(name);
    if (change == pending.end()) { out += f.raw; continue; }
    std::string entry; bool written = false;
    for (const auto &p : Fields(f.value)) {
      if (p.number == 2 && p.wire == 2) { entry += LengthField(2, change->second); written = true; }
      else entry += p.raw;
    }
    if (!written) entry += LengthField(2, change->second);
    out += LengthField(1, entry); pending.erase(change);
  }
  for (const auto &p : pending) out += LengthField(1, LengthField(1, p.first) + LengthField(2, p.second));
  Properties(out); return out;
}
bool HasAdb(const std::string &s) {
  std::istringstream parts(s); std::string p;
  while (std::getline(parts, p, ',')) if (p == "adb") return true;
  return false;
}
std::string AddAdb(const std::string &s) {
  if (HasAdb(s)) return s;
  if (s.empty() || s == "none") return "adb";
  Require(s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_,") == std::string::npos, "Unrecognized USB function list.");
  return s + ",adb";
}
std::string SettingsPath(const std::string &root) {
  const auto canonical = Read(root, "system/users/0/settings_global.xml", true);
  if (canonical.exists) return canonical.path;
  // Bounded, user-0-only fallback. Never search media, backups or other users.
  Fd dir(OpenDir(root, "system/users"));
  DIR *raw = fdopendir(dup(dir.n)); Require(raw, "Could not inspect Android user databases.");
  std::unique_ptr<DIR, int(*)(DIR*)> entries(raw, closedir);
  unsigned count = 0; std::vector<std::string> matches;
  while (auto *e = readdir(raw)) {
    Require(++count < 256, "Too many Android user directories."); const std::string name = e->d_name;
    if (name != "user_0" && name != "user0") continue;
    auto file = Read(root, "system/users/" + name + "/settings_global.xml", true);
    if (file.exists) matches.push_back(file.path);
  }
  Require(matches.size() == 1, matches.empty() ? "Android user 0 global settings were not found. Unlock Android data first." : "Multiple Android global settings databases were found.");
  return matches.front();
}
struct Plan { File before, after; };
File NewFile(std::string path, uid_t uid, gid_t gid, mode_t mode, const std::string &label) {
  File f; f.path = std::move(path); f.exists = true; f.uid = uid; f.gid = gid; f.mode = mode;
#ifdef __ANDROID__
  f.label = label; f.label += '\0';
#else
  (void)label;
#endif
  return f;
}
std::vector<std::string> Keys(const std::string &bytes) {
  std::vector<std::string> out; std::istringstream lines(bytes); std::string line;
  while (std::getline(lines, line)) {
    PublicKey key; std::string error; if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    Require(ParsePublicKey(line, &key, &error), "An existing Android ADB key is invalid."); out.push_back(key.line);
  }
  return out;
}
std::string JournalPath(const std::string &root) {
  auto f = Read(root, std::string(kBackup) + "/journal.json", true); return f.exists ? f.path : "";
}
Json::Value LoadJournal(const std::string &root) {
  const auto file = Read(root, std::string(kBackup) + "/journal.json");
  Json::CharReaderBuilder reader; Json::Value j; std::string error; std::istringstream input(file.bytes);
  Require(Json::parseFromStream(reader, input, &j, &error) && j["schema"] == 1 && j["files"].isArray(), "Invalid Android ADB rescue backup.");
  return j;
}
void SaveJournal(const std::string &root, const Json::Value &j) {
  auto file = NewFile(std::string(kBackup) + "/journal.json", 0, 0, 0600, "u:object_r:system_data_file:s0");
  Json::StreamWriterBuilder writer; file.bytes = Json::writeString(writer, j); Write(root, file);
}
void AddPlan(std::vector<Plan> &plans, const File &before, const std::string &bytes, const File *defaults = nullptr) {
  if (before.exists && before.bytes == bytes) return;
  Require(before.exists || defaults, "Missing Android file metadata.");
  auto after = before.exists ? before : *defaults; after.bytes = bytes; plans.push_back({before, after});
}
std::vector<Plan> Prepare(const std::string &root, const std::string &public_key) {
  std::vector<Plan> plans; const auto settings = Read(root, SettingsPath(root));
  for (const char *suffix : {".bak", ".new"})
    Require(!Read(root, settings.path + suffix, true).exists, "An unfinished Android settings write exists. Boot Android or restore its database before using ADB Rescue.");
  AddPlan(plans, settings, EnabledSettings(settings.bytes));
  auto fallback = Read(root, settings.path + ".fallback", true);
  if (fallback.exists) AddPlan(plans, fallback, EnabledSettings(fallback.bytes));
  const auto properties = Read(root, "property/persistent_properties", true);
  if (properties.exists) {
    Require(!Read(root, "property/persistent_properties.tmp", true).exists, "An unfinished Android persistent-property write exists.");
    auto values = Properties(properties.bytes);
    std::map<std::string, std::string> changes{{"persist.sys.usb.config", AddAdb(values["persist.sys.usb.config"])}};
    // Existing vendor config can override system config during init. Preserve
    // every other USB function; never create vendor properties speculatively.
    if (values.count("persist.vendor.usb.config")) changes["persist.vendor.usb.config"] = AddAdb(values["persist.vendor.usb.config"]);
    for (const char *name : {"next_boot.persist.sys.usb.config", "next_boot.persist.vendor.usb.config"})
      if (values.count(name)) changes[name] = AddAdb(values[name]);
    AddPlan(plans, properties, PatchProperties(properties.bytes, changes));
  } else {
    const auto legacy = Read(root, "property/persist.sys.usb.config");
    Require(legacy.uid == 0 && legacy.gid == 0 && (legacy.mode & 077) == 0, "Unrecognized legacy Android property permissions.");
    AddPlan(plans, legacy, AddAdb(legacy.bytes)); auto vendor = Read(root, "property/persist.vendor.usb.config", true);
    if (vendor.exists) AddPlan(plans, vendor, AddAdb(vendor.bytes));
  }
  if (!public_key.empty()) {
    PublicKey key; std::string error; const bool valid = ParsePublicKey(public_key, &key, &error); Require(valid, error);
    auto keys = Read(root, "misc/adb/adb_keys", true);
    auto lines = keys.exists ? Keys(keys.bytes) : std::vector<std::string>{};
    const auto material = key.line.substr(0, key.line.find(' ')); bool found = false;
    for (const auto &line : lines) if (line.substr(0, line.find(' ')) == material) found = true;
    std::string bytes = keys.bytes;
    if (!found) { if (!bytes.empty() && bytes.back() != '\n') bytes += '\n'; bytes += key.line + '\n'; }
    const auto defaults = NewFile(keys.path, 1000, 2000, 0640, "u:object_r:adb_keys_file:s0"); AddPlan(plans, keys, bytes, &defaults);
    for (const char *suffix : {".bak", ".new"})
      Require(!Read(root, std::string("misc/adb/adb_temp_keys.xml") + suffix, true).exists, "An unfinished Android ADB authorization write exists.");
    auto timestamps = Read(root, "misc/adb/adb_temp_keys.xml", true);
    Xml xml(timestamps.exists ? timestamps.bytes : "<keyStore version=\"1\"/>");
    auto *store = xml.doc.FirstChildElement("keyStore");
    Require(store && store->IntAttribute("version", 1) == 1, "Unsupported Android ADB authorization metadata.");
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    Require(now > 1577836800000LL, "Set the correct date and time before authorizing an Android ADB computer.");
    std::string metadata_key = key.line;
    for (auto *e = store->FirstChildElement("adbKey"); e; e = e->NextSiblingElement("adbKey"))
      if (const char *old = e->Attribute("key")) {
        const std::string s = old; if (s.substr(0, s.find(' ')) == material) metadata_key = s;
      }
    xml.Set("keyStore", "adbKey", "key", metadata_key, {{"lastConnection", std::to_string(now)}});
    const auto meta_defaults = NewFile(timestamps.path, 1000, 2000, 0600, "u:object_r:adb_keys_file:s0"); AddPlan(plans, timestamps, xml.Serialize(), &meta_defaults);
  }
  return plans;
}
std::string OriginalPath(const Json::Value &journal, unsigned index) {
  const std::string prefix = journal.get("prefix", "").asString();
  Require(prefix.size() <= 64 && prefix.find_first_not_of("0123456789-") == std::string::npos,
          "Invalid rescue backup identifier.");
  return std::string(kBackup) + "/" + (prefix.empty() ? "" : prefix + "-") + std::to_string(index) + ".original";
}
std::string Material(const std::string &key) { return key.substr(0, key.find(' ')); }
std::string MergeRestore(const std::string &path, const std::string &before,
                         const std::string &applied, const std::string &current) {
  if (path.find("/settings_global.xml") != std::string::npos) {
    Xml old(before), changed(applied), now(current);
    auto *a = old.doc.FirstChildElement("settings"), *b = changed.doc.FirstChildElement("settings");
    Require(a && b && now.doc.FirstChildElement("settings"), "Android global settings cannot be merged.");
    for (const char *name : {"adb_enabled", "development_settings_enabled"}) {
      tinyxml2::XMLElement *original = nullptr, *modified = nullptr;
      for (auto *e = a->FirstChildElement("setting"); e; e = e->NextSiblingElement("setting"))
        if (e->Attribute("name", name)) original = e;
      for (auto *e = b->FirstChildElement("setting"); e; e = e->NextSiblingElement("setting"))
        if (e->Attribute("name", name)) modified = e;
      if (!modified) continue;
      if (original && original->Attribute("value") && modified->Attribute("value") &&
          std::string(original->Attribute("value")) == modified->Attribute("value")) continue;
      if (!original) now.Erase("settings", "setting", "name", name);
      else {
        Require(original->Attribute("value"), "The original Android ADB value is unsupported.");
        now.Set("settings", "setting", "name", name, {{"value", original->Attribute("value")}});
      }
    }
    return now.Serialize();
  }
  if (path == "property/persistent_properties") {
    const auto a = Properties(before), b = Properties(applied);
    Properties(current); std::map<std::string, std::string> changes; std::set<std::string> removed;
    for (const char *name : {"persist.sys.usb.config", "persist.vendor.usb.config",
                             "next_boot.persist.sys.usb.config", "next_boot.persist.vendor.usb.config"}) {
      auto old = a.find(name), modified = b.find(name);
      if (modified == b.end() || (old != a.end() && old->second == modified->second)) continue;
      if (old == a.end()) removed.insert(name); else changes[name] = old->second;
    }
    std::string out;
    for (const auto &field : Fields(current)) {
      std::string name;
      if (field.number == 1 && field.wire == 2)
        for (const auto &p : Fields(field.value)) if (p.number == 1 && p.wire == 2) name = p.value;
      if (!removed.count(name)) out += field.raw;
    }
    return PatchProperties(out, changes);
  }
  if (path == "misc/adb/adb_keys") {
    std::set<std::string> old, added;
    for (const auto &key : Keys(before)) old.insert(Material(key));
    for (const auto &key : Keys(applied)) if (!old.count(Material(key))) added.insert(Material(key));
    std::string out;
    for (const auto &key : Keys(current)) if (!added.count(Material(key))) out += key + "\n";
    return out;
  }
  if (path == "misc/adb/adb_temp_keys.xml") {
    Xml old(before.empty() ? "<keyStore version=\"1\"/>" : before), changed(applied), now(current);
    auto *a = old.doc.FirstChildElement("keyStore"), *b = changed.doc.FirstChildElement("keyStore"), *n = now.doc.FirstChildElement("keyStore");
    Require(a && b && n, "Android ADB authorization metadata cannot be merged.");
    std::map<std::string, std::string> originals;
    for (auto *e = a->FirstChildElement("adbKey"); e; e = e->NextSiblingElement("adbKey"))
      if (e->Attribute("key") && e->Attribute("lastConnection")) originals[Material(e->Attribute("key"))] = e->Attribute("lastConnection");
    for (auto *e = b->FirstChildElement("adbKey"); e; e = e->NextSiblingElement("adbKey")) {
      if (!e->Attribute("key") || !e->Attribute("lastConnection")) continue;
      const auto material = Material(e->Attribute("key")); const auto previous = originals.find(material);
      if (previous != originals.end() && previous->second == e->Attribute("lastConnection")) continue;
      std::vector<std::string> matches;
      for (auto *key = n->FirstChildElement("adbKey"); key; key = key->NextSiblingElement("adbKey"))
        if (key->Attribute("key") && Material(key->Attribute("key")) == material) matches.push_back(key->Attribute("key"));
      for (const auto &key : matches)
        if (previous == originals.end()) now.Erase("keyStore", "adbKey", "key", key);
        else now.Set("keyStore", "adbKey", "key", key, {{"lastConnection", previous->second}});
    }
    return now.Serialize();
  }
  // Legacy files contain only the one property whose restore was requested.
  if (path == "property/persist.sys.usb.config" || path == "property/persist.vendor.usb.config") return before;
  throw Failure("Unsupported Android ADB rescue rollback target.");
}
void Transaction(const std::string &root, const std::vector<Plan> &plans) {
  if (plans.empty()) return;
  Json::Value previous;
  if (!JournalPath(root).empty()) {
    previous = LoadJournal(root);
    Require(previous["state"] == "applied", "An interrupted ADB Rescue change must be restored before continuing.");
  }
  Fd backup(OpenDir(root, kBackup, true));
  Require(fchmod(backup.n, 0700) == 0 && fchown(backup.n, 0, 0) == 0, "Could not protect rescue backup.");
  Json::Value journal; journal["schema"] = 1; journal["state"] = "prepared"; journal["files"] = Json::Value(Json::arrayValue);
  static unsigned sequence = 0;
  journal["prefix"] = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence);
  for (size_t i = 0; i < plans.size(); ++i) {
    const auto &p = plans[i]; const auto current = Read(root, p.before.path, true);
    Require(current.exists == p.before.exists && current.bytes == p.before.bytes, "Android data changed during rescue preparation.");
    Json::Value r; r["path"] = p.before.path; r["existed"] = p.before.exists;
    r["uid"] = Json::UInt(p.before.uid); r["gid"] = Json::UInt(p.before.gid); r["mode"] = Json::UInt(p.before.mode);
    r["label"] = p.before.label; r["before"] = Hash(p.before.bytes); r["after"] = Hash(p.after.bytes);
    auto saved = NewFile(OriginalPath(journal, i), 0, 0, 0600, "u:object_r:system_data_file:s0");
    saved.bytes = p.before.bytes; Write(root, saved);
    saved.path += ".applied"; saved.bytes = p.after.bytes; Write(root, saved); journal["files"].append(r);
  }
  SaveJournal(root, journal); size_t written = 0;
  try {
    for (const auto &p : plans) {
      const auto current = Read(root, p.before.path, true);
      Require(current.exists == p.before.exists && current.bytes == p.before.bytes, "Android data changed before rescue could be applied.");
      ++written; Write(root, p.after); const auto verify = Read(root, p.after.path);
      Require(verify.bytes == p.after.bytes && verify.uid == p.after.uid && verify.gid == p.after.gid &&
          verify.mode == p.after.mode && verify.label == p.after.label, "Android file metadata verification failed.");
    }
    journal["state"] = "applied"; SaveJournal(root, journal);
    if (previous["files"].isArray() && previous["files"].size() <= 12)
      for (Json::ArrayIndex i = 0; i < previous["files"].size(); ++i) {
        try { Remove(root, OriginalPath(previous, i)); Remove(root, OriginalPath(previous, i) + ".applied"); } catch (...) {}
      }
  } catch (...) {
    for (size_t i = written; i > 0; --i) {
      try { const auto &p = plans[i - 1]; if (p.before.exists) Write(root, p.before); else Remove(root, p.before.path); } catch (...) {}
    }
    throw;
  }
}
}  // namespace
bool ParsePublicKey(const std::string &input, PublicKey *key, std::string *error) {
  try {
    Require(input.size() <= 8192 && input.find('\0') == std::string::npos, "Select the public adbkey.pub file, not a private key.");
    auto line = input; while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    Require(!line.empty() && line.find_first_of("\n\r\t") == std::string::npos, "Select one Android ADB public key.");
    const auto split = line.find(' '); const auto encoded = line.substr(0, split);
    Require(encoded.size() == 700 && encoded.back() == '=' &&
        encoded.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") == std::string::npos,
        "This is not an Android ADB RSA public key. Select adbkey.pub.");
    std::string decoded(525, '\0'); const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()),
        reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size());
    Require(length == 525, "Invalid Android ADB public key encoding."); decoded.resize(524);
    auto little = [&](size_t p) { uint32_t n = 0; for (int i = 3; i >= 0; --i) n = (n << 8) | static_cast<unsigned char>(decoded[p + i]); return n; };
    Require(little(0) == 64 && (little(520) == 65537 || little(520) == 3) &&
        (static_cast<unsigned char>(decoded[8]) & 1) && (static_cast<unsigned char>(decoded[263]) & 128), "Invalid Android ADB RSA key parameters.");
    unsigned char digest[SHA256_DIGEST_LENGTH]; SHA256(reinterpret_cast<const unsigned char *>(decoded.data()), decoded.size(), digest);
    key->line = line; key->fingerprint = "SHA256:" + Hex(digest, sizeof(digest)); key->label = split == std::string::npos ? "" : line.substr(split + 1);
    Require(key->label.size() < 256 && std::none_of(key->label.begin(), key->label.end(), [](unsigned char c) { return c < 32 || c == 127; }), "Invalid ADB public key comment.");
    return true;
  } catch (const std::exception &e) { if (error) *error = e.what(); return false; }
}
Status Inspect(const std::string &root) {
  std::lock_guard<std::mutex> lock(mutex); Status out;
  try {
    const auto path = SettingsPath(root); const auto settings = Read(root, path); Xml xml(settings.bytes);
    auto *node = xml.doc.FirstChildElement("settings"); Require(node, "Unrecognized Android global settings."); bool enabled = false;
    for (auto *e = node->FirstChildElement("setting"); e; e = e->NextSiblingElement("setting"))
      if (e->Attribute("name") && std::string(e->Attribute("name")) == "adb_enabled") enabled = e->Attribute("value", "1");
    auto properties = Read(root, "property/persistent_properties", true);
    if (properties.exists) {
      auto p = Properties(properties.bytes); out.usb_functions = p["persist.sys.usb.config"];
      // USB properties are configuration details, not the Android toggle.
      // Inactive/empty vendor properties must not turn an enabled toggle off.
    } else out.usb_functions = Read(root, "property/persist.sys.usb.config").bytes;
    auto keys = Read(root, "misc/adb/adb_keys", true); out.authorized_keys = keys.exists ? Keys(keys.bytes).size() : 0;
    out.settings_path = root + "/" + path; out.settings_format = xml.binary ? "ABX" : "XML";
    out.available = true; out.enabled = enabled; out.can_restore = !JournalPath(root).empty();
  } catch (const std::exception &e) { out.message = e.what(); try { out.can_restore = !JournalPath(root).empty(); } catch (...) {} }
  return out;
}
bool Enable(const std::string &key, std::string *error, const std::string &root) {
  std::lock_guard<std::mutex> lock(mutex);
  try { Transaction(root, Prepare(root, key)); return true; }
  catch (const std::exception &e) { if (error) *error = e.what(); return false; }
}
bool Restore(std::string *error, const std::string &root) {
  std::lock_guard<std::mutex> lock(mutex);
  try {
    auto journal = LoadJournal(root); const auto &files = journal["files"]; Require(files.size() <= 12, "Invalid rescue backup file count.");
    std::vector<File> originals;
    for (Json::ArrayIndex i = 0; i < files.size(); ++i) {
      const auto &r = files[i]; const auto path = r["path"].asString();
      Require((path.rfind("system/users/", 0) == 0 && path.find("/settings_global.xml") != std::string::npos) ||
          path == "property/persistent_properties" || path == "property/persist.sys.usb.config" || path == "property/persist.vendor.usb.config" ||
          path == "misc/adb/adb_keys" || path == "misc/adb/adb_temp_keys.xml", "Invalid rescue backup target.");
      auto current = Read(root, path, true);
      auto file = Read(root, OriginalPath(journal, i));
      auto applied = Read(root, OriginalPath(journal, i) + ".applied");
      Require(Hash(file.bytes) == r["before"].asString() && Hash(applied.bytes) == r["after"].asString(), "The rescue backup is damaged.");
      const bool existed = r["existed"].asBool();
      file.path = path; file.exists = existed; file.uid = r["uid"].asUInt(); file.gid = r["gid"].asUInt();
      file.mode = r["mode"].asUInt(); file.label = r["label"].asString();
      if (current.exists && Hash(current.bytes) != r["before"].asString() && Hash(current.bytes) != r["after"].asString()) {
        file.bytes = MergeRestore(path, file.bytes, applied.bytes, current.bytes);
        file.exists = true; file.uid = current.uid; file.gid = current.gid; file.mode = current.mode; file.label = current.label;
      }
      originals.push_back(file);
    }
    for (const auto &file : originals) { if (file.exists) Write(root, file); else Remove(root, file.path); }
    Remove(root, std::string(kBackup) + "/journal.json");
    for (size_t i = 0; i < originals.size(); ++i) { Remove(root, OriginalPath(journal, i)); Remove(root, OriginalPath(journal, i) + ".applied"); }
    return true;
  } catch (const std::exception &e) { if (error) *error = e.what(); return false; }
}
}  // namespace aeraui::adb_rescue
