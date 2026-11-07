/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <string>

namespace aera::wifi {

inline bool ValidNetworkName(const std::string& ssid) {
  return !ssid.empty() && ssid.size() <= 32 &&
      std::none_of(ssid.begin(), ssid.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
      });
}

inline bool IsRawPsk(const std::string& password) {
  return password.size() == 64 &&
      std::all_of(password.begin(), password.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
      });
}

// SSIDs are bytes, not trimmed display names. Keep leading/trailing spaces.
inline const char* NetworkInputError(const std::string& ssid,
                                     const std::string& security,
                                     const std::string& password) {
  if (!ValidNetworkName(ssid)) return "Enter a network name of 1 to 32 bytes.";
  if (security == "OPEN") return nullptr;
  const bool raw_psk = (security == "WPA2" || security == "WPA") &&
                       IsRawPsk(password);
  const bool plain = !password.empty() && password.size() <= 63 &&
      std::none_of(password.begin(), password.end(), [](unsigned char c) {
        return c == 0 || c == '\n' || c == '\r';
      });
  if (security == "WPA3")
    return plain ? nullptr : "Enter a password of 1 to 63 bytes.";
  if (security == "WPA2" || security == "WPA")
    return (raw_psk || (plain && password.size() >= 8)) ? nullptr :
        "Use 8 to 63 password bytes or a 64-digit hexadecimal key.";
  return "Unsupported network security.";
}

}  // namespace aera::wifi
