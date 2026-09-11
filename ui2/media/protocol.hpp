/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <cstring>

namespace recovery_ui2::media {
constexpr uint32_t kMagic = 0x414d5031U;
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr uint32_t kFrameBytes = kWidth * kHeight * 4U;
constexpr uint32_t kFrameSlots = 2;
constexpr uint32_t kSharedBytes = kFrameBytes * kFrameSlots;
constexpr uint32_t FrameSlot(uint32_t sequence) { return sequence % kFrameSlots; }

enum class Kind : uint32_t {
  kOpen = 1, kPlay, kPause, kSeekRelative, kClose, kAck,
  kFrame = 32, kStatus, kError, kEnded
};

struct Message {
  uint32_t magic = kMagic;
  Kind kind = Kind::kStatus;
  uint32_t sequence = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t value = 0;
  int64_t position_ms = 0;
  int64_t duration_ms = 0;
  char text[512]{};
};
static_assert(sizeof(Message) == 552);

inline bool Valid(const Message &message, bool from_worker) {
  if (message.magic != kMagic ||
      !memchr(message.text, '\0', sizeof(message.text))) return false;
  if (from_worker)
    return (message.kind == Kind::kFrame && message.x == kWidth &&
            message.y == kHeight && message.value == kFrameBytes) ||
           message.kind == Kind::kStatus || message.kind == Kind::kError ||
           message.kind == Kind::kEnded;
  return message.kind == Kind::kOpen || message.kind == Kind::kPlay ||
         message.kind == Kind::kPause || message.kind == Kind::kSeekRelative ||
         message.kind == Kind::kClose || message.kind == Kind::kAck;
}
}  // namespace recovery_ui2::media
