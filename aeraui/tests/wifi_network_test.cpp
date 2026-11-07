/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_wlan_network.hpp"
#include <cassert>
#include <string>

int main() {
  using namespace aera::wifi;
  assert(!NetworkInputError(" Hidden WLAN ", "WPA2", "12345678"));
  assert(!NetworkInputError("Guest", "OPEN", ""));
  assert(!NetworkInputError("SAE", "WPA3", "x"));
  assert(NetworkInputError("", "OPEN", ""));
  assert(NetworkInputError(std::string(33, 'a'), "OPEN", ""));
  assert(NetworkInputError(std::string("a\0b", 3), "OPEN", ""));
  assert(NetworkInputError("bad\nname", "OPEN", ""));
  assert(!NetworkInputError(std::string(32, 'a'), "OPEN", ""));
  assert(!NetworkInputError(u8"????", "WPA2", "pass word"));
  assert(NetworkInputError(std::string(17, '\xc3') + std::string(17, '\xa4'), "OPEN", ""));
  assert(NetworkInputError("WPA2", "WPA2", "1234567"));
  assert(!NetworkInputError("WPA2", "WPA2", std::string(63, 'x')));
  assert(!NetworkInputError("WPA2", "WPA2", std::string(64, 'a')));
  assert(IsRawPsk(std::string(64, 'a')));
  assert(NetworkInputError("WPA2", "WPA2", std::string(64, 'z')));
  assert(NetworkInputError("WPA3", "WPA3", std::string(64, 'a')));
  assert(NetworkInputError("WPA3", "WPA3", ""));
  assert(NetworkInputError("Unsupported", "WEP", "password"));
  assert(NetworkInputError("WPA2", "WPA2", "bad\npassword"));
}
