/* SPDX-License-Identifier: Apache-2.0 */
// Host API 3: a plugin that draws its own pixels into a shared surface, with
// AERA forwarding touch, its keyboard and Back. Everything else (runtime
// verification, process isolation, the bounded control channel) is Host API
// 2's, unchanged.
//
// Like Browser, a running plugin outlives its scene: leaving it (Home,
// Recents, another app) pauses the plugin and opening it again resumes it
// where it was. It stops when it closes itself, when Recents is cleared or
// drops it, or when it fails.
#include "scene.hpp"

#include "browser/runtime.hpp"
#include "file_picker.hpp"
#include "phone_keyboard.hpp"
#include "plugin_api/launcher.hpp"
#include "plugin_api/protocol.hpp"
#include "plugin_api/session.hpp"
#include "plugin_api/surface.hpp"
#include "plugins/plugin_manager.hpp"
#include "ui_components.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <aeraui/status_bar.hpp>
#include <linux/types.h>
#include <minuitwrp/minui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <sys/system_properties.h>
#include <thread>
#include <time.h>
#include <vector>

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

constexpr int kMaxPointers = 10;
// OPERATION_RESULTs sent per tick while a pick's paths go out, so a large
// selection never fills the control socket.
constexpr int kPickResultsPerTick = 8;
// A selection is at most this many files.
constexpr size_t kMaxPickedFiles = 256;

uint64_t NowMs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

struct PixelScene {
  lv_obj_t *screen = nullptr;
  lv_display_t *display = nullptr;
  lv_obj_t *loading = nullptr, *title = nullptr, *detail = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *viewport = nullptr, *image = nullptr, *keyboard = nullptr;
  lv_image_dsc_t descriptor{};
  ActionCallback callback = nullptr;
  void *context = nullptr;
  plugins::Plugin plugin;
  web::Preparation preparation;
  plugin_api::Process process;
  plugin_api::Session session;
  plugin_api::Surface surface;
  std::thread worker;
  lv_timer_t *timer = nullptr;
  // The viewport in AERA's logical coordinates, and surface pixels per
  // logical unit across and down (the panel is usually smaller than AERA's
  // 1440 columns, and its shape need not be the canvas's exactly).
  int view_left = 0, view_top = 0, view_width = 0, view_height = 0;
  double pixels_per_unit_x = 1.0, pixels_per_unit_y = 1.0;
  bool touch_pressed[kMaxPointers]{};
  bool touch_ignored[kMaxPointers]{};
  // A first contact that landed in a side-edge zone, not yet known to be
  // AERA's Back swipe or a touch for the plugin.
  bool edge_pending = false;
  int edge_x = 0;
  int edge_y = 0;
  bool keyboard_multiline = false;
  // The open kPickFiles picker and its request, and chosen paths not yet
  // sent.
  lv_obj_t *picker = nullptr;
  uint32_t pick_request = 0;
  std::deque<std::string> picked;
  uint32_t keyboard_inset = 0;
  bool frame_waiting_for_refresh = false;
  bool showing = false;
  bool launched = false;
  bool stopped = false;
  bool leaving = false;
  // LIFECYCLE sent last: paused while no scene shows the plugin, inactive
  // while a shade, sheet or the picker covers it.
  bool paused = false;
  bool inactive = false;
  // Stop instead of pausing when the scene is left (Recents cleared).
  bool discard = false;
  uint64_t launched_ms = 0;

  ~PixelScene();
};

// The scene on screen, if it is a pixel plugin's.
PixelScene *gPixelScene = nullptr;
// Every running plugin by id, on screen or paused.
std::map<std::string, PixelScene *> gRunning;

void PixelRefreshReady(lv_event_t *event);

PixelScene::~PixelScene() {
  if (gPixelScene == this) gPixelScene = nullptr;
  const auto running = gRunning.find(plugin.id);
  if (running != gRunning.end() && running->second == this)
    gRunning.erase(running);
  if (timer) lv_timer_delete(timer);
  if (display)
    lv_display_remove_event_cb_with_user_data(display, PixelRefreshReady,
                                              this);
  // The picker overlay outlives the scene until its screen is deleted.
  if (picker) file_picker::Forget(picker);
  preparation.cancel.store(true);
  if (worker.joinable()) worker.join();
  if (session.Connected()) {
    session.Send(plugin_api::Kind::kLifecycle, 0,
                 static_cast<uint32_t>(plugin_api::Lifecycle::kStop));
    session.Send(plugin_api::Kind::kClose);
  }
  session.Close();
  process.Stop();
  // The image may still point into the mapping until LVGL forgets it.
  if (image) lv_image_set_src(image, nullptr);
  lv_image_cache_drop(&descriptor);
  surface.Close();
  web::RemoveRuntime(preparation.directory);
}

