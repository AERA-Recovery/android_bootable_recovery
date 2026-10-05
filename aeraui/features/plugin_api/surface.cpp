/* SPDX-License-Identifier: Apache-2.0 */
#include "surface.hpp"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace aeraui::plugin_api {

bool SurfaceGeometry::Valid() const {
  return width >= 1 && width <= 8192 && height >= 1 && height <= 8192 &&
         stride >= width * 4 && stride % 4 == 0 && slots >= 2 &&
         slots <= kMaxSurfaceSlots && scale >= 0.5 && scale <= 8.0 &&
         refresh_hz >= 1.0 && refresh_hz <= 240.0;
}

bool Surface::Create(const SurfaceGeometry &geometry, uint64_t capacity) {
  Close();
  if (!geometry.Valid()) return false;
  capacity = std::max(capacity, geometry.TotalBytes());
  const int fd = memfd_create("aera-plugin-surface",
                              MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return false;
  if (ftruncate(fd, static_cast<off_t>(capacity)) != 0 ||
      fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
    close(fd);
    return false;
  }
  void *pixels = mmap(nullptr, capacity, PROT_READ, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) {
    close(fd);
    return false;
  }
  geometry_ = geometry;
  capacity_ = capacity;
  generation_ = 0;
  fd_ = fd;
  pixels_ = static_cast<uint8_t *>(pixels);
  slots_.assign(geometry.slots, Slot{});
  pending_ = latched_ = shown_ = -1;
  last_sequence_ = 0;
  return true;
}

bool Surface::Reshape(const SurfaceGeometry &geometry) {
  if (!pixels_ || !geometry.Valid() || geometry.TotalBytes() > capacity_ ||
      geometry.slots != geometry_.slots)
    return false;
  geometry_ = geometry;
  slots_.assign(geometry.slots, Slot{});
  pending_ = latched_ = shown_ = -1;
  ++generation_;
  return true;
}

void Surface::Close() {
  if (pixels_) munmap(pixels_, capacity_);
  if (fd_ >= 0) close(fd_);
  pixels_ = nullptr;
  fd_ = -1;
  slots_.clear();
  pending_ = latched_ = shown_ = -1;
}

Message Surface::SurfaceMessage() const {
  Message message;
  message.kind = Kind::kSurface;
  message.request_id = geometry_.stride;
  message.value = geometry_.width;
  message.flags = geometry_.height;
  snprintf(message.title, sizeof(message.title), "%s", kSurfaceFormat);
  snprintf(message.text, sizeof(message.text), "slots=%u scale=%g refresh=%g",
           geometry_.slots, geometry_.scale, geometry_.refresh_hz);
  return message;
}

bool Surface::Present(uint32_t sequence, uint32_t slot, uint32_t generation,
                      std::vector<uint32_t> &released) {
  if (!pixels_ || slot >= slots_.size() || sequence == 0 ||
      sequence == last_sequence_ || slots_[slot].owner != Owner::kPlugin)
    return false;
  if (generation != generation_) {
    // Drawn for an earlier SURFACE (the plugin had not seen the new one
    // yet): its pixels do not fit, so it goes straight back.
    last_sequence_ = sequence;
    released.push_back(sequence);
    return true;
  }
  if (pending_ >= 0) {
    // Superseded before it was ever latched: give it straight back.
    released.push_back(slots_[pending_].sequence);
    slots_[pending_] = Slot{};
  }
  slots_[slot] = {Owner::kPending, sequence};
  pending_ = static_cast<int>(slot);
  last_sequence_ = sequence;
  return true;
}

const uint8_t *Surface::LatchNext() {
  if (pending_ < 0 || latched_ >= 0) return nullptr;
  latched_ = pending_;
  pending_ = -1;
  slots_[latched_].owner = Owner::kLatched;
  return Pixels(latched_);
}

void Surface::RefreshDone(std::vector<uint32_t> &released) {
  if (latched_ < 0) return;
  if (shown_ >= 0) {
    released.push_back(slots_[shown_].sequence);
    slots_[shown_] = Slot{};
  }
  shown_ = latched_;
  latched_ = -1;
  slots_[shown_].owner = Owner::kShown;
}

}  // namespace aeraui::plugin_api
