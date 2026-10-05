/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <vector>

namespace aeraui::plugin_api {

struct SurfaceGeometry {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;  // bytes per row
  uint32_t slots = 3;
  double scale = 1.0;   // physical pixels per logical pixel for the plugin
  double refresh_hz = 60.0;
  uint64_t FrameBytes() const { return uint64_t{stride} * height; }
  uint64_t TotalBytes() const { return FrameBytes() * slots; }
  bool Valid() const;
};

// Host API 3's pixel surface: a sealed memfd of `slots` BGRA8888 top-down
// frames that the plugin writes and AERA reads. Each slot belongs to exactly
// one side. The plugin hands one over with PRESENT; AERA hands it back with
// FRAME_DONE once a newer frame has replaced it on screen, or at once if a
// newer frame arrived before it was ever shown. The slot on screen stays
// AERA's, because LVGL may redraw from it at any time.
//
// No LVGL here, so the state machine is unit-tested on its own.
class Surface final {
 public:
  ~Surface() { Close(); }
  Surface() = default;
  Surface(const Surface &) = delete;
  Surface &operator=(const Surface &) = delete;

  // Creates and maps the frames. PluginFd() is what the plugin gets on fd 3.
  // `capacity` (bytes, at least the geometry's) sizes the memfd for every
  // geometry Reshape() may later switch to.
  bool Create(const SurfaceGeometry &geometry, uint64_t capacity = 0);
  // Switches to a new geometry within the capacity, e.g. after a rotation,
  // and starts a new surface generation: every slot is the plugin's again
  // and no frame is shown until one of the new generation arrives. Send
  // SurfaceMessage() after it.
  bool Reshape(const SurfaceGeometry &geometry);
  uint32_t Generation() const { return generation_; }
  void Close();
  int PluginFd() const { return fd_; }
  const SurfaceGeometry &Geometry() const { return geometry_; }
  Message SurfaceMessage() const;

  // A PRESENT from the plugin. False for a protocol violation (unknown slot,
  // a slot AERA holds, sequence 0 or reused). `released` receives sequences
  // to answer with FRAME_DONE right away (a skipped, never shown frame, or
  // one drawn for an earlier `generation` than the current SURFACE's).
  bool Present(uint32_t sequence, uint32_t slot, uint32_t generation,
               std::vector<uint32_t> &released);
  // The newest presented frame not yet on screen, latched for the next
  // refresh; nullptr if there is none.
  const uint8_t *LatchNext();
  // The refresh that showed the latched frame has finished. `released`
  // receives the sequence of the frame it replaced.
  void RefreshDone(std::vector<uint32_t> &released);
  bool HasFrame() const { return shown_ >= 0; }

 private:
  enum class Owner : uint8_t { kPlugin, kPending, kLatched, kShown };
  struct Slot {
    Owner owner = Owner::kPlugin;
    uint32_t sequence = 0;
  };
  const uint8_t *Pixels(int slot) const {
    return pixels_ + static_cast<uint64_t>(slot) * geometry_.FrameBytes();
  }
  SurfaceGeometry geometry_{};
  uint64_t capacity_ = 0;
  uint32_t generation_ = 0;
  int fd_ = -1;
  uint8_t *pixels_ = nullptr;
  std::vector<Slot> slots_;
  int pending_ = -1, latched_ = -1, shown_ = -1;
  uint32_t last_sequence_ = 0;
};

}  // namespace aeraui::plugin_api
