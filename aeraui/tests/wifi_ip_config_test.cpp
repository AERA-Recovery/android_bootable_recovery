/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_wlan_ip.hpp"
#include <cassert>
#include <cstdio>
int main() {
  using namespace aera::wifi;
  int prefix = 0;
  assert(ParseSubnet("24", prefix) && prefix == 24);
  assert(ParseSubnet("255.255.255.0", prefix) && prefix == 24);
  assert(ParseSubnet("255.255.255.255", prefix) && prefix == 32);
  assert(!ParseSubnet("255.0.255.0", prefix));
  assert(!ParseSubnet("0", prefix) && !ParseSubnet("33", prefix));
  assert(!ParseSubnet("24;reboot", prefix));
  IpConfig config; std::string error;
  assert(ValidateIpConfig(config, error));
  config.manual = true;
  assert(!ValidateIpConfig(config, error));
  config.address = "192.168.178.50"; config.gateway = "192.168.178.1";
  config.dns1 = "1.1.1.1"; config.dns2 = "2606:4700:4700::1111";
  assert(ValidateIpConfig(config, error));
  config.gateway = "192.168.0.1"; assert(!ValidateIpConfig(config, error));
  config.gateway.clear(); assert(ValidateIpConfig(config, error));
  for (const char *bad : {"192.168.178.0", "192.168.178.255", "127.0.0.1", "0.0.0.0", "224.0.0.1", "1.2.3.4;reboot"}) {
    config.address = bad; assert(!ValidateIpConfig(config, error));
  }
  config.address = "192.168.178.50";
  config.dns1 = "1.1.1.1\nnameserver 8.8.8.8";
  assert(!ValidateIpConfig(config, error));
  puts("Static IPv4, netmask/prefix, gateway and DNS validation passed");
}
