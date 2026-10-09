// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <vector>
#include "../core/status_bar.cpp"

namespace aeraui {
std::string RecoveryUiFont() { return {}; }
bool RecoveryPreference(Preference) { return true; }
int RecoveryBrightness() { return 50; }
void RecoverySetBrightness(int) {}
bool RecoveryFlashlightSupported() { return false; }
bool RecoveryFlashlightEnabled() { return false; }
bool RecoverySetFlashlight(bool) { return false; }
WifiStatus RecoveryWifiStatus() { return {}; }
WifiConnection RecoveryWifiConnection() { return {}; }
void RecoveryVibrate(Haptic) {}
}
namespace aeraui::web { DownloadSummary CurrentDownloadSummary() { return {}; } }
namespace aeraui::plugin_api { MirrorMode ActiveMirrorMode() { return MirrorMode::kOff; } }
namespace aeraui::update { Snapshot GetSnapshot() { return {}; } }
namespace aeraui::recorder {
Snapshot GetSnapshot() { return {}; }
bool Active() { return false; }
bool Installed() { return false; }
}

static lv_indev_data_t pointer{};
static lv_indev_t *input;
static void Touch(int x, int y, bool pressed) {
  pointer.point = {x, y};
  pointer.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  lv_indev_read(input);
}
static void Settle() {
  for (int i = 0; i < 12; ++i) { lv_tick_inc(32); lv_timer_handler(); }
}

int main() {
  using namespace aeraui;
  lv_init(); i18n::Initialize("en");
  auto *display = lv_display_create(1440, 3168);
  std::vector<uint8_t> frame(1440 * 3168 * 4);
  lv_display_set_buffers(display, frame.data(), nullptr, frame.size(), LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); });
  auto *screen = lv_screen_active();
  StatusState state{}; state.screen = screen; state.callback = [](Action, void *) {};
  state.bar = lv_obj_create(screen);
  design::Clear(state.bar); lv_obj_set_size(state.bar, 1440, 165);
  lv_obj_add_flag(state.bar, LV_OBJ_FLAG_CLICKABLE);
  for (auto event : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
    lv_obj_add_event_cb(state.bar, StatusGesture, event, &state);
  input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(input, [](lv_indev_t *, lv_indev_data_t *data) { *data = pointer; });
  lv_obj_update_layout(screen);

  Touch(400, 2800, true); Touch(400, 2800, false);
  assert(state.shade == nullptr);
  Touch(400, 80, true); Touch(400, 80, false); Settle();
  assert(state.shade && state.shade_visible == state.shade_height);
  AnimateShade(&state, false); Settle();

  Touch(400, 80, true);
  lv_obj_send_event(state.bar, LV_EVENT_PRESS_LOST, nullptr);
  Touch(400, 80, false); Settle();
  assert(state.shade == nullptr && !state.dragging && !StatusBarShadeOpen());

  Touch(400, 80, true); Touch(800, 80, true); Touch(800, 80, false); Settle();
  assert(state.shade == nullptr);
  Touch(400, 80, true); Touch(400, 480, true); Touch(400, 480, false); Settle();
  assert(state.shade && state.shade_visible == state.shade_height);
  AnimateShade(&state, false); Settle();

  Touch(400, 80, true); Touch(400, 480, true);
  lv_obj_send_event(state.bar, LV_EVENT_PRESS_LOST, nullptr);
  Touch(400, 480, false); Settle();
  assert(state.shade == nullptr && !state.dragging);
  lv_indev_delete(input); lv_display_delete(display); lv_deinit();
  puts("Bottom taps, status taps, pull-down, horizontal motion and canceled gestures passed");
}
