/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <net/if.h>
#include <poll.h>
#include <resolv.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <strings.h>
#include <thread>

namespace aeraui::public_ip {
inline bool PublicV4(in_addr address) {
  const uint32_t ip = ntohl(address.s_addr);
  return (ip >> 24) != 0 && (ip >> 24) != 10 && (ip >> 24) != 127 && (ip >> 24) < 224 &&
      (ip >> 22) != 0x191 && (ip >> 16) != 0xa9fe && (ip >> 20) != 0xac1 &&
      (ip >> 16) != 0xc0a8 && (ip >> 17) != 0x6309 &&
      (ip >> 8) != 0xc00000 && (ip >> 8) != 0xc00002 &&
      (ip >> 8) != 0xc63364 && (ip >> 8) != 0xcb0071;
}

inline std::string Parse(const unsigned char *bytes, size_t size, uint16_t id) {
  ns_msg message{};
  if (size > 512 || ns_initparse(bytes, static_cast<int>(size), &message) != 0 ||
      ns_msg_id(message) != id || !ns_msg_getflag(message, ns_f_qr) ||
      ns_msg_getflag(message, ns_f_tc) || ns_msg_getflag(message, ns_f_rcode) ||
      ns_msg_count(message, ns_s_qd) != 1) return {};
  ns_rr question{};
  if (ns_parserr(&message, ns_s_qd, 0, &question) != 0 ||
      strcasecmp(ns_rr_name(question), "myip.opendns.com") ||
      ns_rr_type(question) != ns_t_a || ns_rr_class(question) != ns_c_in) return {};
  for (int i = 0; i < ns_msg_count(message, ns_s_an); ++i) {
    ns_rr record{};
    if (ns_parserr(&message, ns_s_an, i, &record) != 0) return {};
    if (strcasecmp(ns_rr_name(record), "myip.opendns.com") ||
        ns_rr_type(record) != ns_t_a || ns_rr_class(record) != ns_c_in || ns_rr_rdlen(record) != 4) continue;
    in_addr address{};memcpy(&address, ns_rr_rdata(record), sizeof(address));
    char text[INET_ADDRSTRLEN]{};
    if (PublicV4(address) && inet_ntop(AF_INET, &address, text, sizeof(text))) return text;
  }
  return {};
}

inline std::string Query(const std::string &interface, const std::string &source) {
  if (interface.empty() || interface.size() >= IFNAMSIZ || interface.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos) return {};
  sockaddr_in local{};local.sin_family = AF_INET;
  if (inet_pton(AF_INET, source.c_str(), &local.sin_addr) != 1) return {};
  std::array<unsigned char, 512> request{};
  const auto id = static_cast<uint16_t>(arc4random());
  ns_put16(id, request.data());ns_put16(0x0100, request.data() + 2);ns_put16(1, request.data() + 4);
  const int name_size = dn_comp("myip.opendns.com", request.data() + 12, request.size() - 16, nullptr, nullptr);
  if (name_size < 0) return {};
  const size_t length = 12 + name_size + 4;
  ns_put16(ns_t_a, request.data() + length - 4);ns_put16(ns_c_in, request.data() + length - 2);
  // OpenDNS's reflector reports the source seen on this exact interface, not
  // the preferred WLAN route. No resolver settings, routes or modem state change.
  for (const char *server : {"208.67.222.222", "208.67.220.220"}) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return {};
    sockaddr_in remote{};remote.sin_family = AF_INET;remote.sin_port = htons(53);
    inet_pton(AF_INET, server, &remote.sin_addr);
    const bool sent = !setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface.c_str(), interface.size() + 1) &&
        !::bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) &&
        !connect(fd, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) &&
        send(fd, request.data(), length, 0) == static_cast<ssize_t>(length);
    std::string result;
    if (sent) {
      pollfd event{fd, POLLIN, 0};
      if (poll(&event, 1, 1200) > 0 && (event.revents & POLLIN)) {
        std::array<unsigned char, 512> response{};
        const auto size = recv(fd, response.data(), response.size(), MSG_TRUNC);
        if (size > 0 && size <= static_cast<ssize_t>(response.size())) result = Parse(response.data(), size, id);
      }
    }
    close(fd);
    if (!result.empty()) return result;
  }
  return {};
}

struct Snapshot { std::string address; bool pending = false; };
class Sampler {
 public:
  Snapshot Sample(const std::string &interface, const std::string &address, bool connected) {
    using Clock = std::chrono::steady_clock;
    const unsigned index = connected ? if_nametoindex(interface.c_str()) : 0;
    const auto key = index ? interface + ":" + std::to_string(index) + ":" + address : "";
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (key != state_->key) { state_->key = key;state_->address.clear();state_->next = {}; }
    if (key.empty()) return {};
    if (!state_->busy && now >= state_->next) {
      state_->busy = true;
      std::thread([state = state_, key, interface, address, index] {
        const auto result = Query(interface, address);
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->key == key && if_nametoindex(interface.c_str()) == index) {
          state->address = result;
          state->next = Clock::now() + std::chrono::seconds(result.empty() ? 30 : 300);
        }
        state->busy = false;
      }).detach();
    }
    return {state_->address, state_->busy && state_->address.empty()};
  }
 private:
  struct State {
    std::mutex mutex;
    std::string key, address;
    std::chrono::steady_clock::time_point next{};
    bool busy = false;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
};
}  // namespace aeraui::public_ip
