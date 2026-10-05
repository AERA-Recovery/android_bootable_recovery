/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <cstring>

namespace aeraui::plugin_api {

constexpr uint32_t kMagic = 0x41325049U;  // A2PI
constexpr uint32_t kProtocolVersion = 2;
// Host API 3 adds a GPU pixel surface for plugins that draw their own UI.
// Messages carry the negotiated version; API 2 plugins never see API 3 kinds.
constexpr uint32_t kProtocolVersion3 = 3;
constexpr uint32_t kMaxProtocolVersion = kProtocolVersion3;
constexpr uint32_t kMaxButtons = 12;
constexpr uint32_t kMaxMetrics = 64;
constexpr uint32_t kMaxSections = 16;

enum Features : uint32_t {
  kFeatureMetrics = 1U << 0,
  kFeatureBackNavigation = 1U << 1,
  // Host API 3: a pixel surface on fd 3 (AERA_SURFACE_FD), SURFACE/PRESENT/
  // FRAME_DONE, touch, keys, Back and keyboard requests.
  kFeaturePixelSurface = 1U << 2,
  // Host API 3: KEYBOARD_INSET whenever AERA's keyboard shows, hides or
  // resizes over the surface.
  kFeatureKeyboardInset = 1U << 3,
  // Host API 3: REQUEST_OPERATION kPickFiles opens AERA's file picker.
  kFeatureFilePicker = 1U << 4,
};

enum class Kind : uint32_t {
  kHello = 1,
  kBeginPage,
  kAddButton,
  kCommitPage,
  kSetStatus,
  kRequestOperation,
  kClose,
  kAddSection,
  kAddMetric,
  kUpdateMetric,
  kSetBackAction,
  // Host API 3, plugin to host.
  kPresent,       // request_id sequence (non-zero), value slot
  kKeyboardShow,  // value Purpose, flags 1 for multiline
  kKeyboardHide,
  kHelloAck = 64,
  kAction,
  kLifecycle,
  kOperationResult,
  // Host API 3, host to plugin.
  kSurface,        // value width, flags height, request_id stride, title
                   // "BGRA8888", text "slots=N scale=S refresh=HZ"
  kFrameDone,      // request_id sequence; its slot belongs to the plugin again
  kTouchDown,      // request_id pointer, value x, flags y, surface pixels
  kTouchMove,
  kTouchUp,
  kKey,            // value Unicode code point; 8 backspace, 13 Enter or OK
  kBack,
  kKeyboardInset,  // value keyboard height over the surface in pixels, 0 hidden
};

// Host API 3 keyboard purposes, as AERA Browser's keyboard has them.
enum class Purpose : uint32_t { kText = 0, kDigits = 2 };
constexpr char kSurfaceFormat[] = "BGRA8888";
constexpr int kSurfaceFd = 3;
constexpr uint32_t kMaxSurfaceSlots = 4;

// Host API 3 adds kInactive (visible, but a shade, sheet or picker takes the
// input) and pauses a plugin whose scene is left instead of stopping it:
// after kPause the host sends only a new SURFACE then kResume, kStop or
// kClose. A plugin that ignores kInactive still behaves correctly.
enum class Lifecycle : uint32_t { kResume = 1, kPause, kStop, kInactive };
// Operation numbers are part of the public Host API 2 wire contract. Never
// renumber an existing entry: independently released plugins send these raw
// values over the socket.
enum class Operation : uint32_t {
  kBackupSettings = 1,
  kRestoreSettings = 2,
  kBackupAndroidSettings = 3,
  kRestoreAndroidSettings = 4,
  kStartMirror = 5,
  kStopMirror = 6,
  kStartWifiMirror = 7,
  // Host API 3 (kFeatureFilePicker): the user picks files, a folder or a
  // place to save in AERA's picker. `flags` is a PickMode, `title` the
  // folder to start in (empty: the current storage), `text` the file
  // extensions to show ("zip,img"; empty: all), or in kSave the suggested
  // name. Each chosen path comes back as its own OPERATION_RESULT (`value`
  // 1, `text` the absolute path, `flags` 1 while more follow); a closed
  // picker answers `value` 0 with empty `text`, a refusal `value` 0 with
  // the reason.
  kPickFiles = 8,
};
enum class PickMode : uint32_t { kFile = 0, kFiles, kFolder, kSave };
enum Flags : uint32_t {
  kPrimary = 1U << 0,
  kDestructive = 1U << 1,
  kDisabled = 1U << 2,
  kMetricGood = 1U << 8,
  kMetricWarning = 1U << 9,
  kMetricCritical = 1U << 10,
  kMetricAccent = 1U << 11,
};

// One bounded packet is one complete message. Strings must be NUL-terminated;
// no pointers, file descriptors, paths, or variable-size allocations cross the
// trust boundary.
struct Message {
  uint32_t magic = kMagic;
  uint32_t version = kProtocolVersion;
  Kind kind = Kind::kSetStatus;
  uint32_t request_id = 0;
  uint32_t value = 0;
  uint32_t flags = 0;
  char title[96]{};
  char text[1024]{};
};

static_assert(sizeof(Message) == 1144);

inline bool StringsValid(const Message &message) {
  return memchr(message.title, '\0', sizeof(message.title)) != nullptr &&
         memchr(message.text, '\0', sizeof(message.text)) != nullptr;
}
inline bool WorkerKind(Kind kind, uint32_t version = kProtocolVersion) {
  return kind >= Kind::kHello &&
         kind <= (version >= kProtocolVersion3 ? Kind::kKeyboardHide
                                               : Kind::kSetBackAction);
}
inline bool HostKind(Kind kind, uint32_t version = kProtocolVersion) {
  return kind >= Kind::kHelloAck &&
         kind <= (version >= kProtocolVersion3 ? Kind::kKeyboardInset
                                               : Kind::kOperationResult);
}
// `version` is the negotiated protocol version. Before negotiation the host
// accepts a HELLO carrying any version it supports.
inline bool Valid(const Message &message, bool from_worker,
                  uint32_t version = kProtocolVersion) {
  return message.magic == kMagic && message.version == version &&
         StringsValid(message) &&
         (from_worker ? WorkerKind(message.kind, version)
                      : HostKind(message.kind, version));
}

}  // namespace aeraui::plugin_api
