/* SPDX-License-Identifier: Apache-2.0 */
#include "aeraui/kernel_settings.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace aeraui::kernel;
int main() {
  char temp[] = "/tmp/aera-kernel-test-XXXXXX"; assert(mkdtemp(temp));
  const std::string root = temp;
  auto put = [&](const std::string &path, const std::string &value) {
    std::filesystem::create_directories(std::filesystem::path(root + path).parent_path());
    std::ofstream(root + path) << value << '\n';
  };
  std::filesystem::create_directories(root + "/tmp");
  put("/proc/sys/net/ipv4/tcp_available_congestion_control", "reno cubic bbr");
  put("/proc/sys/net/ipv4/tcp_congestion_control", "cubic");
  const std::string cpu = "/sys/devices/system/cpu/cpufreq/policy0/";
  put(cpu + "related_cpus", "0 1 2 3");
  put(cpu + "scaling_available_governors", "schedutil performance powersave");
  put(cpu + "scaling_governor", "schedutil");
  put(cpu + "scaling_available_frequencies", "1000000 1500000 2000000");
  put(cpu + "cpuinfo_min_freq", "1000000"); put(cpu + "cpuinfo_max_freq", "2000000");
  put(cpu + "scaling_min_freq", "1000000"); put(cpu + "scaling_max_freq", "2000000");
  put("/sys/block/sda/device/model", "Test UFS"); put("/sys/block/sda/size", "10000000");
  put("/sys/block/sda/queue/scheduler", "none [mq-deadline] kyber bfq");
  put("/sys/block/sda/queue/read_ahead_kb", "128");
  put("/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies", "800000000 400000000 200000000");
  put("/sys/class/kgsl/kgsl-3d0/min_pwrlevel", "2");
  put("/sys/class/kgsl/kgsl-3d0/max_pwrlevel", "0");
  Settings kernel(root);
  auto find = [&](const std::string &name, const std::string &group = "") {
    for (const auto &setting : kernel.Discover())
      if (setting.name == name && (group.empty() || setting.group == group)) return setting;
    assert(false); return Setting{};
  };
  assert(find("Congestion control").choices.size() == 3);
  assert(find("I/O scheduler").current == "mq-deadline");
  std::string error;
  assert(!kernel.Apply("/proc/evil", "1", &error));
  assert(!kernel.Apply("net.tcp", "bbr;reboot", &error));
  assert(kernel.Apply("net.tcp", "bbr", &error));
  kernel.Remember("net.tcp", "bbr");
  assert(find("Congestion control").current == "bbr");
  assert(find("Congestion control").baseline == "cubic");
  Settings restarted(root);
  for (const auto &setting : restarted.Discover())
    if (setting.id == "net.tcp") assert(setting.baseline == "cubic");
  auto minimum = find("min frequency (kHz)", "CPU");
  auto maximum = find("max frequency (kHz)", "CPU");
  assert(!kernel.Apply(minimum.id, "3000000", &error));
  assert(kernel.Apply(maximum.id, "1500000", &error));
  kernel.Remember(maximum.id, "1500000");
  assert(!kernel.Apply(minimum.id, "2000000", &error));
  assert(kernel.Apply("kgsl.max", "400000000", &error));
  kernel.Remember("kgsl.max", "400000000");
  assert(find("max frequency (Hz)", "GPU").current == "400000000");
  assert(!kernel.Apply("kgsl.min", "800000000", &error));
  assert(!kernel.Apply("kgsl.max", "999999999", &error));
  const auto read_ahead = find("Read-ahead (KB)");
  assert(!kernel.Apply(read_ahead.id, "999999999", &error));
  assert(kernel.Apply(read_ahead.id, "1024", &error));
  kernel.SessionOnly(read_ahead.id);
  assert(kernel.ResetAll(&error));
  assert(find("Congestion control").current == "cubic");
  assert(find("Read-ahead (KB)").current == "128");
  // A main config with no overrides removes an earlier override immediately.
  assert(kernel.Apply("net.tcp", "bbr", &error)); kernel.Remember("net.tcp", "bbr");
  kernel.Load({}); assert(find("Congestion control").current == "cubic");
  kernel.Load({{"net.tcp", "bbr"}}); kernel.Poll();
  assert(find("Congestion control").current == "bbr");
  assert(kernel.Apply("net.tcp", "reno", &error)); kernel.SessionOnly("net.tcp");
  kernel.Poll(); assert(find("Congestion control").current == "reno");
  kernel.Skip(); kernel.Load({{"net.tcp", "cubic"}}); kernel.Poll();
  assert(find("Congestion control").current == "reno");
  Overrides decoded;
  const Overrides values{{"net.tcp", "bbr"}, {"cpu.test", "1234"}};
  assert(Decode(Encode(values), &decoded) && values == decoded);
  assert(!Decode("2", &decoded));
  assert(!Decode("1 \"a\" \"1\" \"a\" \"2\"", &decoded));
  assert(!Decode("1 \"a\" \"1\" garbage", &decoded));
  assert(!Decode("1 \"a\" \"hello\nworld\"", &decoded));
  kernel.Load({{"retired.setting", "old"}});
  bool unavailable = false;
  for (const auto &setting : kernel.Discover())
    if (setting.id == "retired.setting") unavailable = setting.path.empty() && !setting.writable;
  assert(unavailable && kernel.ResetAll(&error));
  std::filesystem::remove_all(root);
}
