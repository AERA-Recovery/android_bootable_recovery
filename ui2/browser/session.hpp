// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol.hpp"

namespace recovery_ui2::web {
enum class KeyboardRequest { kNone, kShow, kHide };
// Native side of the bridge. Adopt() is only for a future trusted launcher:
// the current recovery never calls it and has no path/FD/environment override.
// Browser IPC can never initiate recovery operations or select local files.
class Session {
 public:
  ~Session() { Close(); Unmap(); }
  Session() = default;
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  bool Adopt(int frame_fd, int control_fd);
  void Close();
  bool Connected() const { return control_ >= 0; }
  bool Send(Kind kind, int x = 0, int y = 0, uint32_t value = 0, const char *text = "");
  bool Poll(); // Nonblocking, at most eight messages; true if pixels changed.
  // Called after LVGL finishes rendering and flushing the current slot.
  bool AcknowledgeFrame();
  const uint8_t *Pixels() const {
    return shared_ ? shared_ + FrameSlot(sequence_) * kFrameBytes : nullptr;
  }
  const std::string &Status() const { return status_; }
  bool CanBack() const { return can_back_; }
  bool CanForward() const { return can_forward_; }
  unsigned Progress() const { return progress_; }
  KeyboardRequest TakeKeyboardRequest(uint32_t *purpose = nullptr);
 private:
  bool Write(const Message &message);
  void Fail(const char *reason);
  void Unmap();
  int control_ = -1;
  const uint8_t *shared_ = nullptr;
  std::string status_;
  uint32_t sequence_ = 0;
  uint32_t keyboard_purpose_ = 0;
  unsigned progress_ = 0;
  KeyboardRequest keyboard_request_ = KeyboardRequest::kNone;
  bool can_back_ = false, can_forward_ = false;
  bool frame_pending_ = false;
};
}  // namespace recovery_ui2::web