void SetStatus(PixelScene *scene, const char *title, const std::string &detail) {
  if (scene->loading) lv_obj_remove_flag(scene->loading, LV_OBJ_FLAG_HIDDEN);
  if (scene->title) i18n::BindLabel(scene->title, title);
  if (scene->detail) i18n::BindLabel(scene->detail, detail.c_str());
}

// Android's own density, so a plugin lays out as apps do on this phone.
double DeviceScale(int surface_width) {
  char value[PROP_VALUE_MAX]{};
  if (__system_property_get("ro.sf.lcd_density", value) > 0) {
    const long density = strtol(value, nullptr, 10);
    if (density >= 80 && density <= 1280) return density / 160.0;
  }
  // A typical phone is about 411 logical pixels wide.
  return std::clamp(std::round(surface_width / 411.0 * 4) / 4, 1.0, 4.0);
}

// A Sheet or other transient modal over the surface owns input.
bool SheetVisible(lv_obj_t *screen) {
  for (uint32_t index = 0; index < lv_obj_get_child_count(screen); ++index) {
    auto *child = lv_obj_get_child(screen, static_cast<int32_t>(index));
    if (lv_obj_get_user_data(child) == &kModalMarker &&
        !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) return true;
  }
  return false;
}

void SendFrameDone(PixelScene *scene, const std::vector<uint32_t> &released) {
  for (const uint32_t sequence : released)
    scene->session.Send(plugin_api::Kind::kFrameDone, sequence);
}

void PixelRefreshReady(lv_event_t *event) {
  auto *scene = static_cast<PixelScene *>(lv_event_get_user_data(event));
  if (!scene->frame_waiting_for_refresh) return;
  scene->frame_waiting_for_refresh = false;
  std::vector<uint32_t> released;
  scene->surface.RefreshDone(released);
  if (scene->session.Connected()) SendFrameDone(scene, released);
}

uint32_t KeyboardInset(PixelScene *scene) {
  if (!scene->keyboard || lv_obj_has_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN))
    return 0;
  lv_obj_update_layout(scene->keyboard);
  lv_area_t area{};
  lv_obj_get_coords(scene->keyboard, &area);
  const int covered = scene->view_top + scene->view_height - area.y1;
  if (covered <= 0) return 0;
  return static_cast<uint32_t>(std::lround(
      std::min(covered, scene->view_height) * scene->pixels_per_unit_y));
}

void SendKeyboardInset(PixelScene *scene) {
  const uint32_t inset = KeyboardInset(scene);
  if (inset == scene->keyboard_inset) return;
  scene->keyboard_inset = inset;
  if (scene->session.Negotiated())
    scene->session.Send(plugin_api::Kind::kKeyboardInset, 0, inset);
}

void ReleaseTouches(PixelScene *scene) {
  for (int slot = 0; slot < kMaxPointers; ++slot) {
    if (scene->touch_pressed[slot] && scene->session.Negotiated())
      scene->session.Send(plugin_api::Kind::kTouchUp,
                          static_cast<uint32_t>(slot), 0, 0);
    scene->touch_pressed[slot] = false;
    scene->touch_ignored[slot] = false;
  }
  scene->edge_pending = false;
}

void HideKeyboard(PixelScene *scene) {
  if (!scene->keyboard) return;
  lv_obj_add_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
  SendKeyboardInset(scene);
}

