/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "ui_components.hpp"
#include "phone_keyboard.hpp"

namespace aeraui {
namespace {
using namespace widgets;
using namespace design;

constexpr int32_t kWifiIconSize = 72;
#include "wifi_open_icon.inc"
#include "wifi_locked_icon.inc"
#include "wifi_saved_icon.inc"

#define AERA_WIFI_IMAGE(name, pixels)                                    \
  static const lv_image_dsc_t name = {                                  \
      .header = {.magic = LV_IMAGE_HEADER_MAGIC,                         \
                 .cf = LV_COLOR_FORMAT_A8,                              \
                 .flags = 0,                                            \
                 .w = kWifiIconSize,                                    \
                 .h = kWifiIconSize,                                    \
                 .stride = kWifiIconSize},                              \
      .data_size = sizeof(pixels),                                      \
      .data = pixels,                                                    \
  }

AERA_WIFI_IMAGE(kWifiOpenImage, _tmp_aera_wlan_raw);
AERA_WIFI_IMAGE(kWifiLockedImage, _tmp_aera_wlan_locked_raw);
AERA_WIFI_IMAGE(kWifiSavedImage, _tmp_aera_wlan_saved_raw);
#undef AERA_WIFI_IMAGE

WifiRequest gRequest;

struct WifiUi {
  WifiScene scene;
  lv_obj_t *screen = nullptr;
  lv_obj_t *radio_switch = nullptr;
  lv_obj_t *hero = nullptr;
  lv_obj_t *status_plate = nullptr;
  lv_obj_t *status_icon = nullptr;
  lv_timer_t *timer = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  WifiStatus snapshot;
  std::string signature;
  WifiOperation running = WifiOperation::kScan;
  bool busy = false;
  bool activity_animating = false;
  uint32_t busy_phase = 0;
  int list_width = 1312;
};

std::string Signature(const WifiStatus &status) {
  std::string value = status.state + "|" + status.ssid + "|" +
      (status.enabled ? "1" : "0") + (status.connected ? "1" : "0");
  for (const auto &network : status.networks)
    value += "|" + network.ssid + ":" + network.security +
        (network.saved ? ":s" : "") + (network.connected ? ":c" : "");
  return value;
}

void Dispatch(WifiUi *state, WifiRequest request) {
  if (!state || state->busy) return;
  gRequest = std::move(request);
  state->callback(Action::kRunWifiOperation, state->context);
}

void CloseOverlay(lv_obj_t *overlay) { lv_obj_delete_async(overlay); }

void SelectNetwork(WifiUi *state, WifiNetwork network);

void StyleSwitch(lv_obj_t *toggle, bool enabled) {
  lv_obj_set_size(toggle, 108, 60);
  lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(toggle, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER,
                          LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(toggle, kAccent,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(toggle, kText, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(toggle, -8, LV_PART_KNOB);
  if (enabled) lv_obj_add_state(toggle, LV_STATE_CHECKED);
  else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
}

lv_obj_t *SettingsRow(lv_obj_t *parent, int y, const char *title,
                      const char *detail, bool enabled,
                      std::function<bool(bool)> setter,
                      lv_obj_t *message) {
  auto *row = lv_button_create(parent);
  Panel(row, 30, kMainPanel);
  Interactive(row, kMainSelected);
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_size(row, 1216, 160);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_color(row, kMainLine, 0);
  lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
  auto *name = Label(row, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 32, 24);
  auto *copy = Label(row, detail, &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(copy, 32, 88);
  lv_obj_set_width(copy, 940);
  auto *toggle = lv_switch_create(row);
  StyleSwitch(toggle, enabled);
  lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -32, 0);
  OnClick(row, [toggle, setter = std::move(setter), message] {
    const bool desired = !lv_obj_has_state(toggle, LV_STATE_CHECKED);
    if (setter(desired)) {
      if (desired) lv_obj_add_state(toggle, LV_STATE_CHECKED);
      else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
      i18n::BindLabel(message, "Settings saved");
      lv_obj_set_style_text_color(message, kGreen, 0);
    } else {
      i18n::BindLabel(message,
          "Could not change this setting. Connect Wi-Fi before enabling wireless ADB.");
      lv_obj_set_style_text_color(message, kRed, 0);
    }
  });
  return row;
}

void ShowWifiSettings(WifiUi *state) {
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);

  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, 1312, 940);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_set_style_pad_all(sheet, 48, 0);
  lv_obj_set_style_bg_opa(sheet, IsLightMode() ? LV_OPA_90 : LV_OPA_80, 0);
  lv_obj_set_style_blur_backdrop(sheet, true, 0);
  lv_obj_set_style_blur_radius(sheet, 18, 0);

  Label(sheet, "Wi-Fi settings", &lv_font_montserrat_48, kText);
  auto *subtitle = Label(sheet, "Startup and developer connectivity",
                         &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, 0, 70);
  auto *message = Label(sheet, "Changes apply immediately",
                        &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(message, 0, 690);
  lv_obj_set_width(message, 1216);

  SettingsRow(sheet, 130, "Auto-enable",
      "Turn Wi-Fi on when recovery starts", RecoveryWifiAutoEnable(),
      [](bool enabled) { return RecoverySetWifiAutoEnable(enabled); }, message);
  SettingsRow(sheet, 310, "Auto-connect",
      "Reconnect to a saved network automatically", RecoveryWifiAutoConnect(),
      [](bool enabled) { return RecoverySetWifiAutoConnect(enabled); }, message);
  SettingsRow(sheet, 490, "ADB over Wi-Fi",
      "Secure wireless ADB using authorized host keys", RecoveryAdbOverWifi(),
      [](bool enabled) { return RecoverySetAdbOverWifi(enabled); }, message);

  auto *close = Button(sheet, "Done", [overlay] { CloseOverlay(overlay); }, true);
  lv_obj_set_pos(close, 0, 760);
  lv_obj_set_size(close, 1216, 116);
  AnimateEnter(sheet, 0, 38);
}

void SetActivity(WifiUi *state, bool active) {
  if (state == nullptr || state->scene.activity == nullptr) return;
  if (active && !state->activity_animating) {
    state->activity_animating = true;
    lv_obj_remove_flag(state->scene.activity, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t spin;
    lv_anim_init(&spin);
    lv_anim_set_var(&spin, state->scene.activity);
    lv_anim_set_values(&spin, 0, 3600);
    lv_anim_set_duration(&spin, 900);
    lv_anim_set_repeat_count(&spin, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&spin, [](void *target, int32_t angle) {
      lv_obj_set_style_transform_rotation(static_cast<lv_obj_t *>(target),
                                          angle, 0);
    });
    lv_anim_start(&spin);
  } else if (!active && state->activity_animating) {
    state->activity_animating = false;
    lv_anim_delete(state->scene.activity, nullptr);
    lv_obj_set_style_transform_rotation(state->scene.activity, 0, 0);
    lv_obj_add_flag(state->scene.activity, LV_OBJ_FLAG_HIDDEN);
  }
}

std::string BusyTitle(const WifiUi *state) {
  const char *base = "Updating Wi-Fi";
  switch (state->running) {
    case WifiOperation::kEnable: base = "Turning Wi-Fi on"; break;
    case WifiOperation::kDisable: base = "Turning Wi-Fi off"; break;
    case WifiOperation::kDisconnect: base = "Disconnecting"; break;
    case WifiOperation::kScan: base = "Scanning nearby"; break;
    case WifiOperation::kConnect: base = "Connecting"; break;
    case WifiOperation::kForget: base = "Forgetting network"; break;
    case WifiOperation::kTest: base = "Testing connection"; break;
  }
  std::string title(base);
  title.append(1 + (state->busy_phase % 3), '.');
  return title;
}

void EmptyNetworkCard(WifiUi *state, const char *icon, const char *title,
                      const char *detail) {
  auto *card = lv_obj_create(state->scene.list);
  Panel(card, 34, kMainSheet);
  lv_obj_set_pos(card, 0, 0);
  const int width = state->list_width;
  lv_obj_set_size(card, width, 230);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
  auto *plate = IconPlate(card, icon, kAccent, kAccentSoft, 104);
  lv_obj_set_pos(plate, 34, 63);
  auto *name = Label(card, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 174, 56);
  auto *copy = Label(card, detail, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 174, 118);
  lv_obj_set_width(copy, width - 260);
  AnimateEnter(card, 20, 10);
}

void NetworkCard(WifiUi *state, int y, const WifiNetwork &network) {
  auto *card = lv_button_create(state->scene.list);
  // Connected rows stay on the normal page surface. Connectivity is shown
  // with green details only, without a competing cyan selection wash.
  Panel(card, 32, kMainSheet);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, 0, y);
  const int width = state->list_width;
  lv_obj_set_size(card, width, 174);
  lv_obj_set_style_transform_scale(card, 256, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
  OnClick(card, [state, network] { SelectNetwork(state, network); });

  auto *plate = lv_obj_create(card);
  Panel(plate, 28, kMainPanel);
  lv_obj_set_pos(plate, 28, 41);
  lv_obj_set_size(plate, 92, 92);
  const lv_image_dsc_t *source = network.saved ? &kWifiSavedImage :
      network.security == "OPEN" ? &kWifiOpenImage : &kWifiLockedImage;
  auto *network_icon = lv_image_create(plate);
  lv_image_set_src(network_icon, source);
  lv_obj_set_style_image_recolor(network_icon,
      network.connected ? kGreen : kAccent, 0);
  lv_obj_set_style_image_recolor_opa(network_icon, LV_OPA_COVER, 0);
  lv_obj_center(network_icon);
  auto *name = Label(card, network.ssid.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 152, 34);
  lv_obj_set_width(name, width - 300);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  std::string detail = network.security == "OPEN" ? "Open network" : network.security;
  if (network.saved) detail += "  /  Saved";
  if (network.connected) detail = "Connected  /  " + detail;
  auto *copy = Label(card, detail.c_str(), &lv_font_montserrat_24,
                     network.connected ? kGreen : kMuted);
  lv_obj_set_pos(copy, 152, 98);
  lv_obj_set_width(copy, width - 300);
  lv_label_set_long_mode(copy, LV_LABEL_LONG_DOT);
  const char *trailing = network.connected ? LV_SYMBOL_OK : LV_SYMBOL_RIGHT;
  auto *end = Label(card, trailing, &lv_font_montserrat_32,
                    network.connected ? kGreen : kMutedStrong);
  lv_obj_align(end, LV_ALIGN_RIGHT_MID, -38, 0);
  AnimateEnter(card, 18 + static_cast<uint32_t>(y / 8), 10);
}

void PasswordDialog(WifiUi *state, const WifiNetwork &network) {
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);

  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, 1312, 1260);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_set_style_pad_all(sheet, 48, 0);
  auto *title = Label(sheet, network.ssid.c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_width(title, 1180);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  const std::string password_hint =
      i18n::Format("%s network password", network.security.c_str());
  auto *hint = Label(sheet, password_hint.c_str(),
                     &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 0, 76);

  auto *input = TextArea(sheet);
  lv_obj_set_pos(input, 0, 130);
  lv_obj_set_size(input, 1216, 126);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_password_mode(input, true);
  lv_textarea_set_max_length(input, 63);
  lv_textarea_set_placeholder_text(input, "Password");
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_bg_color(input, kMainPanel, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(input, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(input, 24, 0);
  lv_obj_set_style_pad_all(input, 28, 0);

  auto *keyboard = lv_keyboard_create(sheet);
  phone_keyboard::Apply(keyboard);
  // LVGL keyboards default to BOTTOM_MID; use sheet-local coordinates so the
  // complete keyboard remains between the password field and action buttons.
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard, 0, 286);
  lv_obj_set_size(keyboard, 1216, 650);
  lv_keyboard_set_textarea(keyboard, input);

  auto *cancel = Button(sheet, "Cancel", [overlay] { CloseOverlay(overlay); });
  lv_obj_set_pos(cancel, 0, 996);
  lv_obj_set_size(cancel, 580, 116);
  auto *connect = Button(sheet, "Connect", [state, network, input, overlay] {
    WifiRequest request;
    request.operation = WifiOperation::kConnect;
    request.ssid = network.ssid;
    request.password = lv_textarea_get_text(input);
    CloseOverlay(overlay);
    Dispatch(state, std::move(request));
  }, true);
  lv_obj_set_pos(connect, 636, 996);
  lv_obj_set_size(connect, 580, 116);
  lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
  AnimateEnter(sheet, 0, 42);
}

void SavedDialog(WifiUi *state, const WifiNetwork &network) {
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, 1312, 700);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_set_style_pad_all(sheet, 48, 0);
  auto *title = Label(sheet, network.ssid.c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_width(title, 1180);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  const std::string saved_detail =
      (network.connected ? "Connected / " : "Saved / ") + network.security;
  auto *detail = Label(sheet, saved_detail.c_str(),
      &lv_font_montserrat_32, network.connected ? kGreen : kMutedStrong);
  lv_obj_set_pos(detail, 0, 100);
  auto *connect = Button(sheet, network.connected ? "Disconnect" : "Connect",
      [state, network, overlay] {
        WifiRequest request;
        request.operation = network.connected ? WifiOperation::kDisconnect
                                              : WifiOperation::kConnect;
        request.ssid = network.ssid;
        request.use_saved_credentials = !network.connected;
        CloseOverlay(overlay);
        Dispatch(state, std::move(request));
      }, !network.connected);
  lv_obj_set_pos(connect, 0, 240);
  lv_obj_set_size(connect, 1216, 116);
  if (network.connected) {
    lv_obj_set_style_bg_color(connect, kRedSoft, 0);
    lv_obj_set_style_bg_color(connect, kMainSelected, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(connect, 0), kRed, 0);
  }
  auto *forget = Button(sheet, "Forget saved network", [state, network, overlay] {
    WifiRequest request;
    request.operation = WifiOperation::kForget;
    request.ssid = network.ssid;
    CloseOverlay(overlay);
    Dispatch(state, std::move(request));
  });
  lv_obj_set_pos(forget, 0, 380);
  lv_obj_set_size(forget, 1216, 116);
  auto *cancel = Button(sheet, "Cancel", [overlay] { CloseOverlay(overlay); });
  lv_obj_set_pos(cancel, 0, 520);
  lv_obj_set_size(cancel, 1216, 116);
  AnimateEnter(sheet, 0, 42);
}

void SelectNetwork(WifiUi *state, WifiNetwork network) {
  if (state->busy) return;
  if (network.saved) {
    SavedDialog(state, network);
  } else if (network.security == "OPEN") {
    WifiRequest request;
    request.operation = WifiOperation::kConnect;
    request.ssid = network.ssid;
    Dispatch(state, std::move(request));
  } else {
    PasswordDialog(state, network);
  }
}

void Populate(WifiUi *state) {
  lv_obj_clean(state->scene.list);
  if (!state->snapshot.supported) {
    EmptyNetworkCard(state, LV_SYMBOL_WARNING, "Wi-Fi unavailable",
                     "Wireless support is not available on this device.");
    return;
  }
  if (!state->snapshot.enabled) {
    EmptyNetworkCard(state, LV_SYMBOL_WIFI, "Wi-Fi is off",
                     "Turn it on to discover nearby networks.");
    return;
  }
  if (state->snapshot.networks.empty()) {
    EmptyNetworkCard(state, LV_SYMBOL_REFRESH, "Looking for networks",
                     state->busy ? "Scanning nearby access points."
                                 : "No networks found. Tap Scan again.");
    return;
  }
  int y = 0;
  for (const auto &network : state->snapshot.networks) {
    NetworkCard(state, y, network);
    y += 192;
  }
}

void Refresh(WifiUi *state, bool force) {
  if (!state) return;
  state->snapshot = RecoveryWifiStatus();
  const auto &status = state->snapshot;
  const bool busy = state->busy || status.busy;
  const std::string busy_title = busy ? BusyTitle(state) : std::string();
  const char *title = !status.supported ? "Unavailable" :
      busy ? busy_title.c_str() : status.connected ? "Connected" :
      status.enabled ? "Ready to connect" : "Wi-Fi is off";
  i18n::BindLabel(state->scene.status, title);
  std::string detail;
  if (status.connected) {
    detail = status.ssid;
    if (!status.ip_address.empty()) detail += "  /  " + status.ip_address;
  } else if (busy) {
    if (!status.state.empty()) detail = status.state;
    else if (state->running == WifiOperation::kConnect && !gRequest.ssid.empty())
      detail = "Joining " + gRequest.ssid;
    else detail = "Updating wireless state";
  } else {
    detail = status.enabled ? "Choose a network below" : "Wireless radio is disabled";
  }
  i18n::BindLabel(state->scene.detail, detail.c_str());
  if (status.enabled) lv_obj_add_state(state->radio_switch, LV_STATE_CHECKED);
  else lv_obj_remove_state(state->radio_switch, LV_STATE_CHECKED);
  SetActivity(state, busy);
  // A successful connection should not ring the whole hero card in green;
  // the green Wi-Fi icon and status text already communicate that state.
  lv_obj_set_style_border_opa(state->hero, LV_OPA_TRANSP, 0);
  lv_obj_set_style_text_color(state->status_icon,
      status.connected ? kGreen : kAccent, 0);
  lv_obj_set_style_bg_color(state->status_plate, kMainPanel, 0);
  lv_obj_set_style_border_color(state->status_plate, kMainLine, 0);
  lv_obj_set_style_border_opa(state->status_plate, LV_OPA_TRANSP, 0);
  for (auto *control : {state->scene.toggle, state->scene.refresh, state->scene.test}) {
    if (busy) lv_obj_add_state(control, LV_STATE_DISABLED);
    else lv_obj_remove_state(control, LV_STATE_DISABLED);
  }
  const std::string signature = Signature(status);
  if (force || signature != state->signature) {
    state->signature = signature;
    Populate(state);
  }
}

void Timer(lv_timer_t *timer) {
  auto *state = static_cast<WifiUi *>(lv_timer_get_user_data(timer));
  if (state->busy) ++state->busy_phase;
  Refresh(state, false);
}
}  // namespace

WifiScene BuildWifiScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *state = new WifiUi;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  state->scene.state = state;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    auto *s = static_cast<WifiUi *>(lv_event_get_user_data(event));
    if (s->timer) lv_timer_delete(s->timer);
    delete s;
  }, LV_EVENT_DELETE, state);

