/* SPDX-License-Identifier: Apache-2.0 */
#include "aeraui/kernel_settings.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits.h>
#include <sstream>
#include <unistd.h>

namespace aeraui::kernel {
namespace {
bool Number(const std::string &s, int64_t *value) {
  if (s.empty() || s.size() > 16 || s.find_first_not_of("0123456789") != std::string::npos) return false;
  char *end = nullptr; errno = 0;
  const auto n = strtoll(s.c_str(), &end, 10);
  if (errno || !end || *end) return false;
  *value = n; return true;
}
std::vector<std::string> Words(const std::string &text) {
  std::istringstream input(text); std::vector<std::string> result; std::string word;
  while (input >> word) result.push_back(word);
  return result;
}
std::vector<std::string> Children(const std::string &path) {
  std::vector<std::string> result;
  auto *dir = opendir(path.c_str()); if (!dir) return result;
  while (const auto *entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") result.push_back(name);
  }
  closedir(dir); std::sort(result.begin(), result.end()); return result;
}
std::string Identity(const std::string &s) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : s) { hash ^= c; hash *= 1099511628211ULL; }
  std::ostringstream out; out << std::hex << hash; return out.str();
}
bool Safe(const std::string &s) {
  return !s.empty() && s.size() <= 256 &&
      std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 32 && c < 127; });
}
}
std::string Encode(const Overrides &values) {
  std::ostringstream output; output << 1;
  for (const auto &[key, value] : values) output << ' ' << std::quoted(key) << ' ' << std::quoted(value);
  return output.str();
}
bool Decode(const std::string &text, Overrides *values) {
  if (!values || text.size() > 32768) return false;
  if (text.empty()) { values->clear(); return true; }
  std::istringstream input(text); int schema = 0;
  if (!(input >> schema) || schema != 1) return false;
  Overrides parsed; input >> std::ws;
  while (!input.eof()) {
    std::string key, value;
    if (!(input >> std::quoted(key) >> std::quoted(value)) || !Safe(key) || !Safe(value) ||
        parsed.size() >= 128 || !parsed.emplace(key, value).second) return false;
    input >> std::ws;
  }
  *values = std::move(parsed); return true;
}
Settings::Settings(std::string root) : root_(std::move(root)) {
  std::ifstream input(root_ + "/tmp/aera-kernel-defaults.conf");
  std::string line;
  if (std::getline(input, line)) Decode(line, &baselines_);
}
std::string Settings::Read(const std::string &path) const {
  std::ifstream input(root_ + path); std::string line;
  if (!std::getline(input, line) || line.size() > 8192) return {};
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
  return line;
}
std::vector<Setting> Settings::Discover() {
  std::vector<Setting> found;
  const auto previous_defaults = baselines_.size();
  auto add = [&](const std::string &id, const std::string &group, const std::string &device,
                 const std::string &name, const std::string &path,
                 std::vector<std::string> choices, int64_t lo = 0, int64_t hi = 0) {
    auto current = Read(path); if (current.empty()) return;
    if (id.rfind("kgsl.", 0) == 0) {
      int64_t level;
      if (!Number(current, &level) || level >= static_cast<int64_t>(choices.size())) return;
      current = choices[level];
    }
    if (name == "I/O scheduler") {
      for (auto &choice : choices) {
        if (choice.front() == '[' && choice.back() == ']') {
          choice = choice.substr(1, choice.size() - 2); current = choice;
        }
      }
      if (current.find(' ') != std::string::npos) return;
    }
    if (choices.empty()) { int64_t n; if (!Number(current, &n) || hi <= lo) return; }
    else if (std::find(choices.begin(), choices.end(), current) == choices.end()) choices.push_back(current);
    baselines_.emplace(id, current);
    const auto saved = saved_.find(id);
    const auto error = errors_.find(id);
    found.push_back({id, group, device, name, path, current, baselines_.at(id),
        saved == saved_.end() ? "" : saved->second,
        skipped_ ? "Skipped for this session" : error == errors_.end() ? "" : error->second,
        std::move(choices), lo, hi, access((root_ + path).c_str(), W_OK) == 0});
  };
  add("net.tcp", "Network", "TCP", "Congestion control", "/proc/sys/net/ipv4/tcp_congestion_control",
      Words(Read("/proc/sys/net/ipv4/tcp_available_congestion_control")));
  add("net.ecn", "Network", "TCP", "ECN", "/proc/sys/net/ipv4/tcp_ecn", {"0", "1", "2"});
  add("net.mtu_probe", "Network", "TCP", "MTU probing", "/proc/sys/net/ipv4/tcp_mtu_probing", {"0", "1", "2"});
  add("net.idle", "Network", "TCP", "Slow start after idle", "/proc/sys/net/ipv4/tcp_slow_start_after_idle", {"0", "1"});
  add("net.fastopen", "Network", "TCP", "TCP Fast Open", "/proc/sys/net/ipv4/tcp_fastopen", {"0", "1", "2", "3"});
  for (const char *name : {"rmem_max", "wmem_max"})
    add(std::string("net.") + name, "Network", "Socket buffers", name,
        std::string("/proc/sys/net/core/") + name, {}, 4096, 67108864);
  for (const auto &disk : Children(root_ + "/sys/block")) {
    const std::string base = "/sys/block/" + disk;
    char resolved[PATH_MAX];
    if (!realpath((root_ + base).c_str(), resolved)) continue;
    const std::string location = resolved;
    if (disk.rfind("loop", 0) == 0 || disk.rfind("ram", 0) == 0 ||
        disk.rfind("dm-", 0) == 0 || disk.rfind("zram", 0) == 0) continue;
    const std::string model = Read(base + "/device/model");
    const auto key = "block." + Identity(location.substr(root_.size()) + "|" + model + "|" +
        Read(base + "/device/serial") + "|" + Read(base + "/size"));
    const std::string device = disk + (model.empty() ? "" : " / " + model);
    add(key + ".scheduler", "Storage", device, "I/O scheduler", base + "/queue/scheduler",
        Words(Read(base + "/queue/scheduler")));
    add(key + ".read_ahead", "Storage", device, "Read-ahead (KB)", base + "/queue/read_ahead_kb", {}, 0, 16384);
    add(key + ".requests", "Storage", device, "Queue depth", base + "/queue/nr_requests", {}, 4, 4096);
    add(key + ".affinity", "Storage", device, "I/O completion affinity", base + "/queue/rq_affinity", {"0", "1", "2"});
  }
  for (const auto &policy : Children(root_ + "/sys/devices/system/cpu/cpufreq")) {
    if (policy.rfind("policy", 0) != 0) continue;
    const std::string base = "/sys/devices/system/cpu/cpufreq/" + policy;
    const std::string cpus = Read(base + "/related_cpus");
    if (cpus.empty()) continue;
    const std::string key = "cpu." + Identity(cpus);
    const std::string device = "CPU " + cpus;
    add(key + ".governor", "CPU", device, "Governor", base + "/scaling_governor",
        Words(Read(base + "/scaling_available_governors")));
    int64_t lo, hi;
    if (Number(Read(base + "/cpuinfo_min_freq"), &lo) && Number(Read(base + "/cpuinfo_max_freq"), &hi)) {
      auto frequencies = Words(Read(base + "/scaling_available_frequencies"));
      for (const char *limit : {"min", "max"}) {
        const std::string path = base + "/scaling_" + limit + "_freq";
        auto available = frequencies;
        for (const auto &v : {Read(base + "/cpuinfo_min_freq"), Read(base + "/cpuinfo_max_freq")})
          if (std::find(available.begin(), available.end(), v) == available.end()) available.push_back(v);
        add(key + "." + limit, "CPU", device, std::string(limit) + " frequency (kHz)", path,
            frequencies.empty() ? std::vector<std::string>{} : available, lo, hi);
      }
    }
  }
  for (const auto &node : Children(root_ + "/sys/class/devfreq")) {
    const std::string base = "/sys/class/devfreq/" + node;
    // Generic devfreq may also expose memory/interconnect controllers. Restrict
    // tuning to recognized GPU devices rather than guessing at arbitrary nodes.
    if (node.find("gpu") == std::string::npos && node.find("kgsl") == std::string::npos &&
        node.find("mali") == std::string::npos) continue;
    const std::string key = "gpu." + Identity(node);
    add(key + ".governor", "GPU", node, "Governor", base + "/governor", Words(Read(base + "/available_governors")));
    const auto frequencies = Words(Read(base + "/available_frequencies"));
    if (!frequencies.empty()) {
      for (const char *limit : {"min", "max"})
        add(key + "." + limit, "GPU", node, std::string(limit) + " frequency (Hz)",
            base + "/" + limit + "_freq", frequencies);
    }
  }
  // Qualcomm KGSL exposes a discrete power-level table on some kernels that
  // have no generic devfreq attributes. Never expose voltages or thermal knobs.
  const std::string kgsl = "/sys/class/kgsl/kgsl-3d0";
  const auto gpu_frequencies = Words(Read(kgsl + "/gpu_available_frequencies"));
  if (!gpu_frequencies.empty()) {
    for (const char *limit : {"min", "max"})
      add(std::string("kgsl.") + limit, "GPU", "Qualcomm GPU", std::string(limit) + " frequency (Hz)",
          kgsl + "/" + limit + "_pwrlevel", gpu_frequencies);
  }
  add("vm.cache_pressure", "Memory", "Virtual memory", "VFS cache pressure", "/proc/sys/vm/vfs_cache_pressure", {}, 1, 1000);
  // Swappiness is meaningful only when swap is actually active.
  std::ifstream swaps(root_ + "/proc/swaps"); std::string line; std::getline(swaps, line);
  if (std::getline(swaps, line) && !line.empty())
    add("vm.swappiness", "Memory", "Virtual memory", "Swappiness", "/proc/sys/vm/swappiness", {}, 0, 200);
  if (baselines_.size() != previous_defaults) {
    // /tmp survives UI restarts but not a real reboot. Preserve the actual
    // pre-override defaults without carrying them into another kernel boot.
    const std::string path = root_ + "/tmp/aera-kernel-defaults.conf";
    const int fd = open((path + ".tmp").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
      const std::string data = Encode(baselines_);
      const bool ok = write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size());
      close(fd);
      if (ok) rename((path + ".tmp").c_str(), path.c_str());
      else unlink((path + ".tmp").c_str());
    }
  }
  for (const auto &entry : saved_) {
    const auto &id = entry.first;
    if (std::none_of(found.begin(), found.end(), [&](const auto &setting) { return setting.id == id; }))
      found.push_back({id, "Unavailable", id, "Unavailable saved override", "", "", "", entry.second,
          "Not available on this device", {}, 0, 0, false});
  }
  return found;
}
bool Settings::Write(const std::string &path, const std::string &value, std::string *error) {
  const int fd = open((root_ + path).c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) { if (error) *error = strerror(errno); return false; }
  const std::string text = value + "\n";
  ssize_t n;
  do { n = write(fd, text.data(), text.size()); } while (n < 0 && errno == EINTR);
  const int reason = errno; const bool ok = n == static_cast<ssize_t>(text.size()); close(fd);
  if (!ok && error) *error = strerror(reason);
  return ok;
}
bool Settings::Apply(const std::string &id, const std::string &value, std::string *error) {
  return ApplyKnown(Discover(), id, value, error);
}
bool Settings::ApplyKnown(const std::vector<Setting> &all, const std::string &id,
                          const std::string &value, std::string *error) {
  auto fail = [&](const std::string &message) { errors_[id] = message; if (error) *error = message; return false; };
  const auto it = std::find_if(all.begin(), all.end(), [&](const auto &s) { return s.id == id; });
  if (it == all.end()) return fail("Not available on this device");
  if (!it->writable) return fail("Read-only kernel setting");
  int64_t number;
  if (!it->choices.empty()) {
    if (std::find(it->choices.begin(), it->choices.end(), value) == it->choices.end())
      return fail("Value is not supported by this kernel");
  } else if (!Number(value, &number) || number < it->minimum || number > it->maximum)
    return fail("Value is outside the allowed range");
  const bool minimum = id.size() >= 4 && id.compare(id.size() - 4, 4, ".min") == 0;
  const bool maximum = id.size() >= 4 && id.compare(id.size() - 4, 4, ".max") == 0;
  if ((minimum || maximum) && Number(value, &number)) {
    const auto other = id.substr(0, id.size() - 4) + (minimum ? ".max" : ".min");
    const auto limit = std::find_if(all.begin(), all.end(), [&](const auto &s) { return s.id == other; });
    int64_t opposite;
    if (limit != all.end() && Number(limit->current, &opposite)) {
      const bool invalid = minimum ? number > opposite : number < opposite;
      if (invalid) return fail("Minimum and maximum limits would conflict");
    }
  }
  std::string raw = value;
  if (id.rfind("kgsl.", 0) == 0)
    raw = std::to_string(std::find(it->choices.begin(), it->choices.end(), value) - it->choices.begin());
  if (!Write(it->path, raw, error)) return fail(error ? *error : "Kernel rejected this value");
  auto current = Read(it->path);
  if (id.rfind("kgsl.", 0) == 0) {
    int64_t level;
    if (Number(current, &level) && level < static_cast<int64_t>(it->choices.size())) current = it->choices[level];
  }
  if (it->name == "I/O scheduler") {
    const auto selected = "[" + value + "]";
    current = current.find(selected) != std::string::npos ? value : current;
  }
  if (current != value) return fail("Kernel did not apply the requested value");
  applied_[id] = value; errors_.erase(id); return true;
}
void Settings::Remember(const std::string &id, const std::string &value) {
  saved_[id] = value; session_.erase(id);
}
void Settings::Load(const Overrides &values) {
  if (values == saved_) return;
  const auto previous = saved_;
  for (const auto &[id, value] : previous) {
    (void)value;
    if (!values.count(id)) Reset(id, nullptr);
  }
  saved_ = values;
  for (auto it = applied_.begin(); it != applied_.end(); )
    if (!saved_.count(it->first) || saved_.at(it->first) != it->second) it = applied_.erase(it);
    else ++it;
}
void Settings::Poll() {
  if (skipped_ || saved_.empty()) return;
  if (std::all_of(saved_.begin(), saved_.end(), [&](const auto &value) {
        const auto applied = applied_.find(value.first);
        return session_.count(value.first) ||
            (applied != applied_.end() && applied->second == value.second);
      })) return;
  // Discover once before writes so defaults are captured before any override.
  auto settings = Discover();
  for (const auto &[id, value] : saved_)
    if (!session_.count(id) && (!applied_.count(id) || applied_.at(id) != value)) {
      if (ApplyKnown(settings, id, value, nullptr))
        for (auto &setting : settings) if (setting.id == id) setting.current = value;
    }
}
bool Settings::Reset(const std::string &id, std::string *error) {
  const auto all = Discover();
  const bool available = std::any_of(all.begin(), all.end(), [&](const auto &setting) {
    return setting.id == id && !setting.path.empty();
  });
  const auto baseline = baselines_.find(id);
  if (!available || baseline == baselines_.end()) {
    saved_.erase(id); applied_.erase(id); errors_.erase(id); session_.erase(id); return true;
  }
  if (!Apply(id, baseline->second, error)) return false;
  saved_.erase(id); applied_.erase(id); errors_.erase(id); session_.erase(id); return true;
}
bool Settings::ResetAll(std::string *error) {
  // Multiple passes let frequency pairs return to defaults in a valid order.
  for (int pass = 0; pass < 3 && (!saved_.empty() || !applied_.empty()); ++pass) {
    auto before = saved_;
    before.insert(applied_.begin(), applied_.end());
    for (const auto &[id, value] : before) { (void)value; Reset(id, error); }
  }
  return saved_.empty() && applied_.empty();
}
}  // namespace aeraui::kernel