void ShowKeyboard(PixelScene *scene, uint32_t purpose, bool multiline) {
  if (!scene->keyboard) return;
  ReleaseTouches(scene);
  scene->keyboard_multiline = multiline;
  lv_keyboard_set_mode(scene->keyboard,
      purpose == static_cast<uint32_t>(plugin_api::Purpose::kDigits)
          ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_remove_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(scene->keyboard);
  SendKeyboardInset(scene);
}

// One key as a Unicode code point; 0 for keyboard UI (mode switches).
uint32_t KeyCode(const char *key) {
  if (!strcmp(key, LV_SYMBOL_BACKSPACE)) return 8;
  if (!strcmp(key, LV_SYMBOL_NEW_LINE)) return 13;
  const auto *bytes = reinterpret_cast<const unsigned char *>(key);
  const size_t length = strlen(key);
  if (length == 1) return bytes[0] >= 32 && bytes[0] < 127 ? bytes[0] : 0;
  // A single UTF-8 character that is not an LVGL symbol (those live in the
  // private use area).
  uint32_t code = 0;
  size_t expected = 0;
  if ((bytes[0] & 0xe0) == 0xc0) { code = bytes[0] & 0x1f; expected = 2; }
  else if ((bytes[0] & 0xf0) == 0xe0) { code = bytes[0] & 0x0f; expected = 3; }
  else if ((bytes[0] & 0xf8) == 0xf0) { code = bytes[0] & 0x07; expected = 4; }
  if (expected == 0 || length != expected) return 0;
  for (size_t i = 1; i < length; ++i) {
    if ((bytes[i] & 0xc0) != 0x80) return 0;
    code = (code << 6) | (bytes[i] & 0x3f);
  }
  if (code >= 0xe000 && code <= 0xf8ff) return 0;
  return code;
}

// Leaves the scene once the plugin is gone, outside of any LVGL callback that
// still holds the scene.
void Leave(PixelScene *scene) {
  if (scene->leaving) return;
  scene->leaving = true;
  if (scene->viewport) lv_obj_set_user_data(scene->viewport, nullptr);
  auto callback = scene->callback;
  auto *context = scene->context;
  lv_async_call([](void *data) {
    auto *pair = static_cast<std::pair<ActionCallback, void *> *>(data);
    pair->first(Action::kBack, pair->second);
    delete pair;
  }, new std::pair<ActionCallback, void *>(callback, context));
}

void ClosePicker(PixelScene *scene) {
  if (!scene->picker) return;
  file_picker::Forget(scene->picker);
  file_picker::Dismiss(scene->picker);
  scene->picker = nullptr;
  scene->picked.clear();
}

void Stop(PixelScene *scene, const char *title, const std::string &detail) {
  if (scene->stopped) return;
  scene->stopped = true;
  const auto running = gRunning.find(scene->plugin.id);
  if (running != gRunning.end() && running->second == scene)
    gRunning.erase(running);
  // A paused plugin has no scene to report to.
  if (!scene->screen) {
    lv_async_call([](void *data) { delete static_cast<PixelScene *>(data); },
                  scene);
    return;
  }
  ClosePicker(scene);
  scene->session.Close();
  scene->process.Stop();
  if (scene->viewport) {
    lv_obj_set_user_data(scene->viewport, nullptr);
    lv_obj_add_flag(scene->viewport, LV_OBJ_FLAG_HIDDEN);
  }
  scene->showing = false;
  HideKeyboard(scene);
  SetStatus(scene, title, detail);
}

// kPickFiles: the pick itself is the user's consent, so no permission or
// prompt is involved, as with a file dialog on a desktop.
void StartPick(PixelScene *scene, const plugin_api::Message &message) {
  auto refuse = [&](const char *reason) {
    scene->session.Send(plugin_api::Kind::kOperationResult,
                        message.request_id, 0, 0, nullptr, reason);
  };
  if (message.request_id == 0) return refuse("Unsupported host operation.");
  if (!scene->screen) return refuse("The plugin is not on screen.");
  if (scene->picker || !scene->picked.empty())
    return refuse("A file picker is already open.");
  if (message.flags > static_cast<uint32_t>(plugin_api::PickMode::kSave))
    return refuse("Unknown file picker mode.");
  const auto mode = static_cast<plugin_api::PickMode>(message.flags);
  file_picker::Request request;
  request.mode = mode == plugin_api::PickMode::kFiles ? file_picker::Mode::kFiles
      : mode == plugin_api::PickMode::kFolder ? file_picker::Mode::kFolder
      : mode == plugin_api::PickMode::kSave ? file_picker::Mode::kSave
                                            : file_picker::Mode::kFile;
  request.start = message.title;
  request.max_path = sizeof(message.text);
  request.max_count = kMaxPickedFiles;
  if (mode == plugin_api::PickMode::kSave) {
    request.suggested_name = message.text;
  } else {
    std::string extension;
    for (const char *c = message.text;; ++c) {
      if (*c == ',' || *c == ' ' || *c == '\0') {
        if (!extension.empty()) request.extensions.push_back(extension);
        extension.clear();
        if (*c == '\0') break;
      } else if (*c != '.' || !extension.empty()) {
        extension += static_cast<char>(tolower(static_cast<unsigned char>(*c)));
      }
    }
  }
  ReleaseTouches(scene);
  HideKeyboard(scene);
  scene->pick_request = message.request_id;
  scene->picker = file_picker::Show(scene->screen, std::move(request),
      [scene](std::vector<std::string> paths) {
        scene->picker = nullptr;
        scene->picked.assign(paths.begin(), paths.end());
      },
      [scene] {
        scene->picker = nullptr;
        if (scene->session.Negotiated())
          scene->session.Send(plugin_api::Kind::kOperationResult,
                              scene->pick_request, 0, 0);
      });
}

void SendPicked(PixelScene *scene) {
  for (int sent = 0; sent < kPickResultsPerTick && !scene->picked.empty();
       ++sent) {
    const std::string path = std::move(scene->picked.front());
    scene->picked.pop_front();
    if (!scene->session.Send(plugin_api::Kind::kOperationResult,
                             scene->pick_request, 1,
                             scene->picked.empty() ? 0 : 1, nullptr,
                             path.c_str()))
      return;
  }
}

void HandleMessage(PixelScene *scene, const plugin_api::Message &message) {
  switch (message.kind) {
    case plugin_api::Kind::kPresent: {
      std::vector<uint32_t> released;
      if (!scene->surface.Present(message.request_id, message.value,
                                  message.flags, released)) {
        Stop(scene, "Plugin stopped",
             "The plugin presented a frame it does not own.");
        return;
      }
      // While paused the plugin's frames wait for the next SURFACE, which
      // gives every slot back.
      if (!scene->paused) SendFrameDone(scene, released);
      break;
    }
    case plugin_api::Kind::kKeyboardShow:
      // Unknown purposes get the text keyboard.
      ShowKeyboard(scene, message.value, message.flags & 1U);
      break;
    case plugin_api::Kind::kKeyboardHide:
      HideKeyboard(scene);
      break;
    case plugin_api::Kind::kSetStatus:
      // Shown only while there is no frame yet (start-up and failures).
      if (!scene->showing && scene->detail)
        i18n::BindLabel(scene->detail, message.text);
      break;
    case plugin_api::Kind::kClose: {
      const bool on_screen = scene->screen != nullptr;
      Stop(scene, "Plugin closed", "");
      if (on_screen) Leave(scene);
      break;
    }
    case plugin_api::Kind::kRequestOperation:
      if (message.value ==
          static_cast<uint32_t>(plugin_api::Operation::kPickFiles)) {
        StartPick(scene, message);
        break;
      }
      // The other host operations stay with the declarative API.
      scene->session.Send(plugin_api::Kind::kOperationResult,
                          message.request_id, 0, 0, nullptr,
                          "Unsupported host operation.");
      break;
    default:
      // Host API 2's page messages have no meaning on a pixel surface.
      Stop(scene, "Plugin stopped",
           "The plugin sent a message Host API 3 does not allow.");
      break;
  }
}

void ShowNextFrame(PixelScene *scene) {
  if (!scene->image || scene->frame_waiting_for_refresh) return;
  const uint8_t *pixels = scene->surface.LatchNext();
  if (!pixels) return;
  scene->descriptor.data = pixels;
  lv_image_cache_drop(&scene->descriptor);
  lv_image_set_src(scene->image, &scene->descriptor);
  lv_obj_invalidate(scene->image);
  scene->frame_waiting_for_refresh = true;
  if (!scene->showing) {
    scene->showing = true;
    lv_obj_add_flag(scene->loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(scene->viewport, LV_OBJ_FLAG_HIDDEN);
  }
}

bool Launch(PixelScene *scene) {
  plugin_api::LaunchOptions options;
  options.host_api = plugin_api::kProtocolVersion3;
  options.surface_fd = scene->surface.PluginFd();
  options.data_dir = plugins::DataDirectory(scene->plugin);
  options.data_volatile = plugins::DataDirectoryVolatile(options.data_dir);
  int control = -1;
  std::string error;
  if (!scene->process.Start(scene->preparation.directory, control, error,
                            options)) {
    Stop(scene, "Plugin unavailable", error);
    return false;
  }
  if (!scene->session.Adopt(control, plugin_api::kProtocolVersion3,
                            {scene->surface.SurfaceMessage()})) {
    Stop(scene, "Plugin unavailable", scene->session.Status());
    return false;
  }
  return true;
}

void Tick(lv_timer_t *timer) {
  auto *scene = static_cast<PixelScene *>(lv_timer_get_user_data(timer));
  if (scene->stopped) return;
  if (!scene->preparation.done.load(std::memory_order_acquire)) {
    if (scene->progress)
      lv_bar_set_value(scene->progress, scene->preparation.progress.load(),
                       LV_ANIM_OFF);
    return;
  }
  if (!scene->preparation.verified) {
    Stop(scene, "Plugin unavailable", scene->preparation.error);
    return;
  }
  if (!scene->launched) {
    scene->launched = true;
    scene->launched_ms = NowMs();
    lv_bar_set_value(scene->progress, 100, LV_ANIM_ON);
    i18n::BindLabel(scene->detail, "Starting");
    if (!Launch(scene)) return;
  }
  const bool was_negotiated = scene->session.Negotiated();
  for (const auto &message : scene->session.Poll()) {
    HandleMessage(scene, message);
    if (scene->stopped) return;
  }
  if (!was_negotiated && scene->session.Negotiated() &&
      scene->viewport) {
    // Back now belongs to the plugin, as on Android.
    lv_obj_set_user_data(scene->viewport, &kPersistentModalMarker);
    scene->keyboard_inset = 0;
    SendKeyboardInset(scene);
  }
  if (!scene->session.Connected()) {
    Stop(scene, "Plugin stopped", scene->session.Status());
    return;
  }
  if (!scene->session.Negotiated() && NowMs() - scene->launched_ms > 5000) {
    Stop(scene, "Plugin unavailable", "Plugin handshake timed out");
    return;
  }
  SendPicked(scene);
  ShowNextFrame(scene);
  // Still visible but not taking input: a plugin that ignores INACTIVE
  // keeps working, it just gets no touches meanwhile.
  if (scene->screen && scene->session.Negotiated() && !scene->paused) {
    const bool covered = scene->picker || StatusBarShadeOpen() ||
                         SheetVisible(scene->screen);
    if (covered != scene->inactive) {
      scene->inactive = covered;
      scene->session.Send(plugin_api::Kind::kLifecycle, 0,
          static_cast<uint32_t>(covered ? plugin_api::Lifecycle::kInactive
                                        : plugin_api::Lifecycle::kResume));
    }
  }
  if (!scene->process.Running()) {
    Stop(scene, "Plugin stopped",
         "Plugin process exited; its resources were reclaimed");
  }
}

// Sends a touch at screen point (x, y), mapped to surface pixels.
bool SendTouch(PixelScene *s, plugin_api::Kind kind, int slot, int x, int y) {
  const int px = std::clamp(static_cast<int>(std::lround(
      (x - s->view_left) * s->pixels_per_unit_x)), 0,
      static_cast<int>(s->surface.Geometry().width) - 1);
  const int py = std::clamp(static_cast<int>(std::lround(
      (y - s->view_top) * s->pixels_per_unit_y)), 0,
      static_cast<int>(s->surface.Geometry().height) - 1);
  return s->session.Send(kind, static_cast<uint32_t>(slot),
                         static_cast<uint32_t>(px), static_cast<uint32_t>(py));
}

// The surface for the area below the status bar, in panel pixels.
plugin_api::SurfaceGeometry ViewGeometry(PixelScene *scene) {
  plugin_api::SurfaceGeometry geometry;
  geometry.width = static_cast<uint32_t>(
      std::lround(scene->view_width * scene->pixels_per_unit_x));
  geometry.height = static_cast<uint32_t>(
      std::lround(scene->view_height * scene->pixels_per_unit_y));
  geometry.stride = geometry.width * 4;
  geometry.slots = 3;
  geometry.scale = DeviceScale(static_cast<int>(
      std::min(geometry.width, geometry.height)));
  geometry.refresh_hz = 60.0;
  return geometry;
}

// Places the viewport, image and keyboard for the screen's current
// orientation and returns the surface geometry that fits it.
plugin_api::SurfaceGeometry Layout(PixelScene *scene) {
  lv_obj_t *screen = scene->screen;
  // Full width below the status bar; the dock is hidden on app surfaces.
  scene->view_left = 0;
  scene->view_top = StatusBarHeight();
  scene->view_width = lv_obj_get_width(screen);
  scene->view_height = lv_obj_get_height(screen) - scene->view_top;
  // Each axis of the canvas maps onto the same axis of the panel, so the
  // surface is exactly as wide and as tall as the glass that shows it.
  const int screen_width = lv_obj_get_width(screen);
  const int screen_height = lv_obj_get_height(screen);
  const int panel_short = std::min(gr_fb_width(), gr_fb_height());
  const int panel_long = std::max(gr_fb_width(), gr_fb_height());
  const bool landscape = screen_width > screen_height;
  const int panel_width = landscape ? panel_long : panel_short;
  const int panel_height = landscape ? panel_short : panel_long;
  scene->pixels_per_unit_x = screen_width > 0 && panel_width > 0
      ? std::min(1.0, static_cast<double>(panel_width) / screen_width) : 1.0;
  scene->pixels_per_unit_y = screen_height > 0 && panel_height > 0
      ? std::min(1.0, static_cast<double>(panel_height) / screen_height)
      : 1.0;
  const plugin_api::SurfaceGeometry geometry = ViewGeometry(scene);
  if (scene->viewport) {
    lv_obj_set_pos(scene->viewport, scene->view_left, scene->view_top);
    lv_obj_set_size(scene->viewport, scene->view_width, scene->view_height);
  }
  if (scene->image) {
    lv_image_set_scale_x(scene->image, static_cast<uint32_t>(
        (scene->view_width * 256 + geometry.width / 2) / geometry.width));
    lv_image_set_scale_y(scene->image, static_cast<uint32_t>(
        (scene->view_height * 256 + geometry.height / 2) / geometry.height));
  }
  scene->descriptor.header.w = geometry.width;
  scene->descriptor.header.h = geometry.height;
  scene->descriptor.header.stride = geometry.stride;
  scene->descriptor.data_size = static_cast<uint32_t>(geometry.FrameBytes());
  if (scene->keyboard) {
    const bool landscape = Landscape(screen);
    lv_obj_set_size(scene->keyboard, landscape ? 2408 : scene->view_width,
                    landscape ? 720 : 760);
    lv_obj_align(scene->keyboard, landscape ? LV_ALIGN_BOTTOM_RIGHT
                                            : LV_ALIGN_BOTTOM_MID, 0, 0);
  }
  return geometry;
}

// AERA rotated: keep the plugin running and give it a surface of the new
// shape in the same memory (sized for both orientations at the start).
void Rotate(PixelScene *scene) {
  const auto geometry = Layout(scene);
  if (geometry.width == scene->surface.Geometry().width &&
      geometry.height == scene->surface.Geometry().height)
    return;
  ReleaseTouches(scene);
  // Nothing of the old shape stays on screen.
  scene->frame_waiting_for_refresh = false;
  if (scene->image) lv_image_set_src(scene->image, nullptr);
  lv_image_cache_drop(&scene->descriptor);
  if (!scene->surface.Reshape(geometry)) {
    Stop(scene, "Plugin stopped", "Could not resize the plugin's display surface.");
    return;
  }
  if (scene->showing && scene->viewport) {
    scene->showing = false;
    lv_obj_add_flag(scene->viewport, LV_OBJ_FLAG_HIDDEN);
  }
  if (scene->session.Negotiated()) {
    scene->session.Send(scene->surface.SurfaceMessage());
    scene->keyboard_inset = UINT32_MAX;
    SendKeyboardInset(scene);
  }
}

// Lets go of the scene's screen and widgets. A plugin that is kept first
// gets what it is waiting for: a closed picker, the paths already chosen,
// its touches.
void Release(PixelScene *scene, bool keep) {
  if (gPixelScene == scene) gPixelScene = nullptr;
  if (scene->display)
    lv_display_remove_event_cb_with_user_data(scene->display,
                                              PixelRefreshReady, scene);
  if (keep) {
    if (scene->picker) {
      file_picker::Forget(scene->picker);
      scene->picker = nullptr;
      scene->session.Send(plugin_api::Kind::kOperationResult,
                          scene->pick_request, 0, 0);
    }
    while (!scene->picked.empty()) SendPicked(scene);
    ReleaseTouches(scene);
    scene->keyboard_inset = UINT32_MAX;
  }
  if (scene->image) lv_image_set_src(scene->image, nullptr);
  lv_image_cache_drop(&scene->descriptor);
  scene->screen = nullptr;
  scene->display = nullptr;
  scene->loading = scene->title = scene->detail = scene->progress = nullptr;
  scene->viewport = scene->image = scene->keyboard = nullptr;
  scene->frame_waiting_for_refresh = false;
  scene->showing = false;
}

// The scene is going away. A running plugin is paused and kept, as Browser
// is; one that is stopped, still starting or being discarded goes with it.
void Detach(PixelScene *scene) {
  const auto running = gRunning.find(scene->plugin.id);
  const bool keep = !scene->stopped && !scene->leaving && !scene->discard &&
                    scene->session.Connected() && scene->session.Negotiated() &&
                    running != gRunning.end() && running->second == scene;
  Release(scene, keep);
  if (!keep) {
    delete scene;
    return;
  }
  // From here AERA sends only RESUME (with a new SURFACE first), STOP or
  // CLOSE, and no FRAME_DONE: the plugin's rendering stalls by itself.
  scene->inactive = false;
  scene->paused = true;
  scene->session.Send(plugin_api::Kind::kLifecycle, 0,
                      static_cast<uint32_t>(plugin_api::Lifecycle::kPause));
}

// Opens a paused plugin's scene again: a new SURFACE (every slot is the
// plugin's again, in the current orientation), its keyboard inset, RESUME.
void Resume(PixelScene *scene) {
  const auto geometry = Layout(scene);
  if (!scene->surface.Reshape(geometry)) {
    Stop(scene, "Plugin stopped",
         "Could not resize the plugin's display surface.");
    return;
  }
  scene->paused = false;
  lv_bar_set_value(scene->progress, 100, LV_ANIM_OFF);
  i18n::BindLabel(scene->detail, "");
  // Back belongs to the plugin, as on Android.
  lv_obj_set_user_data(scene->viewport, &kPersistentModalMarker);
  scene->session.Send(scene->surface.SurfaceMessage());
  SendKeyboardInset(scene);
  scene->session.Send(plugin_api::Kind::kLifecycle, 0,
                      static_cast<uint32_t>(plugin_api::Lifecycle::kResume));
}
}  // namespace

void ShutdownPixelPlugin(const std::string &id) {
  const auto running = gRunning.find(id);
  if (running == gRunning.end()) return;
  PixelScene *scene = running->second;
  // The one on screen stops when its scene is left; a paused one now.
  if (scene->screen) scene->discard = true;
  else delete scene;
}

void ShutdownPixelPlugins() {
  std::vector<std::string> ids;
  for (const auto &running : gRunning) ids.push_back(running.first);
  for (const auto &id : ids) ShutdownPixelPlugin(id);
}

bool PixelPluginActive() {
  return gPixelScene && !gPixelScene->stopped;
}


bool PixelPluginHandlePointer(int slot, int x, int y, bool pressed) {
  auto *s = gPixelScene;
  if (!s || slot < 0 || slot >= kMaxPointers || !s->showing || s->stopped ||
      !s->session.Negotiated())
    return false;
  if (StatusBarShadeOpen() || SheetVisible(s->screen)) {
    ReleaseTouches(s);
    return false;
  }
  const bool inside = x >= s->view_left && y >= s->view_top &&
      x < s->view_left + s->view_width && y < s->view_top + s->view_height;
  if (s->touch_ignored[slot]) {
    if (!pressed) s->touch_ignored[slot] = false;
    return false;
  }
  // A first contact in a side-edge zone may be AERA's Back swipe, which the
  // engine tracks while we return false. Decide by movement, as Android's
  // edge Back does: once it moves inward past the touch slop, mostly
  // horizontally, it is Back and stays AERA's; a tap, or a contact that
  // moves along the edge, is the plugin's and starts where it landed.
  const int screen_width = lv_obj_get_width(s->screen);
  const int edge = std::max(72, screen_width / 20);
  if (slot == 0 && s->edge_pending) {
    const int inward = s->edge_x <= edge ? x - s->edge_x : s->edge_x - x;
    const int along = std::abs(y - s->edge_y);
    const int slop = std::max(16, edge / 3);
    if (pressed && inward <= slop && along <= slop) return false;
    s->edge_pending = false;
    if (pressed && inward > along) {
      s->touch_ignored[slot] = true;
      return false;
    }
    // Claim the contact: the engine cancels its edge swipe when we return
    // true. Deliver the press where it landed, then this event.
    if (!SendTouch(s, plugin_api::Kind::kTouchDown, slot, s->edge_x, s->edge_y))
      return false;
    s->touch_pressed[slot] = true;
  }
  const bool was_pressed = s->touch_pressed[slot];
  if (pressed && !was_pressed) {
    bool on_keyboard = false;
    if (s->keyboard && !lv_obj_has_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN)) {
      lv_area_t area{};
      lv_obj_get_coords(s->keyboard, &area);
      on_keyboard = x >= area.x1 && x <= area.x2 && y >= area.y1 &&
                    y <= area.y2;
    }
    if (!inside || on_keyboard) {
      s->touch_ignored[slot] = true;
      return false;
    }
    if (slot == 0 && (x <= edge || x >= screen_width - edge)) {
      s->edge_pending = true;
      s->edge_x = x;
      s->edge_y = y;
      return false;
    }
  } else if (!was_pressed) {
    return false;
  }
  const auto kind = !pressed ? plugin_api::Kind::kTouchUp
      : was_pressed ? plugin_api::Kind::kTouchMove
                    : plugin_api::Kind::kTouchDown;
  SendTouch(s, kind, slot, x, y);
  s->touch_pressed[slot] = pressed;
  return true;
}

