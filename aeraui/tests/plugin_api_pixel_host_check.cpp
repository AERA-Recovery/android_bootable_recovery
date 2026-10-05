/* SPDX-License-Identifier: Apache-2.0 */
// Runs a real Host API 3 plugin through AERA's own launcher, session and
// pixel surface, playing the pixel scene's part without LVGL: one refresh
// every 16 ms, touches in surface pixels, Back.
//
//   plugin_api_pixel_host_check RUNTIME_TREE OUT_DIR
//
// RUNTIME_TREE is an expanded ui-runtime payload (usr/bin/aera-plugin ...).
// Written for flutter-aera's counter app: it taps the floating button three
// times, checks the picture changed, then presses Back on the root route and
// expects the plugin to CLOSE. Frames are written as PPM files.
#include "plugin_api/launcher.hpp"
#include "plugin_api/session.hpp"
#include "plugin_api/surface.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace aeraui {
namespace i18n {
const char *CurrentLanguage() { return "en"; }
}  // namespace i18n
bool RecoveryLightMode() { return true; }
}  // namespace aeraui

using namespace aeraui::plugin_api;
using Clock = std::chrono::steady_clock;

namespace {
std::vector<uint8_t> gShown;

void WritePpm(const std::string &path, const SurfaceGeometry &g,
              const std::vector<uint8_t> &bgra) {
  FILE *file = fopen(path.c_str(), "wb");
  assert(file);
  fprintf(file, "P6\n%u %u\n255\n", g.width, g.height);
  std::vector<uint8_t> row(g.width * 3);
  for (uint32_t y = 0; y < g.height; ++y) {
    const uint8_t *in = bgra.data() + uint64_t{y} * g.stride;
    for (uint32_t x = 0; x < g.width; ++x) {
      row[x * 3] = in[x * 4 + 2];
      row[x * 3 + 1] = in[x * 4 + 1];
      row[x * 3 + 2] = in[x * 4];
    }
    fwrite(row.data(), 1, row.size(), file);
  }
  fclose(file);
}

struct Host {
  Surface surface;
  Session session;
  const uint8_t *latched = nullptr;
  unsigned presents = 0, shown = 0;
  bool closed = false;

  // One iteration of the scene's timer and the display's refresh.
  void Tick() {
    for (const auto &message : session.Poll()) {
      if (message.kind == Kind::kPresent) {
        std::vector<uint32_t> released;
        assert(surface.Present(message.request_id, message.value, message.flags, released));
        for (uint32_t sequence : released)
          session.Send(Kind::kFrameDone, sequence);
        ++presents;
      } else if (message.kind == Kind::kClose) {
        closed = true;
      } else if (message.kind == Kind::kKeyboardShow ||
                 message.kind == Kind::kKeyboardHide) {
        printf("host: keyboard %s\n",
               message.kind == Kind::kKeyboardShow ? "show" : "hide");
      } else {
        printf("host: kind %u\n", static_cast<unsigned>(message.kind));
      }
    }
    // REFR_READY for the frame latched last tick, then latch the next.
    if (latched) {
      std::vector<uint32_t> released;
      surface.RefreshDone(released);
      for (uint32_t sequence : released)
        session.Send(Kind::kFrameDone, sequence);
      gShown.assign(latched, latched + surface.Geometry().FrameBytes());
      ++shown;
      latched = nullptr;
    }
    latched = surface.LatchNext();
  }

  void RunFor(int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end && session.Connected() && !closed) {
      Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  void Tap(uint32_t x, uint32_t y) {
    session.Send(Kind::kTouchDown, 0, x, y);
    RunFor(50);
    session.Send(Kind::kTouchUp, 0, x, y);
    RunFor(250);
  }
};
}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s RUNTIME_TREE OUT_DIR\n", argv[0]);
    return 2;
  }
  const std::string out = argv[2];
  // The launcher only accepts AERA's private RAM runtime layout.
  char runtime[] = "/tmp/aera-p2-XXXXXX";
  assert(mkdtemp(runtime));
  const std::string copy = std::string("cp -R --preserve=mode ") + argv[1] + "/. " + runtime;
  assert(system(copy.c_str()) == 0);
  assert(chmod(runtime, 0700) == 0);
  const std::string data = out + "/plugin-data";
  assert(system(("mkdir -p " + data).c_str()) == 0);

  Host host;
  SurfaceGeometry g;
  g.width = 1080;
  g.height = 2400;
  g.stride = g.width * 4;
  g.slots = 3;
  g.scale = 2.75;
  assert(host.surface.Create(g));

  Process process;
  LaunchOptions options;
  options.host_api = kProtocolVersion3;
  options.surface_fd = host.surface.PluginFd();
  options.data_dir = data;
  int control = -1;
  std::string error;
  if (!process.Start(runtime, control, error, options)) {
    fprintf(stderr, "launch: %s\n", error.c_str());
    return 1;
  }
  assert(host.session.Adopt(control, kProtocolVersion3,
                            {host.surface.SurfaceMessage()}));

  const auto started = Clock::now();
  while (host.shown == 0 && host.session.Connected() &&
         Clock::now() - started < std::chrono::seconds(60))
    host.RunFor(100);
  assert(host.session.Negotiated() && host.session.Version() == 3);
  assert(host.shown > 0);
  printf("host: first frame after %lld ms\n",
         static_cast<long long>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 Clock::now() - started).count()));
  host.RunFor(1000);
  const auto before = gShown;
  WritePpm(out + "/start.ppm", g, before);
  for (int i = 0; i < 3; ++i) host.Tap(980, 2250);
  host.RunFor(1000);
  WritePpm(out + "/after.ppm", g, gShown);
  assert(gShown != before);
  printf("host: %u presents, %u refreshes\n", host.presents, host.shown);

  // Back on the root route: Flutter pops the app, which closes the plugin.
  host.session.Send(Kind::kBack);
  host.RunFor(3000);
  assert(host.closed);
  host.session.Send(Kind::kLifecycle, 0,
                    static_cast<uint32_t>(Lifecycle::kStop));
  host.session.Close();
  process.Stop();
  assert(system((std::string("rm -rf ") + runtime).c_str()) == 0);
  printf("PIXEL_HOST_OK\n");
  return 0;
}