  Header(screen, "Wi-Fi", "Networks & connectivity", callback, context);
  const bool landscape = Landscape(screen);
  auto *settings = Button(screen, LV_SYMBOL_SETTINGS "  Wi-Fi settings",
                          [state] { ShowWifiSettings(state); });
  lv_obj_set_size(settings, 360, 104);
  lv_obj_align(settings, LV_ALIGN_TOP_RIGHT, -64, landscape ? 184 : 232);
  lv_obj_set_style_radius(settings, 32, 0);
  auto *panel = lv_obj_create(screen);
  Panel(panel, 36, kMainSheet);
  state->hero = panel;
  lv_obj_set_pos(panel, 64, landscape ? 350 : 420);
  lv_obj_set_size(panel, landscape ? 1450 : 1312,
                  landscape ? 520 : 352);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_border_color(panel, kMainLine, 0);
  lv_obj_set_style_border_opa(panel, LV_OPA_TRANSP, 0);

  auto *status_plate = lv_obj_create(panel);
  state->status_plate = status_plate;
  Panel(status_plate, 34, kMainPanel);
  lv_obj_set_pos(status_plate, 40, 38);
  lv_obj_set_size(status_plate, 124, 124);
  lv_obj_set_style_border_width(status_plate, 1, 0);
  lv_obj_set_style_border_color(status_plate, kMainLine, 0);
  lv_obj_set_style_border_opa(status_plate, LV_OPA_TRANSP, 0);
  state->status_icon = Label(status_plate, LV_SYMBOL_WIFI,
                             &lv_font_montserrat_48, kAccent);
  lv_obj_center(state->status_icon);

