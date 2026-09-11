// SPDX-License-Identifier: Apache-2.0
#include "session.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recovery_ui2::web {
void Session::Unmap() {
  if (shared_) munmap(const_cast<uint8_t *>(shared_), kSharedBytes);
  shared_ = nullptr;
}
void Session::Close() {
  if (control_ >= 0) close(control_);
  control_ = -1;
  can_back_ = can_forward_ = false; progress_ = 0; sequence_ = 0;
  keyboard_request_ = KeyboardRequest::kNone; keyboard_purpose_ = 0;
  frame_pending_ = false;
}
void Session::Fail(const char *reason) { status_ = reason; Close(); }
bool Session::Adopt(int frame_fd, int control_fd) {
  Close();
  Unmap();
  struct stat info{}; int type = 0; socklen_t size = sizeof(type);
  const int seals = fcntl(frame_fd, F_GET_SEALS);
  const bool valid = frame_fd >= 0 && control_fd >= 0 && frame_fd != control_fd &&
      !fstat(frame_fd, &info) && S_ISREG(info.st_mode) && info.st_size == kSharedBytes &&
      seals >= 0 && (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) == (F_SEAL_SHRINK | F_SEAL_GROW) &&
      !getsockopt(control_fd, SOL_SOCKET, SO_TYPE, &type, &size) && type == SOCK_SEQPACKET;
  if (!valid) {
    if (frame_fd >= 0) close(frame_fd);
    if (control_fd >= 0 && control_fd != frame_fd) close(control_fd);
    status_ = "Invalid isolated browser channel."; return false;
  }
  auto *mapped = mmap(nullptr, kSharedBytes, PROT_READ, MAP_SHARED, frame_fd, 0);
  close(frame_fd);
  if (mapped == MAP_FAILED) { close(control_fd); status_ = "Could not map browser pixels."; return false; }
  shared_ = static_cast<uint8_t *>(mapped); control_ = control_fd;
  if (fcntl(control_, F_SETFD, FD_CLOEXEC) < 0) { Fail("Could not protect browser channel."); return false; }
  status_ = "Starting browser";
  return true;
}
bool Session::Write(const Message &message) {
  if (!Connected()) return false;
  const auto count = send(control_, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL);
  if (count == sizeof(message)) return true;
  Fail("Browser input channel stopped responding."); return false;
}
bool Session::Send(Kind kind, int x, int y, uint32_t value, const char *text) {
  Message message; message.kind = kind; message.x = x; message.y = y; message.value = value;
  if (!text || strlen(text) >= sizeof(message.text)) return false;
  memcpy(message.text, text, strlen(text) + 1);
  if (!Valid(message, false)) return false;
  return Write(message);
}
bool Session::AcknowledgeFrame() {
  if (!frame_pending_) return Connected();
  Message ack; ack.kind = Kind::kAck; ack.sequence = sequence_;
  if (!Write(ack)) return false;
  frame_pending_ = false;
  return true;
}
KeyboardRequest Session::TakeKeyboardRequest(uint32_t *purpose) {
  const auto request = keyboard_request_;
  if (purpose) *purpose = keyboard_purpose_;
  keyboard_request_ = KeyboardRequest::kNone;
  return request;
}
bool Session::Poll() {
  bool changed = false;
  for (int i = 0; i < 8 && Connected(); ++i) {
    Message message;
    const auto count = recv(control_, &message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count < 0 && errno == EINTR) continue;
    if (count != sizeof(message) || !Valid(message, true)) {
      Fail("Browser closed or sent an invalid response."); break;
    }
    if (message.kind == Kind::kFrame) {
      if (frame_pending_ || message.sequence != sequence_ + 1) {
        Fail("Browser frame sequence mismatch."); break;
      }
      sequence_ = message.sequence;
      // The sequence selects one of two fixed, sealed slots. LVGL consumes this
      // immutable slot before the next tick acknowledges it; the worker can
      // then only write the other slot. No engine-controlled descriptor,
      // allocation size or process address crosses the boundary.
      frame_pending_ = true; changed = true;
    } else if (message.kind == Kind::kStatus) {
      progress_ = message.value; can_back_ = message.x; can_forward_ = message.y;
      status_ = message.text;
    } else if (message.kind == Kind::kKeyboardShow) {
      keyboard_purpose_ = message.value;
      keyboard_request_ = KeyboardRequest::kShow;
    } else if (message.kind == Kind::kKeyboardHide) {
      keyboard_request_ = KeyboardRequest::kHide;
    } else status_ = message.text;
  }
  return changed;
}
}  // namespace recovery_ui2::web
