/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string>
namespace aeraui::adb_rescue {
struct Status {
  bool available = false, enabled = false, can_restore = false;
  unsigned authorized_keys = 0;
  std::string message, settings_path, settings_format, usb_functions;
};
struct PublicKey { std::string line, fingerprint, label; };
bool ParsePublicKey(const std::string &input, PublicKey *key, std::string *error);
Status Inspect(const std::string &data_root = "/data");
bool Enable(const std::string &public_key, std::string *error, const std::string &data_root = "/data");
bool Restore(std::string *error, const std::string &data_root = "/data");
}
