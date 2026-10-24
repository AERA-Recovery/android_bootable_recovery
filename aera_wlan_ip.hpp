/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <arpa/inet.h>
#include <cstdint>
#include <string>

namespace aera::wifi {
struct IpConfig {
  bool manual = false;
  std::string address, gateway, dns1, dns2;
  int prefix = 24;
};
inline bool ParseSubnet(const std::string &text, int &prefix) {
  if (!text.empty() && text.size() <= 2 && text.find_first_not_of("0123456789") == std::string::npos) {
    int value = 0;
    for (char c : text) value = value * 10 + c - '0';
    if (value >= 1 && value <= 32) { prefix = value; return true; }
    return false;
  }
  in_addr mask{};
  if (inet_pton(AF_INET, text.c_str(), &mask) != 1) return false;
  const uint32_t bits = ntohl(mask.s_addr), inverse = ~bits;
  if (!bits || (inverse & (inverse + 1))) return false;
  prefix = 0;
  for (uint32_t bit = 0x80000000U; bit & bits; bit >>= 1) ++prefix;
  return true;
}
inline bool ValidHost(const std::string &text, uint32_t *value = nullptr) {
  in_addr address{};
  if (inet_pton(AF_INET, text.c_str(), &address) != 1) return false;
  const auto host = ntohl(address.s_addr);
  if ((host >> 24) == 0 || (host >> 24) == 127 || (host >> 24) >= 224) return false;
  if (value) *value = host;
  return true;
}
inline bool ValidDns(const std::string &text) {
  in6_addr address{};
  return text.empty() || ValidHost(text) ||
      (inet_pton(AF_INET6, text.c_str(), &address) == 1 &&
       !IN6_IS_ADDR_UNSPECIFIED(&address) && !IN6_IS_ADDR_MULTICAST(&address));
}
inline bool ValidateIpConfig(const IpConfig &config, std::string &error) {
  error.clear();
  if (!config.manual) return true;
  uint32_t address = 0, gateway = 0;
  if (!ValidHost(config.address, &address)) error = "Enter a valid IPv4 address.";
  else if (config.prefix < 1 || config.prefix > 32) error = "Enter a subnet mask or prefix from 1 to 32.";
  else {
    const uint32_t host_mask = config.prefix == 32 ? 0 : (UINT32_MAX >> config.prefix);
    if (config.prefix <= 30 && (!(address & host_mask) || (address & host_mask) == host_mask))
      error = "The IP address cannot be the network or broadcast address.";
    else if (!config.gateway.empty() &&
        (!ValidHost(config.gateway, &gateway) || gateway == address ||
         ((address ^ gateway) & ~host_mask) != 0 ||
         (config.prefix <= 30 && (!(gateway & host_mask) || (gateway & host_mask) == host_mask))))
      error = "The gateway must be a different address in the same subnet.";
    else if (!ValidDns(config.dns1) || !ValidDns(config.dns2) ||
             (config.dns1.empty() && !config.dns2.empty()))
      error = "Enter valid DNS addresses, starting with DNS 1.";
  }
  return error.empty();
}
}
