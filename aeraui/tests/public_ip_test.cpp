/* SPDX-License-Identifier: Apache-2.0 */
#include <functional>
using namespace std;
#include "../platform/public_ip.hpp"
#include <cassert>
#include <vector>

namespace {
std::vector<unsigned char> Answer(const char *address) {
  std::vector<unsigned char> packet(512);
  ns_put16(1234, packet.data());
  ns_put16(0x8180, packet.data() + 2);
  ns_put16(1, packet.data() + 4);
  ns_put16(1, packet.data() + 6);
  const int name = dn_comp("myip.opendns.com", packet.data() + 12, 480, nullptr, nullptr);
  assert(name > 0);
  size_t offset = 12 + name;
  ns_put16(ns_t_a, packet.data() + offset);
  ns_put16(ns_c_in, packet.data() + offset + 2);
  offset += 4;
  packet[offset++] = 0xc0;
  packet[offset++] = 12;
  ns_put16(ns_t_a, packet.data() + offset);
  ns_put16(ns_c_in, packet.data() + offset + 2);
  ns_put16(4, packet.data() + offset + 8);
  in_addr parsed{};
  assert(inet_pton(AF_INET, address, &parsed) == 1);
  memcpy(packet.data() + offset + 10, &parsed, 4);
  packet.resize(offset + 14);
  return packet;
}
}

int main() {
  using namespace aeraui::public_ip;
  for (const char *address : {"0.0.0.0", "10.142.1.2", "100.64.1.2", "100.127.255.255",
       "127.0.0.1", "169.254.1.2", "172.16.0.1", "172.31.255.255", "192.168.1.21",
       "192.0.2.1", "198.18.1.1", "198.19.255.255", "198.51.100.1", "203.0.113.1",
       "224.0.0.1", "255.255.255.255"}) {
    const auto packet = Answer(address);
    assert(Parse(packet.data(), packet.size(), 1234).empty());
  }
  auto packet = Answer("8.8.4.4");
  assert(Parse(packet.data(), packet.size(), 1234) == "8.8.4.4");
  assert(Parse(packet.data(), packet.size(), 1235).empty());
  for (size_t size = 0; size < packet.size(); ++size)
    assert(Parse(packet.data(), size, 1234).empty());
  packet[3] = 0x83;
  assert(Parse(packet.data(), packet.size(), 1234).empty());
  packet[3] = 0x80;
  packet[2] |= 2;
  assert(Parse(packet.data(), packet.size(), 1234).empty());
  packet[2] = 0x81;
  packet[13] = 'x';
  assert(Parse(packet.data(), packet.size(), 1234).empty());
  assert(Query("invalid/interface", "10.1.2.3").empty());
  assert(Query("lo", "invalid-address").empty());
  Sampler sampler;
  assert(sampler.Sample("", "", false).address.empty());
  assert(!sampler.Sample("", "", false).pending);
}