void BuildPixelPluginScene(lv_obj_t *screen, const plugins::Plugin &plugin,
                           ActionCallback callback, void *context) {
  // The same plugin still running and paused resumes; an updated one starts
  // afresh.
  PixelScene *scene = nullptr;
  const auto running = gRunning.find(plugin.id);
  if (running != gRunning.end()) {
    PixelScene *current = running->second;
    const bool same = !current->stopped && !current->leaving &&
        !current->discard && current->session.Negotiated() &&
        current->plugin.payload_sha256 == plugin.payload_sha256;
    if (same && current->screen) {
      // Opened again while its scene is still up (its own Recents card): it
      // moves to the new screen, and the old one goes without pausing it.
      lv_obj_remove_event_cb_with_user_data(current->screen, nullptr, current);
      Release(current, true);
    }
    if (same) scene = current;
    else if (!current->screen) delete current;
  }
  const bool resume = scene != nullptr;
  if (!scene) {
    scene = new PixelScene;
    scene->plugin = plugin;
    gRunning[plugin.id] = scene;
  }
  gPixelScene = scene;
  scene->screen = screen;
  scene->display = lv_obj_get_display(screen);
  scene->callback = callback;
  scene->context = context;
  scene->leaving = false;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    Detach(static_cast<PixelScene *>(lv_event_get_user_data(event)));
  }, LV_EVENT_DELETE, scene);
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);

  // The view's size, for the loading panel; widgets are placed below.
  Layout(scene);

  scene->viewport = lv_obj_create(screen);
  Clear(scene->viewport);
  lv_obj_remove_flag(scene->viewport, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(scene->viewport, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(scene->viewport, [](lv_event_t *event) {
    auto *scene = static_cast<PixelScene *>(lv_event_get_user_data(event));
    if (scene->session.Negotiated()) {
      ReleaseTouches(scene);
      scene->session.Send(plugin_api::Kind::kBack);
    }
  }, LV_EVENT_CANCEL, scene);
  scene->image = lv_image_create(scene->viewport);
  lv_image_set_pivot(scene->image, 0, 0);
  lv_obj_remove_flag(scene->image, LV_OBJ_FLAG_CLICKABLE);
  scene->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  // BGRA bytes are LVGL's ARGB8888. USER1 selects the OpenGL renderer's
  // streaming-texture path, as for Browser.
  scene->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  scene->descriptor.header.flags =
      LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;

  scene->loading = lv_obj_create(screen);
  Panel(scene->loading, 44, kMainPanel);
  lv_obj_set_size(scene->loading, std::min(1312, scene->view_width - 128),
                  720);
  lv_obj_align(scene->loading, LV_ALIGN_CENTER, 0, 0);
  scene->title = Label(scene->loading, plugin.name.c_str(),
                       &lv_font_montserrat_48, kText);
  lv_obj_set_width(scene->title, std::min(1150, scene->view_width - 240));
  lv_obj_set_style_text_align(scene->title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(scene->title, LV_ALIGN_TOP_MID, 0, 120);
  scene->detail = Label(scene->loading,
      "Verifying the plugin runtime and expanding it into private RAM.",
      &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(scene->detail, std::min(1080, scene->view_width - 240));
  lv_obj_set_style_text_align(scene->detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(scene->detail, 16, 0);
  lv_obj_align(scene->detail, LV_ALIGN_TOP_MID, 0, 240);
  scene->progress = lv_bar_create(scene->loading);
  lv_obj_set_size(scene->progress, std::min(1020, scene->view_width - 300),
                  16);
  lv_obj_align(scene->progress, LV_ALIGN_TOP_MID, 0, 520);
  lv_obj_set_style_bg_color(scene->progress, kAccent, LV_PART_INDICATOR);
  Navigation(screen, Action::kNone, callback, context, true);

  scene->keyboard = lv_keyboard_create(screen);
  lv_obj_set_user_data(scene->keyboard, &kPersistentModalMarker);
  phone_keyboard::Apply(scene->keyboard);
  lv_obj_add_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(scene->keyboard, [](lv_event_t *event) {
    auto *scene = static_cast<PixelScene *>(lv_event_get_user_data(event));
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
      HideKeyboard(scene);
    } else if (code == LV_EVENT_READY) {
      // OK submits a single-line field, as Enter would.
      if (!scene->keyboard_multiline && scene->session.Negotiated())
        scene->session.Send(plugin_api::Kind::kKey, 0, 13);
      HideKeyboard(scene);
    } else if (code == LV_EVENT_VALUE_CHANGED &&
               scene->session.Negotiated()) {
      const auto selected = lv_buttonmatrix_get_selected_button(
          scene->keyboard);
      const char *key = lv_buttonmatrix_get_button_text(scene->keyboard,
                                                        selected);
      const uint32_t code_point = key ? KeyCode(key) : 0;
      if (code_point)
        scene->session.Send(plugin_api::Kind::kKey, 0, code_point);
    }
  }, LV_EVENT_ALL, scene);

  lv_display_add_event_cb(scene->display, PixelRefreshReady,
                          LV_EVENT_REFR_READY, scene);
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    auto *scene = static_cast<PixelScene *>(lv_event_get_user_data(event));
    if (!scene->stopped) Rotate(scene);
  }, LV_EVENT_SIZE_CHANGED, scene);
  if (resume) {
    Resume(scene);
    return;
  }

  const plugin_api::SurfaceGeometry geometry = Layout(scene);
  // Room for the surface in either orientation, so a rotation keeps the
  // plugin and its memory (pages are only allocated as they are drawn).
  const uint64_t long_side = static_cast<uint64_t>(std::lround(
      std::max(lv_obj_get_width(screen), lv_obj_get_height(screen)) *
      std::max(scene->pixels_per_unit_x, scene->pixels_per_unit_y)));
  if (!scene->surface.Create(geometry,
                             long_side * long_side * 4 * geometry.slots)) {
    Stop(scene, "Plugin unavailable",
         "Could not create the plugin's display surface.");
    return;
  }
  scene->worker = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, scene->plugin.id.c_str(),
                              scene->plugin.type.c_str(),
                              scene->plugin.entry.c_str());
  });
  scene->timer = lv_timer_create(Tick, 4, scene);
}

}  // namespace aeraui
