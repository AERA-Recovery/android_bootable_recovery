/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aeraui::plugin_api {

class Session final {
 public:
  ~Session() { Close(); }
  // `max_version` is the highest protocol this scene offers: 2 for the
  // declarative surface, 3 for the pixel surface. `after_ack` is sent right
  // after HELLO_ACK and before LIFECYCLE/RESUME (Host API 3's SURFACE).
  bool Adopt(int control_fd, uint32_t max_version = kProtocolVersion,
             std::vector<Message> after_ack = {});
  std::vector<Message> Poll();
  bool Send(Kind kind, uint32_t request_id = 0, uint32_t value = 0,
            uint32_t flags = 0, const char *title = nullptr,
            const char *text = nullptr);
  // Sends a prepared host message (SURFACE) at the negotiated version.
  bool Send(Message message);
  bool Connected() const { return control_ >= 0; }
  bool Negotiated() const { return negotiated_; }
  uint32_t Version() const { return version_; }
  const std::string &Status() const { return status_; }
  void Close();

 private:
  void Fail(const char *message);
  bool Write(const Message &message);
  int control_ = -1;
  bool negotiated_ = false;
  uint32_t max_version_ = kProtocolVersion;
  uint32_t version_ = kProtocolVersion;
  std::vector<Message> after_ack_;
  uint64_t rate_window_ms_ = 0;
  uint32_t rate_messages_ = 0;
  std::string status_;
};

}  // namespace aeraui::plugin_api
