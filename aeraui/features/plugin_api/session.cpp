/* SPDX-License-Identifier: Apache-2.0 */
#include "session.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace aeraui::plugin_api {
namespace {
uint64_t NowMs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
}  // namespace

bool Session::Adopt(int control_fd, uint32_t max_version,
                    std::vector<Message> after_ack) {
  Close();
  if (max_version < kProtocolVersion || max_version > kMaxProtocolVersion) {
    if (control_fd >= 0) close(control_fd);
    status_ = "Invalid AERA Host API version.";
    return false;
  }
  int type = 0;
  socklen_t length = sizeof(type);
  if (control_fd < 0 ||
      getsockopt(control_fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 ||
      type != SOCK_SEQPACKET) {
    if (control_fd >= 0) close(control_fd);
    status_ = "Invalid AERA plugin channel.";
    return false;
  }
  const int flags = fcntl(control_fd, F_GETFL);
  if (flags < 0 || fcntl(control_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    close(control_fd);
    status_ = "Could not protect the AERA plugin channel.";
    return false;
  }
  control_ = control_fd;
  negotiated_ = false;
  max_version_ = max_version;
  version_ = kProtocolVersion;
  after_ack_ = std::move(after_ack);
  rate_window_ms_ = NowMs();
  rate_messages_ = 0;
  status_ = "Waiting for plugin handshake";
  return true;
}

bool Session::Write(const Message &message) {
  if (!Connected() || !Valid(message, false, version_)) return false;
  const ssize_t count = send(control_, &message, sizeof(message),
                             MSG_DONTWAIT | MSG_NOSIGNAL);
  if (count == static_cast<ssize_t>(sizeof(message))) return true;
  Fail("The plugin stopped responding.");
  return false;
}

bool Session::Send(Kind kind, uint32_t request_id, uint32_t value,
                   uint32_t flags, const char *title, const char *text) {
  Message message;
  message.version = version_;
  message.kind = kind;
  message.request_id = request_id;
  message.value = value;
  message.flags = flags;
  if (title) snprintf(message.title, sizeof(message.title), "%s", title);
  if (text) snprintf(message.text, sizeof(message.text), "%s", text);
  return Write(message);
}

bool Session::Send(Message message) {
  message.version = version_;
  return Write(message);
}

std::vector<Message> Session::Poll() {
  std::vector<Message> messages;
  for (unsigned packet = 0; packet < 24 && Connected(); ++packet) {
    Message message;
    const ssize_t count = recv(control_, &message, sizeof(message),
                               MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count < 0 && errno == EINTR) continue;
    // Before negotiation a HELLO may carry any version this scene offers.
    const uint32_t expected = negotiated_ ? version_ : message.version;
    if (count != static_cast<ssize_t>(sizeof(message)) ||
        (!negotiated_ && (message.version < kProtocolVersion ||
                          message.version > max_version_)) ||
        !Valid(message, true, expected)) {
      Fail("The plugin closed or sent an invalid message.");
      break;
    }
    // PRESENT is bounded by slot ownership (at most one per slot until
    // FRAME_DONE), so frames do not count against the control rate limit.
    if (message.kind != Kind::kPresent) {
      const uint64_t now = NowMs();
      if (now - rate_window_ms_ >= 1000) {
        rate_window_ms_ = now;
        rate_messages_ = 0;
      }
      if (++rate_messages_ > 128) {
        Fail("The plugin exceeded the message rate limit.");
        break;
      }
    }
    if (!negotiated_) {
      // HELLO advertises the inclusive [minimum, maximum] protocol range; the
      // scene offers exactly max_version_ (a declarative scene speaks 2, a
      // pixel scene 3).
      if (message.kind != Kind::kHello || message.value == 0 ||
          message.value > message.flags || message.value > max_version_ ||
          message.flags < max_version_) {
        Fail(max_version_ >= kProtocolVersion3
                 ? "The plugin does not support AERA Host API 3."
                 : "The plugin does not support AERA Host API 2.");
        break;
      }
      negotiated_ = true;
      version_ = max_version_;
      status_ = "Plugin connected";
      const uint32_t features = version_ >= kProtocolVersion3
          ? kFeatureBackNavigation | kFeaturePixelSurface |
                kFeatureKeyboardInset | kFeatureFilePicker
          : kFeatureMetrics | kFeatureBackNavigation;
      bool sent = Send(Kind::kHelloAck, 0, version_, features);
      for (auto &extra : after_ack_) {
        extra.version = version_;
        sent = sent && Write(extra);
      }
      after_ack_.clear();
      if (!sent || !Send(Kind::kLifecycle, 0,
                         static_cast<uint32_t>(Lifecycle::kResume))) break;
      continue;
    }
    if (message.kind == Kind::kHello) {
      Fail("The plugin repeated its handshake.");
      break;
    }
    messages.push_back(message);
  }
  return messages;
}

void Session::Fail(const char *message) {
  status_ = message;
  if (control_ >= 0) close(control_);
  control_ = -1;
  negotiated_ = false;
}

void Session::Close() {
  if (control_ >= 0) close(control_);
  control_ = -1;
  negotiated_ = false;
  version_ = kProtocolVersion;
  after_ack_.clear();
  rate_window_ms_ = 0;
  rate_messages_ = 0;
}

}  // namespace aeraui::plugin_api
