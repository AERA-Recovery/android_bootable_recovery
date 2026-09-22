/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>
#include <vector>

class AeraSecrets {
 public:
  struct AdbDevice {
    std::string fingerprint;
    std::string name;
    std::string key;
    long long last_seen = 0;
  };

  static bool SetWlanNetwork(const std::string& ssid,
                             const std::string& password,
                             const std::string& security);
  static bool GetWlanNetwork(const std::string& ssid,
                             std::string& password,
                             std::string& security);
  static bool DeleteWlanNetwork(const std::string& ssid);
  static bool ListWlanNetworks(std::vector<std::string>& ssids);

  static bool SetNasPassword(const std::string& password);
  static bool ClearNasPassword();
  static bool GetNasPassword(std::string& password);
  static bool HasNasPassword();

  static bool AddAdbDevice(const std::string& key,
                           const std::string& name = "");
  static bool ListAdbDevices(std::vector<AdbDevice>& devices);
  static bool DeleteAdbDevice(const std::string& id,
                              std::string& removed_key);
};
