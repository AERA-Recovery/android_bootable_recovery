// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
// Snapshot a DT-declared installed runtime into RAM before starting its users.
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <glob.h>
#include <json/json.h>
#include <linux/magic.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include <android-base/properties.h>
#endif

namespace {
using std::string;
struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void Need(bool ok, const string& text) {
  if (!ok) throw Failure(text + (errno ? ": " + string(strerror(errno)) : ""));
}
bool Below(const string& path, const string& root) {
  return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
                          path[root.size()] == '/');
}
bool Exists(const string& path) {
  struct stat s{};
  return lstat(path.c_str(), &s) == 0;
}
string Read(const string& path) {
  std::ifstream f(path);
  return { std::istreambuf_iterator<char>(f), {} };
}
bool Token(const string& s) {
  return !s.empty() &&
         s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
             string::npos;
}
string Relative(const string& s) {
  Need(!s.empty() && s[0] != '/' && s.find_first_of("\n\r\t ") == string::npos,
       "Invalid relative runtime path");
  std::istringstream in(s);
  string part, result;
  while (std::getline(in, part, '/')) {
    Need(part != "..", "Runtime path escapes its root");
    if (part.empty() || part == ".") continue;
    result += (result.empty() ? "" : "/") + part;
  }
  return result;
}
string Parent(const string& p) {
  auto n = p.rfind('/');
  return n == 0 ? "/" : p.substr(0, n);
}
string Base(const string& p) {
  return p.substr(p.rfind('/') + 1);
}
void Directories(const string& path, mode_t mode = 0755) {
  Need(!path.empty() && path[0] == '/', "Directory must be absolute");
  for (size_t n = 1; n <= path.size(); ++n) {
    if (n != path.size() && path[n] != '/') continue;
    string p = path.substr(0, n);
    if (p.empty()) continue;
    if (mkdir(p.c_str(), mode) == 0)
      Need(chmod(p.c_str(), mode) == 0, "Cannot set runtime directory mode");
    else
      Need(errno == EEXIST, "Cannot create " + p);
    struct stat st{};
    Need(lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode),
         "Directory is a symlink or not a directory: " + p);
  }
}
bool Ram(const string& path) {
  struct statfs fs{};
  return statfs(path.c_str(), &fs) == 0 && (fs.f_type == TMPFS_MAGIC || fs.f_type == RAMFS_MAGIC);
}
string MountEscape(string s) {
  for (const auto& item : std::vector<std::pair<string, string>>{
           { "\\040", " " }, { "\\011", "\t" }, { "\\012", "\n" }, { "\\134", "\\" } }) {
    size_t p = 0;
    while ((p = s.find(item.first, p)) != string::npos) {
      s.replace(p, 4, item.second);
      p += item.second.size();
    }
  }
  return s;
}
struct Mount {
  string target, source, fs, options;
};
std::vector<Mount> Mounts() {
  std::ifstream input("/proc/self/mountinfo");
  string line;
  std::vector<Mount> rows;
  while (std::getline(input, line)) {
    std::istringstream in(line);
    string id, parent, device, root, target, options, next;
    if (!(in >> id >> parent >> device >> root >> target >> options)) continue;
    while (in >> next && next != "-") {
    }
    Mount m;
    m.target = MountEscape(target);
    m.options = options;
    if (in >> m.fs >> m.source) {
      m.source = MountEscape(m.source);
      rows.push_back(m);
    }
  }
  return rows;
}
void LabelAndTimes(int fd, const string& source, const struct stat& st) {
  Need(fchown(fd, st.st_uid, st.st_gid) == 0, "Cannot preserve ownership");
  Need(fchmod(fd, st.st_mode & 07777) == 0, "Cannot preserve mode");
  timespec times[2] = { st.st_atim, st.st_mtim };
  Need(futimens(fd, times) == 0, "Cannot preserve timestamps");
  char label[1024];
  ssize_t size = getxattr(source.c_str(), "security.selinux", label, sizeof(label));
  // OEM-only labels may not exist in recovery's policy (EINVAL). In that case
  // keep the recovery RAM filesystem's valid default label, as for an ordinary
  // recovery-owned file, instead of importing an unknown Android label.
  if (size > 0 && fsetxattr(fd, "security.selinux", label, size, 0) != 0)
    Need(errno == ENOTSUP || errno == EOPNOTSUPP || errno == EPERM || errno == EACCES ||
             errno == EINVAL,
         "Cannot preserve file context");
}
void RemoveTree(const string& path) {
  struct stat s{};
  if (lstat(path.c_str(), &s)) return;
  if (S_ISDIR(s.st_mode)) {
    DIR* d = opendir(path.c_str());
    Need(d, "Cannot inspect temporary directory");
    while (dirent* e = readdir(d)) {
      string n = e->d_name;
      if (n != "." && n != "..") RemoveTree(path + "/" + n);
    }
    closedir(d);
    Need(rmdir(path.c_str()) == 0, "Cannot remove temporary directory");
  } else
    Need(unlink(path.c_str()) == 0, "Cannot remove temporary file");
}
struct Source {
  string name, root, mounted_at, block;
  bool owned = false;
};
template <typename Header, typename Program, typename Dynamic>
std::vector<string> ReadElfDependencies(const string& bytes) {
  Need(bytes.size() >= sizeof(Header), "Truncated runtime ELF header");
  Header h{};
  memcpy(&h, bytes.data(), sizeof(h));
  auto span = [&](uint64_t offset, uint64_t length) {
    return offset <= bytes.size() && length <= bytes.size() - offset;
  };
  Need(h.e_phentsize == sizeof(Program) && span(h.e_phoff, uint64_t(h.e_phnum) * sizeof(Program)),
       "Invalid runtime ELF program headers");
  std::vector<Program> programs;
  Program dynamic{};
  bool found = false;
  for (unsigned i = 0; i < h.e_phnum; ++i) {
    Program p{};
    memcpy(&p, bytes.data() + h.e_phoff + i * sizeof(p), sizeof(p));
    programs.push_back(p);
    if (p.p_type == PT_DYNAMIC) {
      dynamic = p;
      found = true;
    }
  }
  if (!found) return {};
  Need(span(dynamic.p_offset, dynamic.p_filesz) && dynamic.p_filesz % sizeof(Dynamic) == 0,
       "Invalid runtime ELF dynamic table");
  uint64_t strings = 0, string_size = 0;
  std::vector<uint64_t> needed;
  for (uint64_t offset = dynamic.p_offset; offset < dynamic.p_offset + dynamic.p_filesz;
       offset += sizeof(Dynamic)) {
    Dynamic d{};
    memcpy(&d, bytes.data() + offset, sizeof(d));
    if (d.d_tag == DT_NULL) break;
    if (d.d_tag == DT_STRTAB)
      strings = d.d_un.d_ptr;
    else if (d.d_tag == DT_STRSZ)
      string_size = d.d_un.d_val;
    else if (d.d_tag == DT_NEEDED)
      needed.push_back(d.d_un.d_val);
  }
  if (needed.empty()) return {};
  uint64_t file_offset = 0;
  bool mapped = false;
  for (const auto& p : programs)
    if (p.p_type == PT_LOAD && strings >= p.p_vaddr && strings - p.p_vaddr <= p.p_filesz &&
        string_size <= p.p_filesz - (strings - p.p_vaddr)) {
      file_offset = p.p_offset + (strings - p.p_vaddr);
      mapped = true;
      break;
    }
  Need(mapped && span(file_offset, string_size), "Invalid runtime ELF string table");
  std::vector<string> result;
  for (auto offset : needed) {
    Need(offset < string_size, "Invalid runtime ELF dependency offset");
    const char* start = bytes.data() + file_offset + offset;
    const char* end = static_cast<const char*>(memchr(start, 0, string_size - offset));
    Need(end, "Unterminated runtime ELF dependency");
    string name(start, end);
    Need(name.find('/') == string::npos && !name.empty(), "Invalid runtime ELF dependency name");
    result.push_back(name);
  }
  return result;
}
std::vector<string> ElfDependencies(const string& file) {
  struct stat st{};
  Need(stat(file.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size < 256LL * 1024 * 1024,
       "Invalid runtime ELF file: " + file);
  string bytes = Read(file);
  Need(bytes.size() == static_cast<size_t>(st.st_size), "Cannot read runtime ELF: " + file);
  Need(bytes.size() > EI_NIDENT && memcmp(bytes.data(), ELFMAG, SELFMAG) == 0 &&
           bytes[EI_DATA] == ELFDATA2LSB,
       "Runtime dependency is not a supported ELF: " + file);
  if (bytes[EI_CLASS] == ELFCLASS64)
    return ReadElfDependencies<Elf64_Ehdr, Elf64_Phdr, Elf64_Dyn>(bytes);
  Need(bytes[EI_CLASS] == ELFCLASS32, "Unknown runtime ELF class");
  return ReadElfDependencies<Elf32_Ehdr, Elf32_Phdr, Elf32_Dyn>(bytes);
}
class Stage {
 public:
  Json::Value config, phase;
  string id, name, slot, base, root, temporary, identity, fixture;
  bool test = false, copy_only = false;
  int lock = -1;
  uint64_t bytes = 0, files = 0;
  std::vector<Source> sources;
  std::map<std::pair<dev_t, ino_t>, string> copied;
  std::vector<Mount> replaced;
  std::vector<string> published;

  Stage(const string& path, const string& phase_name, string test_fixture = "",
        string test_output = "")
      : name(phase_name), fixture(test_fixture) {
    Json::CharReaderBuilder reader;
    std::istringstream input(Read(path));
    string error;
    Need(Json::parseFromStream(reader, input, &config, &error),
         "Invalid runtime configuration: " + error);
    Need(config["version"].asInt() == 1, "Unsupported runtime configuration");
    id = config["name"].asString();
    Need(Token(id) && Token(name), "Invalid runtime name");
    phase = config["phases"][name];
    Need(phase.isObject(), "Unknown runtime phase");
    test = !fixture.empty();
#ifndef AERA_RUNTIME_RAM_HOST_TEST
    Need(!test, "Fixture mode is not available in recovery");
#endif
#ifdef __ANDROID__
    slot = android::base::GetProperty("ro.boot.slot_suffix", "");
#else
    slot = "_a";
#endif
    Need(slot.empty() || slot == "_a" || slot == "_b", "Unknown active boot slot");
    base = test ? test_output : "/tmp/aera-runtime/" + id;
    Need(!base.empty() && base[0] == '/', "Invalid RAM output directory");
    Directories(base);
    Need(test || Ram(base), "Runtime destination is not in RAM");
    lock = open((base + "/." + name + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                0600);
    Need(lock >= 0, "Cannot open runtime lock");
    bool acquired = false;
    for (int i = 0; i < 300; ++i) {
      if (flock(lock, LOCK_EX | LOCK_NB) == 0) {
        acquired = true;
        break;
      }
      Need(errno == EWOULDBLOCK, "Cannot lock runtime");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Need(acquired, "Another runtime preparation did not finish");
    root = base + "/" + name;
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : Read(path) + slot + Read("/proc/sys/kernel/random/boot_id")) {
      hash ^= c;
      hash *= 1099511628211ULL;
    }
    std::ostringstream hex;
    hex << std::hex << hash;
    identity = hex.str() + "\n";
  }
  ~Stage() {
    for (auto it = sources.rbegin(); it != sources.rend(); ++it)
      if (it->owned) umount(it->mounted_at.c_str());
    if (lock >= 0) close(lock);
  }
  string SourceRoot(const string& name) {
    for (auto& s : sources)
      if (s.name == name) return s.root;
    throw Failure("Unknown runtime source: " + name);
  }
  void ChooseContentRoot(Source& source, const Json::Value& item) {
    if (!item["content_roots"].isArray()) return;
    for (const auto& relative : item["content_roots"]) {
      string subdir = Relative(relative.asString());
      string candidate = source.root + (subdir.empty() ? "" : "/" + subdir);
      struct stat info{};
      if (lstat(candidate.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
        source.root = candidate;
        return;
      }
    }
    throw Failure("No supported content root in " + source.name);
  }
  void OpenSources() {
    for (const auto& item : phase["partitions"]) {
      Source s;
      s.name = item["name"].asString();
      string part = item.get("partition", s.name).asString();
      Need(Token(s.name) && Token(part), "Invalid source partition");
      if (test) {
        s.root = fixture + "/" + s.name;
        Need(Exists(s.root), "Missing fixture source " + s.name);
        ChooseContentRoot(s, item);
        sources.push_back(s);
        continue;
      }
      string suffix = item.get("slotted", true).asBool() ? slot : "";
      if (item.get("slotted", true).asBool())
        Need(!slot.empty(), "Runtime requires a known boot slot");
      for (int attempt = 0; attempt < 100 && s.block.empty(); ++attempt) {
        for (const string& prefix :
             { string("/dev/block/mapper/"), string("/dev/block/bootdevice/by-name/"),
               string("/dev/block/by-name/") }) {
          string block = prefix + part + suffix;
          struct stat st{};
          if (stat(block.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
            s.block = block;
            break;
          }
        }
        if (s.block.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      Need(!s.block.empty(), "Active-slot partition is missing: " + part + suffix);
      s.root = base + "/.sources-" + name + "/" + s.name;
      s.mounted_at = s.root;
      Directories(s.root);
      for (const auto& m : Mounts())
        if (m.target == s.mounted_at) {
          struct stat block{}, existing{};
          Need(stat(s.block.c_str(), &block) == 0 && stat(s.mounted_at.c_str(), &existing) == 0 &&
                   block.st_rdev == existing.st_dev,
               "An unrelated source mount occupies " + s.mounted_at);
          Need(umount(s.mounted_at.c_str()) == 0,
               "A previous runtime source is still busy: " + s.mounted_at);
        }
      bool mounted = false;
      for (const auto& type : item["filesystems"]) {
        string fs = type.asString();
        Need(fs == "erofs" || fs == "ext4" || fs == "vfat", "Unsupported runtime filesystem");
        // FAT has no stored Unix modes: never inherit init's restrictive umask.
        const char* options = fs == "ext4"   ? "noload"
                              : fs == "vfat" ? "fmask=0022,dmask=0022"
                                             : nullptr;
        if (mount(s.block.c_str(), s.root.c_str(), fs.c_str(),
                  MS_RDONLY | MS_NOATIME | MS_NOSUID | MS_NODEV | MS_NOEXEC, options) == 0) {
          mounted = true;
          break;
        }
      }
      Need(mounted, "Cannot mount runtime source read-only: " + s.block);
      s.owned = true;
      sources.push_back(s);
      Need(mount(nullptr, s.root.c_str(), nullptr, MS_PRIVATE, nullptr) == 0,
           "Cannot isolate runtime source");
      ChooseContentRoot(sources.back(), item);
    }
  }
  string RamLinkRoot(const Json::Value& link) {
    string other = link["phase"].asString();
    Need(Token(other) && other != name, "Invalid RAM source phase");
    Need(Read(base + "/" + other + "/.ready") == identity,
         "Required RAM phase is not ready: " + other);
    return base + "/" + other + "/" + Relative(link["path"].asString());
  }
  bool PreparedRamPath(const string& path) {
    for (const auto& link : phase["ram_links"])
      if (Below(path, RamLinkRoot(link))) return test || Ram(path);
    return false;
  }
  string Resolve(string path, unsigned hops = 0) {
    Need(hops < 48, "Runtime symlink loop");
    // Resolve each symlink inside its declared source, translating Android's
    // absolute /vendor, /odm and /system links into our private source mounts.
    for (auto& source : sources)
      if (Below(path, source.root)) {
        string relative = path.substr(source.root.size()), current = source.root;
        std::istringstream parts(relative);
        string part;
        while (std::getline(parts, part, '/')) {
          if (part.empty() || part == ".") continue;
          Need(part != "..", "Source symlink escapes its partition");
          current += "/" + part;
          struct stat st{};
          Need(lstat(current.c_str(), &st) == 0, "Missing runtime file: " + current);
          if (S_ISLNK(st.st_mode)) {
            char target[8192];
            ssize_t n = readlink(current.c_str(), target, sizeof(target) - 1);
            Need(n >= 0, "Cannot read runtime symlink");
            target[n] = 0;
            string next = target;
            if (!next.empty() && next[0] == '/') {
              bool mapped = false;
              // More specific, already published RAM aliases take precedence
              // over the enclosing partition's namespace (for example a firmware
              // mountpoint inside a vendor directory).
              for (const auto& link : phase["ram_links"]) {
                string prefix = link["prefix"].asString();
                Need(prefix.size() > 1 && prefix[0] == '/' && prefix.find("..") == string::npos,
                     "Invalid RAM namespace alias");
                if (Below(next, prefix)) {
                  next = RamLinkRoot(link) + next.substr(prefix.size());
                  mapped = true;
                  break;
                }
              }
              if (!mapped)
                for (auto& other : sources) {
                  string prefix = "/" + other.name;
                  if (Below(next, prefix)) {
                    next = other.root + next.substr(prefix.size());
                    mapped = true;
                    break;
                  }
                }
              Need(mapped, "Undeclared absolute runtime symlink: " + next);
            } else {
              next = Parent(current) + "/" + next;
              std::vector<string> clean;
              std::istringstream pieces(next);
              string p;
              while (std::getline(pieces, p, '/')) {
                if (p.empty() || p == ".") continue;
                if (p == "..") {
                  Need(!clean.empty(), "Invalid source symlink");
                  clean.pop_back();
                } else
                  clean.push_back(p);
              }
              next.clear();
              for (const auto& p : clean) next += "/" + p;
            }
            string rest;
            std::getline(parts, rest, '\0');
            if (!rest.empty()) next += "/" + rest;
            return Resolve(next, hops + 1);
          }
        }
        return current;
      }
    Need(PreparedRamPath(path), "Runtime link leaves declared sources: " + path);
    return path;
  }
  void Copy(const string& src, const string& dst, std::set<string> ancestors = {},
            unsigned depth = 0) {
    Need(depth < 48, "Runtime directory recursion limit exceeded");
    string source = Resolve(src);
    struct stat st{};
    Need(stat(source.c_str(), &st) == 0, "Cannot inspect " + source);
    Need(ancestors.insert(source).second, "Runtime directory link cycle");
    if (S_ISDIR(st.st_mode)) {
      Directories(dst);
      DIR* d = opendir(source.c_str());
      Need(d, "Cannot list " + source);
      std::vector<string> names;
      while (dirent* e = readdir(d)) {
        string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
      }
      closedir(d);
      for (const auto& n : names) Copy(source + "/" + n, dst + "/" + n, ancestors, depth + 1);
      int fd = open(dst.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      Need(fd >= 0, "Cannot open RAM directory");
      LabelAndTimes(fd, source, st);
      close(fd);
      return;
    }
    Need(S_ISREG(st.st_mode), "Runtime source is not an ordinary file: " + source);
    Directories(Parent(dst));
    Need(!Exists(dst), "Duplicate runtime destination: " + dst);
    auto key = std::make_pair(st.st_dev, st.st_ino);
    auto previous = copied.find(key);
    if (previous != copied.end() && link(previous->second.c_str(), dst.c_str()) == 0) {
      ++files;
      return;
    }
    // Reuse immutable files from an earlier RAM phase without duplicating the
    // firmware payload. This never hardlinks a file from an Android partition.
    if (PreparedRamPath(source) && link(source.c_str(), dst.c_str()) == 0) {
      copied[key] = dst;
      ++files;
      return;
    }
    int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    Need(input >= 0, "Cannot open " + source);
    int output = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
      close(input);
      throw Failure("Cannot create runtime file: " + dst);
    }
    char buffer[256 * 1024];
    uint64_t count = 0;
    try {
      for (;;) {
        ssize_t n = read(input, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        Need(n >= 0, "Runtime read failed");
        if (!n) break;
        ssize_t offset = 0;
        while (offset < n) {
          ssize_t w = write(output, buffer + offset, n - offset);
          if (w < 0 && errno == EINTR) continue;
          Need(w > 0, "Runtime copy failed (RAM full?)");
          offset += w;
        }
        count += n;
      }
      Need(count == static_cast<uint64_t>(st.st_size), "Runtime file changed while copying");
      LabelAndTimes(output, source, st);
    } catch (...) {
      close(input);
      close(output);
      throw;
    }
    close(input);
    close(output);
    copied[key] = dst;
    ++files;
    bytes += count;
  }
  void CloseSources() {
    for (auto it = sources.rbegin(); it != sources.rend(); ++it)
      if (it->owned) {
        Need(umount(it->mounted_at.c_str()) == 0,
             "Runtime source is still busy: " + it->mounted_at);
        it->owned = false;
      }
  }
  void CopyElfClosure() {
    struct Object {
      string path, destination;
    };
    std::vector<Object> pending;
    for (const auto& item : phase["elf_roots"]) {
      if (item.isMember("provided")) {
        string path = item["provided"].asString();
        Need(test || Ram(path), "Provided runtime executable is not in RAM: " + path);
        pending.push_back({ path, "" });
        continue;
      }
      string label = item["source"].asString(), origin = SourceRoot(label),
             pattern = origin + "/" + Relative(item["path"].asString());
      glob_t matches{};
      int rc = glob(pattern.c_str(), GLOB_NOSORT, nullptr, &matches);
      Need(rc == 0 || rc == GLOB_NOMATCH, "Cannot inspect runtime ELF roots");
      Need(matches.gl_pathc || item.get("optional", false).asBool(),
           "Missing runtime ELF root: " + pattern);
      for (size_t i = 0; i < matches.gl_pathc; ++i) {
        string path = matches.gl_pathv[i];
        pending.push_back({ path, temporary + "/" + label + path.substr(origin.size()) });
      }
      globfree(&matches);
    }
    std::set<string> visited;
    while (!pending.empty()) {
      Object object = pending.back();
      pending.pop_back();
      string resolved = object.destination.empty() ? object.path : Resolve(object.path);
      if (!object.destination.empty() && !Exists(object.destination))
        Copy(object.path, object.destination);
      if (!visited.insert(resolved).second) continue;
      Need(visited.size() < 4096, "Runtime ELF dependency graph is unexpectedly large");
      for (const auto& library : ElfDependencies(resolved)) {
        bool found = false;
        for (const auto& search : phase["library_search"]) {
          if (search.isMember("snapshot")) {
            string path = temporary + "/" + Relative(search["snapshot"].asString()) + "/" + library;
            if (Exists(path)) {
              pending.push_back({ path, "" });
              found = true;
              break;
            }
          } else if (search.isMember("provided")) {
            string path = search["provided"].asString() + "/" + library;
            if (Exists(path)) {
              Need(test || Ram(path), "Provided runtime library is not in RAM: " + path);
              pending.push_back({ path, "" });
              found = true;
              break;
            }
          } else {
            string label = search["source"].asString(),
                   directory = Relative(search["path"].asString()),
                   relative = (directory.empty() ? "" : directory + "/") + library;
            string path = SourceRoot(label) + "/" + relative;
            if (Exists(path)) {
              pending.push_back({ path, temporary + "/" + label + "/" + relative });
              found = true;
              break;
            }
          }
        }
        Need(found, "Missing runtime ELF dependency " + library + " required by " + object.path);
      }
    }
  }
  void Prepare() {
    if (Read(root + "/.ready") == identity) {
      if (!test)
        for (const auto& item : phase["partitions"]) {
          Source s;
          s.name = item["name"].asString();
          string part = item.get("partition", s.name).asString();
          Need(Token(s.name) && Token(part), "Invalid source partition");
          string suffix = item.get("slotted", true).asBool() ? slot : "";
          for (const string& prefix :
               { string("/dev/block/mapper/"), string("/dev/block/bootdevice/by-name/"),
                 string("/dev/block/by-name/") }) {
            struct stat st{};
            string block = prefix + part + suffix;
            if (stat(block.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
              s.block = block;
              break;
            }
          }
          sources.push_back(s);
        }
      Publish();
      return;
    }
    // A flash/transition can stop an in-progress bootstrap. Once we own the
    // phase lock, discard its unpublished RAM-only partial copy before retrying.
    DIR* leftovers = opendir(base.c_str());
    Need(leftovers, "Cannot inspect runtime cache");
    std::vector<string> stale;
    string prefix = "." + name + ".building-";
    while (dirent* entry = readdir(leftovers)) {
      string item = entry->d_name;
      if (item.compare(0, prefix.size(), prefix) == 0) stale.push_back(base + "/" + item);
    }
    closedir(leftovers);
    for (const auto& item : stale) {
      for (const auto& m : Mounts())
        Need(!Below(m.target, item), "A partial runtime directory contains a mount");
      RemoveTree(item);
    }
    Need(!Exists(root),
         "An incomplete or different runtime snapshot already exists; reboot recovery");
    temporary = base + "/." + name + ".building-" + std::to_string(getpid());
    Need(!Exists(temporary), "Runtime staging directory already exists");
    Directories(temporary);
    try {
      OpenSources();
      for (const auto& item : phase["copies"]) {
        string origin = SourceRoot(item["source"].asString()),
               destination = Relative(item["to"].asString());
        Json::Value paths = item["paths"];
        if (paths.isNull()) paths.append(item["path"]);
        std::vector<string> matches;
        for (const auto& value : paths) {
          string pattern = origin + "/" + Relative(value.asString());
          glob_t result{};
          int rc = glob(pattern.c_str(), GLOB_NOSORT, nullptr, &result);
          Need(rc == 0 || rc == GLOB_NOMATCH, "Cannot expand runtime files");
          for (size_t i = 0; i < result.gl_pathc; ++i) matches.push_back(result.gl_pathv[i]);
          globfree(&result);
          if (!matches.empty()) break;
        }
        if (matches.empty()) {
          Need(item.get("optional", false).asBool(),
               "Required runtime file set is missing: " + item["source"].asString() + "/" +
                   item.get("path", paths.toStyledString()).asString());
          continue;
        }
        bool many = item.get("contents", false).asBool();
        for (const auto& source : matches)
          Copy(source, temporary + "/" + destination + (many ? "/" + Base(source) : ""));
      }
      CopyElfClosure();
      // No source partition may remain mounted when consumers are started.
      CloseSources();
      std::ofstream ready(temporary + "/.ready");
      ready << identity;
      ready.close();
      Need(ready.good(), "Cannot commit runtime snapshot");
      Need(rename(temporary.c_str(), root.c_str()) == 0, "Cannot publish runtime snapshot");
      temporary.clear();
    } catch (...) {
      for (auto& s : sources)
        if (s.owned && umount(s.mounted_at.c_str()) == 0) s.owned = false;
      if (!temporary.empty()) RemoveTree(temporary);
      throw;
    }
    fprintf(stderr, "AERA RAM runtime: %s/%s copied %llu files, %.1f MB\n", id.c_str(),
            name.c_str(), static_cast<unsigned long long>(files), bytes / 1000000.0);
    Publish();
  }
  void Bind(const string& from, const string& target) {
    Need(Below(from, root) && Exists(from), "Missing staged binding source");
    Need(target.size() > 1 && target[0] == '/' && target.find("..") == string::npos,
         "Invalid binding target");
    bool allowed = false;
    for (const auto& prefix : phase["allowed_targets"])
      if (Below(target, prefix.asString())) allowed = true;
    Need(allowed, "Binding target was not declared by the device");
    if (test) return;
    auto mounts = Mounts();
    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it)
      if (it->target == target) {
        struct stat a{}, b{};
        Need(stat(from.c_str(), &a) == 0 && stat(target.c_str(), &b) == 0,
             "Cannot inspect existing binding");
        if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) return;
        // Only an explicitly declared source partition may be replaced. Never
        // detach a busy mount or hide a physical mount below another mount.
        bool ours = false;
        for (const auto& s : sources) {
          struct stat block{};
          if (!s.block.empty() && stat(s.block.c_str(), &block) == 0 && block.st_rdev == b.st_dev)
            ours = true;
        }
        Need(ours, "Refusing to replace an unrelated existing mount: " + target);
        Need(umount(target.c_str()) == 0, "Existing runtime mount is busy: " + target);
        replaced.push_back(*it);
      }
    string parent = Parent(target), ancestor = parent;
    while (!Exists(ancestor) && ancestor != "/") ancestor = Parent(ancestor);
    Need(Ram(ancestor), "Runtime binding target is not on the recovery RAM filesystem: " + target);
    Directories(parent);
    struct stat st{};
    Need(stat(from.c_str(), &st) == 0, "Cannot inspect binding source");
    struct stat current{};
    if (lstat(target.c_str(), &current) == 0 && S_ISLNK(current.st_mode))
      Need(unlink(target.c_str()) == 0, "Cannot replace RAM alias");
    if (S_ISDIR(st.st_mode))
      Directories(target);
    else if (!Exists(target)) {
      int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
      Need(fd >= 0, "Cannot create binding target");
      close(fd);
    }
    Need(mount(from.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) == 0,
         "Cannot bind RAM runtime file");
    published.push_back(target);
    Need(mount(nullptr, target.c_str(), nullptr, MS_PRIVATE, nullptr) == 0,
         "Cannot isolate RAM binding");
    Need(mount(nullptr, target.c_str(), nullptr,
               MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, nullptr) == 0,
         "Cannot protect RAM binding");
  }
  void Publish() {
    if (copy_only) return;
    if (!test)
      for (const auto& prefix : phase["private_mounts"]) {
        for (const auto& m : Mounts())
          if (m.target == prefix.asString())
            Need(mount(nullptr, m.target.c_str(), nullptr, MS_PRIVATE, nullptr) == 0,
                 "Cannot isolate runtime parent mount");
      }
    try {
      for (const auto& item : phase["bindings"]) {
        string pattern = root + "/" + Relative(item["from"].asString());
        glob_t list{};
        int rc = glob(pattern.c_str(), GLOB_NOSORT, nullptr, &list);
        Need(rc == 0 || rc == GLOB_NOMATCH, "Cannot inspect staged bindings");
        if (!list.gl_pathc) {
          globfree(&list);
          Need(item.get("optional", false).asBool(),
               "Required binding source is missing: " + pattern);
          continue;
        }
        for (size_t i = 0; i < list.gl_pathc; ++i) {
          string from = list.gl_pathv[i];
          Bind(from, item["to"].asString() +
                         (item.get("contents", false).asBool() ? "/" + Base(from) : ""));
        }
        globfree(&list);
      }
    } catch (...) {
      for (auto it = published.rbegin(); it != published.rend(); ++it) umount(it->c_str());
      for (auto it = replaced.rbegin(); it != replaced.rend(); ++it)
        mount(it->source.c_str(), it->target.c_str(), it->fs.c_str(),
              MS_RDONLY | MS_NOSUID | MS_NODEV, it->fs == "ext4" ? "noload" : nullptr);
      throw;
    }
  }
};
}  // namespace
int main(int argc, char** argv) {
  try {
    bool copy_only = argc == 4 && string(argv[3]) == "--copy-only";
    Need(argc == 3 || copy_only
#ifdef AERA_RUNTIME_RAM_HOST_TEST
             || argc == 5
#endif
         ,
         "Usage: aera-runtime-stage CONFIG PHASE [--copy-only]");
    auto start = std::chrono::steady_clock::now();
    Stage stage(argv[1], argv[2], argc == 5 ? argv[3] : "", argc == 5 ? argv[4] : "");
    stage.copy_only = copy_only;
    stage.Prepare();
    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fprintf(stderr, "AERA RAM runtime: %s ready in %.2f seconds\n", argv[2], elapsed);
    printf("%s\n", stage.root.c_str());
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "AERA RAM runtime: ERROR: %s\n", e.what());
    return 1;
  }
}
