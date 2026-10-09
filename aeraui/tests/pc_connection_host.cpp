/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_remote/pc_connection.hpp"
#include "../features/update/payload_inspector.hpp"
#include "../features/update/payload_arb.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <thread>

namespace {
std::string storage;
bool locked = false;
}
namespace aeraui {
std::string RecoveryDevice() { return "test-device"; }
std::string RecoveryVersion() { return "PC connection test"; }
std::string RecoverySlot() { return "A"; }
std::string RecoveryBootSlot() { return "a"; }
bool RecoveryDataLocked() { return locked; }
std::string RecoveryStorage() { return storage; }
WifiConnection RecoveryWifiConnection() { return {true, "Test network"}; }
std::vector<Volume> RecoveryVolumes(const std::string &) {
  return {{"USB storage", storage + "/usb"}};
}
std::vector<Volume> RecoveryImageVolumes() {
  return {{"Boot", "/boot", 67108864, false, true, false},
          {"System", "/system", 1073741824, false, false, true},
          {"Modem", "/block/modem", 536870912, false, true, false},
          {"Splash", "/block/splash", 16777216, false, false, false}};
}
}
namespace aera::remote {
std::string Address() { return "127.0.0.1"; }
bool Running() { return false; }
}
// Package parsing is covered by payload_inspector_test. This fixture supplies
// deterministic preflight results and never reads real device firmware.
namespace aeraui::payload {
Info InspectZip(const std::string &path, bool) {
  std::ifstream file(path, std::ios::binary);
  char data[64]{}; file.read(data, sizeof(data));
  const std::string marker(data, static_cast<size_t>(file.gcount()));
  Info info;
  if (marker.rfind("AERA-test-OTA\n", 0) != 0) return info;
  info.is_payload = true; info.valid = true;
  info.target_build = "Infinity-X Test"; info.target_device = "test-device";
  info.device_codename = "infiniti"; info.target_sdk = "Android 16";
  info.security_patch = "2026-10-01"; info.build_id = "TEST.20261009";
  info.system_fingerprint = "test/test-device/infiniti:16/TEST.20261009/1:user/release-keys";
  info.payload_bytes = 128ULL * 1024 * 1024; info.expanded_bytes = 256ULL * 1024 * 1024;
  info.format_version = 2; info.dynamic_partitions = true;
  info.operations = 3; info.operation_types = "raw, zero";
  info.partitions = {{"system", 192ULL * 1024 * 1024, 1}, {"vendor", 64ULL * 1024 * 1024, 1}, {"xbl_config", 4096, 1}};
  info.arb_available = true; info.arb_index = marker.find("upgrade") != std::string::npos ? 3 :
      marker.find("downgrade") != std::string::npos ? 1 : 2;
  info.arb_detail = "Simulated firmware metadata";
  if (marker.find("malformed") != std::string::npos) {
    info.valid = false; info.error = "Simulated malformed payload manifest.";
  }
  return info;
}
std::string Summary(const Info &info) { return info.is_payload ? "Simulated full OTA" : "Installer ZIP"; }
DeviceArb ReadDeviceArb() { return {{true, 2, "Fixture slot A"}, {true, 2, "Fixture slot B"}}; }
ArbDecision CompareArb(bool includes_firmware, const Arb &package, const DeviceArb &) {
  return !includes_firmware ? ArbDecision::NotApplicable : package.index < 2 ? ArbDecision::Downgrade :
      package.index > 2 ? ArbDecision::Upgrade : ArbDecision::Same;
}
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  storage = argv[1];
  std::filesystem::create_directories(storage + "/usb");
  const int port = std::stoi(argv[2]);
  const bool demo = argc > 3 && std::string(argv[3]) == "--demo";
  if (!aera::pc::SetEnabled(true, port)) return 3;
  std::cout << "READY" << std::endl;
  bool run = demo;
  aera::pc::InstallRequest install;
  auto started = std::chrono::steady_clock::now();
  for (;;) {
    pollfd input{0, POLLIN, 0};
    if (poll(&input, 1, 50) > 0 && (input.revents & POLLIN)) {
      std::string command;
      if (!std::getline(std::cin, command) || command == "quit") break;
      aera::pc::ConnectionRequest request;
      if (command == "approve" || command == "remember" || command == "deny") {
        if (aera::pc::TakeConnectionRequest(&request))
          aera::pc::ResolveConnection(request.id, command != "deny", command == "remember");
      } else if (command == "run") run = true;
      else if (command == "hold") run = false;
      else if (command == "busy" || command == "idle") aera::pc::SetRecoveryBusy(command == "busy");
      else if (command == "reboot") {
        std::string target;
        if (aera::pc::TakeRebootRequest(&target)) std::cout << "REBOOT " << target << std::endl;
      }
      else if (command == "off") aera::pc::SetEnabled(false, port, false);
      else if (command == "on") {
        if (!aera::pc::SetEnabled(true, port, false)) return 5;
      }
      else if (command == "lock" || command == "unlock") {
        locked = command == "lock"; aera::pc::RefreshPlatformInfo();
      } else if (command == "restart") {
        aera::pc::SetEnabled(false, port);
        if (!aera::pc::SetEnabled(true, port)) return 4;
      } else if (command == "forget") aera::pc::ForgetComputers();
      else if (command == "prompt") {
        aera::pc::UpdateInstallerPrompt({true, "test-question", "Installer question",
                                      "Continue the simulated installation?", "Continue", "Cancel"});
      } else if (command == "presentation") {
        aeraui::InstallerPresentation presentation;
        presentation.active = true;
        presentation.package_name = "Simulated payload ZIP";
        presentation.device = "test-device";
        presentation.author = "AERA test fixture";
        presentation.stage = 2;
        presentation.stage_count = 5;
        presentation.stage_title = "Reading payload";
        presentation.stage_detail = "No device partitions are accessed.";
        aera::pc::UpdateInstallerPresentation(presentation);
      }
      std::cout << "OK " << command << std::endl;
    }
    if (demo) {
      aera::pc::ConnectionRequest request;
      if (aera::pc::TakeConnectionRequest(&request))
        aera::pc::ResolveConnection(request.id, true, true);
    }
    if (run && install.id.empty() && aera::pc::TakeInstallRequest(&install)) {
      if (!aera::pc::BeginInstall(install.id)) install = {};
      else {
        // Deliberately no RecoveryRunJob: this harness never flashes anything.
        started = std::chrono::steady_clock::now();
        std::cout << "INSTALL " << static_cast<int>(install.job.job) << ' '
                  << install.job.both_slots << ' ' << install.job.path << std::endl;
        std::cout << "VISIBLE " << install.job.show_on_device << std::endl;
      }
    }
    if (!install.id.empty()) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count();
      aera::pc::UpdateInstall(std::min(99L, ms / 30), "Simulated installation",
                            "Test installer: no device partitions are accessed.\n");
      std::string id; bool accepted;
      if (aera::pc::TakePromptAnswer(&id, &accepted)) {
        std::cout << "ANSWER " << id << ' ' << accepted << std::endl;
        aera::pc::UpdateInstallerPrompt({});
      }
      if (ms >= (demo ? 10000 : 3000)) { aera::pc::CompleteInstall(install.id, 0); install = {}; }
    }
  }
  aera::pc::SetEnabled(false, port);
}