  state->scene.status = Label(panel, "Checking...", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(state->scene.status, 198, 34);
  lv_obj_set_width(state->scene.status, 690);
  lv_label_set_long_mode(state->scene.status, LV_LABEL_LONG_DOT);
  state->scene.detail = Label(panel, "Reading wireless state", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->scene.detail, 198, 108);
  lv_obj_set_width(state->scene.detail, 690);
  lv_label_set_long_mode(state->scene.detail, LV_LABEL_LONG_DOT);
  state->scene.toggle = lv_button_create(panel);
  Panel(state->scene.toggle, 38, kMainPanel);
  Interactive(state->scene.toggle, kMainSelected);
  lv_obj_set_pos(state->scene.toggle, 930, 50);
  lv_obj_set_size(state->scene.toggle, 294, 96);
  lv_obj_set_style_border_width(state->scene.toggle, 1, 0);
  lv_obj_set_style_border_color(state->scene.toggle, kMainLine, 0);
  lv_obj_set_style_border_opa(state->scene.toggle, LV_OPA_30, 0);
  auto *radio_label = Label(state->scene.toggle, "Wi-Fi",
                            &lv_font_montserrat_24, kText);
  lv_obj_align(radio_label, LV_ALIGN_LEFT_MID, 28, 0);
  state->radio_switch = lv_switch_create(state->scene.toggle);
  StyleSwitch(state->radio_switch, false);
  lv_obj_align(state->radio_switch, LV_ALIGN_RIGHT_MID, -22, 0);
  OnClick(state->scene.toggle, [state] {
    WifiRequest request;
    request.operation = state->snapshot.enabled ? WifiOperation::kDisable : WifiOperation::kEnable;
    Dispatch(state, std::move(request));
  });

  state->scene.refresh = Button(panel, LV_SYMBOL_REFRESH "  Scan again", [state] {
    WifiRequest request;
    request.operation = WifiOperation::kScan;
    Dispatch(state, std::move(request));
  });
  lv_obj_set_pos(state->scene.refresh, 40, 210);
  lv_obj_set_size(state->scene.refresh, 587, 104);
  lv_obj_set_style_radius(state->scene.refresh, 30, 0);
  state->scene.test = Button(panel, LV_SYMBOL_GPS "  Test connection", [state] {
    WifiRequest request;
    request.operation = WifiOperation::kTest;
    Dispatch(state, std::move(request));
  });
  lv_obj_set_pos(state->scene.test, 651, 210);
  lv_obj_set_size(state->scene.test, 587, 104);
  lv_obj_set_style_radius(state->scene.test, 30, 0);

  auto *caption = Label(screen, "AVAILABLE NETWORKS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(caption, 3, 0);
  lv_obj_set_pos(caption, landscape ? 1580 : 80,
                 landscape ? 306 : 830);
  state->scene.list = Scroll(screen, landscape ? 350 : 884,
                             landscape ? 900 : 1960);
  if (landscape) {
    lv_obj_set_x(state->scene.list, 1560);
    lv_obj_set_width(state->scene.list, 1544);
    state->list_width = 1544;
  } else {
    lv_obj_set_x(state->scene.list, 64);
    lv_obj_set_width(state->scene.list, 1312);
    state->list_width = 1312;
  }
  Navigation(screen, Action::kSettings, callback, context);
  AnimateEnter(panel, 10, 14);
  AnimateEnter(settings, 30, 12);
  Refresh(state, true);
  state->timer = lv_timer_create(Timer, 260, state);
  return state->scene;
}

void SetWifiRequest(const WifiRequest &request) { gRequest = request; }
WifiRequest GetWifiRequest() { return gRequest; }

void SetWifiBusy(const WifiScene &scene, const WifiRequest &request) {
  auto *state = static_cast<WifiUi *>(scene.state);
  if (!state) return;
  state->busy = true;
  state->running = request.operation;
  state->busy_phase = 0;
  state->signature.clear();
  Refresh(state, true);
}

void CompleteWifiOperation(const WifiScene &scene, bool success) {
  auto *state = static_cast<WifiUi *>(scene.state);
  if (!state) return;
  const WifiOperation completed = state->running;
  state->busy = false;
  Refresh(state, true);
  if (completed == WifiOperation::kTest) {
    const auto status = RecoveryWifiStatus();
    Sheet(state->screen, success ? "Network test passed" : "Network test failed",
          status.test_result.empty() ? "No test details were returned." : status.test_result);
  } else if (!success) {
    Sheet(state->screen, "Wi-Fi action failed",
          "AERA could not complete the request. Check the password, signal and recovery log, then try again.");
  }
}
}  // namespace aeraui
