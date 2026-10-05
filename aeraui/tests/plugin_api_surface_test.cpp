/* SPDX-License-Identifier: Apache-2.0 */
#include "plugin_api/surface.hpp"

#include <cassert>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace aeraui::plugin_api;

int main() {
  SurfaceGeometry g;
  g.width = 4; g.height = 2; g.stride = 16; g.slots = 3; g.scale = 3.5;
  Surface surface;
  assert(surface.Create(g));
  // The plugin writes through its own mapping of fd 3.
  auto *plugin = static_cast<uint8_t *>(mmap(nullptr, g.TotalBytes(),
      PROT_READ | PROT_WRITE, MAP_SHARED, surface.PluginFd(), 0));
  assert(plugin != MAP_FAILED);
  // Sealed: the plugin cannot shrink the frames under AERA.
  assert(ftruncate(surface.PluginFd(), 1) != 0);

  const Message m = surface.SurfaceMessage();
  assert(m.kind == Kind::kSurface && m.value == 4 && m.flags == 2 &&
         m.request_id == 16 && !strcmp(m.title, "BGRA8888") &&
         !strcmp(m.text, "slots=3 scale=3.5 refresh=60"));

  std::vector<uint32_t> released;
  assert(!surface.Present(0, 0, 0, released));   // sequence 0
  assert(!surface.Present(1, 3, 0, released));   // no such slot
  memset(plugin, 0x11, g.FrameBytes());
  assert(surface.Present(1, 0, 0, released) && released.empty());
  assert(!surface.Present(2, 0, 0, released));   // slot 0 is AERA's now
  assert(!surface.Present(1, 1, 0, released));   // sequence reused
  const uint8_t *frame = surface.LatchNext();
  assert(frame && frame[0] == 0x11 && !surface.LatchNext());
  surface.RefreshDone(released);
  assert(released.empty() && surface.HasFrame());  // first frame replaces none

  // Two frames before a refresh: the older one is skipped and returned.
  assert(surface.Present(2, 1, 0, released) && released.empty());
  assert(surface.Present(3, 2, 0, released));
  assert(released == std::vector<uint32_t>{2});
  released.clear();
  assert(surface.Present(4, 1, 0, released));     // slot 1 was given back
  assert(released == std::vector<uint32_t>{3});
  released.clear();
  assert(surface.LatchNext() != nullptr);
  surface.RefreshDone(released);
  assert(released == std::vector<uint32_t>{1});  // frame 1 left the screen
  released.clear();

  // Rotation: a new geometry within the capacity starts generation 1. Every
  // slot is the plugin's again and nothing is shown until a new frame.
  SurfaceGeometry rotated = g;
  rotated.width = 2; rotated.height = 4; rotated.stride = 8;
  assert(surface.Reshape(rotated) && surface.Generation() == 1);
  assert(!surface.HasFrame() && !surface.LatchNext());
  const Message r = surface.SurfaceMessage();
  assert(r.value == 2 && r.flags == 4 && r.request_id == 8);
  // A frame drawn for the old SURFACE goes straight back, unshown.
  assert(surface.Present(5, 0, 0, released));
  assert(released == std::vector<uint32_t>{5} && !surface.LatchNext());
  released.clear();
  assert(surface.Present(6, 0, 1, released) && released.empty());
  assert(surface.LatchNext() != nullptr);
  // Larger than the memfd, or another slot count: refused.
  SurfaceGeometry big = g;
  big.width = 64; big.stride = 256;
  assert(!surface.Reshape(big));
  SurfaceGeometry fewer = rotated;
  fewer.slots = 2;
  assert(!surface.Reshape(fewer));

  // A capacity sized for both orientations.
  Surface roomy;
  SurfaceGeometry tall = g;
  tall.width = 2; tall.height = 6; tall.stride = 8;
  assert(roomy.Create(g, 6 * 6 * 4 * 3));
  assert(roomy.Reshape(tall));
  return 0;
}
