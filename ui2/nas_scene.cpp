/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene.hpp"

#include <algorithm>
#include <string>

#include "phone_keyboard.hpp"
#include "ui_components.hpp"

namespace recovery_ui2 {
namespace {
using namespace design;
using namespace widgets;

NasRequest gRequest;

enum class Field { kHost, kPort, kUser, kPassword, kShare, kPath, kDomain };

struct NasUi {
  NasScene scene;
  lv_obj_t *screen = nullptr;
  lv_obj_t *hero = nullptr;
  lv_obj_t *status_icon = nullptr;
  lv_obj_t *activity = nullptr;
  lv_timer_t *timer = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  NasStatus snapshot;
  NasOperation running = NasOperation::kMountAndUse;
  bool busy = false;
  bool activity_animating = false;
  uint32_t busy_phase = 0;
};

const char *FieldTitle(Field field) {
  switch (field) {
    case Field::kHost: return "Host or IP address";
    case Field::kPort: return "SFTP port";
    case Field::kUser: return "Username";
    case Field::kPassword: return "Password";
    case Field::kShare: return "SMB share name";
    case Field::kPath: return "Remote folder or path";
    case Field::kDomain: return "SMB domain or workgroup";
  }
  return "Network storage";
}

std::string FieldValue(const NasConfig &config, Field field) {
  switch (field) {
    case Field::kHost: return config.host;
    case Field::kPort: return config.port;
    case Field::kUser: return config.user;
    case Field::kPassword: return config.password;
    case Field::kShare: return config.share;
    case Field::kPath: return config.path;
    case Field::kDomain: return config.domain;
  }
  return {};
}

void SetField(NasConfig *config, Field field, const std::string &value) {
  if (config == nullptr) return;
  switch (field) {
    case Field::kHost: config->host = value; break;
    case Field::kPort: config->port = value; break;
    case Field::kUser: config->user = value; break;
    case Field::kPassword: config->password = value; break;
    case Field::kShare: config->share = value; break;
    case Field::kPath: config->path = value; break;
    case Field::kDomain: config->domain = value; break;
  }
}

uint32_t FieldLimit(Field field) {
  switch (field) {
    case Field::kPassword: return 254;
    case Field::kPath: return 256;
    case Field::kPort: return 5;
    default: return 128;
  }
}

void Populate(NasUi *state);

bool SaveConfig(NasUi *state, const NasConfig &config) {
  std::string error;
  if (!RecoverySetNasConfig(config, &error)) {
    Sheet(state->screen, "Invalid network storage setting",
          error.empty() ? "The setting could not be saved." : error);
    return false;
  }
  state->snapshot = RecoveryNasStatus();
  return true;
}

void Close(lv_obj_t *overlay) { lv_obj_delete_async(overlay); }

void SetActivity(NasUi *state, bool active) {
  if (state == nullptr || state->activity == nullptr) return;
  if (active && !state->activity_animating) {
    state->activity_animating = true;
    lv_obj_remove_flag(state->activity, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t spin;
    lv_anim_init(&spin);
    lv_anim_set_var(&spin, state->activity);
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
    lv_anim_delete(state->activity, nullptr);
    lv_obj_set_style_transform_rotation(state->activity, 0, 0);
    lv_obj_add_flag(state->activity, LV_OBJ_FLAG_HIDDEN);
  }
}

std::string BusyTitle(const NasUi *state) {
  const char *base = "Updating storage";
  switch (state->running) {
    case NasOperation::kMountAndUse: base = "Mounting storage"; break;
    case NasOperation::kUse: base = "Selecting storage"; break;
    case NasOperation::kUnmount: base = "Unmounting storage"; break;
  }
  std::string title(base);
  title.append(1 + (state->busy_phase % 3), '.');
  return title;
}

void EditField(NasUi *state, Field field) {
  if (state->busy) return;
  if (state->snapshot.mounted) {
    Sheet(state->screen, "Network storage is mounted",
          "Unmount it before changing its connection settings.");
    return;
  }

  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);

  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, 1312, 1260);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_set_style_pad_all(sheet, 48, 0);
  auto *title = Label(sheet, FieldTitle(field), &lv_font_montserrat_48, kText);
  lv_obj_set_width(title, 1180);
  auto *hint = Label(sheet,
      field == Field::kPassword ? "The value is hidden while you type."
      : field == Field::kPath ? "SFTP may use an absolute path; SMB uses a path inside the share."
      : "Saved securely for AERA network storage.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 0, 76);
  lv_obj_set_width(hint, 1180);

  auto *input = lv_textarea_create(sheet);
  lv_obj_set_pos(input, 0, 150);
  lv_obj_set_size(input, 1216, 126);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_max_length(input, FieldLimit(field));
  lv_textarea_set_password_mode(input, field == Field::kPassword);
  lv_textarea_set_text(input, FieldValue(state->snapshot.config, field).c_str());
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
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard, 0, 306);
  lv_obj_set_size(keyboard, 1216, 630);
  lv_keyboard_set_mode(keyboard, field == Field::kPort
                                     ? LV_KEYBOARD_MODE_NUMBER
                                     : LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(keyboard, input);

  auto *cancel = Button(sheet, "Cancel", [overlay] { Close(overlay); });
  lv_obj_set_pos(cancel, 0, 986);
  lv_obj_set_size(cancel, 580, 116);
  auto *save = Button(sheet, "Save", [state, field, input, overlay] {
    NasConfig config = state->snapshot.config;
    SetField(&config, field, lv_textarea_get_text(input));
    if (!SaveConfig(state, config)) return;
    Close(overlay);
    Populate(state);
  }, true);
  lv_obj_set_pos(save, 636, 986);
  lv_obj_set_size(save, 580, 116);
  lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
  AnimateEnter(sheet, 0, 42);
}

void Dispatch(NasUi *state, NasOperation operation) {
  if (state == nullptr || state->busy) return;
  const auto &config = state->snapshot.config;
  if (operation == NasOperation::kMountAndUse && config.host.empty()) {
    Sheet(state->screen, "Host required",
          "Enter the SFTP or SMB server hostname/IP before mounting.");
    return;
  }
  gRequest.operation = operation;
  state->callback(Action::kRunNasOperation, state->context);
}

void AddField(NasUi *state, int x, int y, Field field) {
  std::string value = FieldValue(state->snapshot.config, field);
  if (field == Field::kPassword)
    value = value.empty() ? "Not set" : "Password saved";
  else if (value.empty())
    value = "Not set";
  auto *card = lv_button_create(state->scene.list);
  Panel(card, 30, kMainSheet);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, 640, 160);
  lv_obj_set_style_transform_scale(card, 256, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
  OnClick(card, [state, field] { EditField(state, field); });
  auto *name = Label(card, FieldTitle(field), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(name, 30, 26);
  auto *copy = Label(card, value.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(copy, 30, 78);
  lv_obj_set_width(copy, 520);
  lv_label_set_long_mode(copy, LV_LABEL_LONG_DOT);
  auto *edit = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_24,
                     kMutedStrong);
  lv_obj_align(edit, LV_ALIGN_RIGHT_MID, -28, 0);
  AnimateEnter(card, 20 + static_cast<uint32_t>(y / 10), 9);
}

void SelectProtocol(NasUi *state, const char *type) {
  if (state->snapshot.mounted) {
    Sheet(state->screen, "Network storage is mounted",
          "Unmount it before switching protocols.");
    return;
  }
  if (state->snapshot.config.type == type) return;
  NasConfig config = state->snapshot.config;
  config.type = type;
  if (config.type == "sftp") config.port = "22";
  if (SaveConfig(state, config)) Populate(state);
}

lv_obj_t *ProtocolButton(NasUi *state, lv_obj_t *parent, int x,
                         const char *type, const char *title) {
  const bool selected = state->snapshot.config.type == type;
  auto *button = lv_button_create(parent);
  Panel(button, 28, selected ? kAccentSoft : kMainPanel);
  Interactive(button, kMainSelected);
  lv_obj_set_pos(button, x, 28);
  lv_obj_set_size(button, 282, 112);
  lv_obj_set_style_border_width(button, selected ? 2 : 1, 0);
  lv_obj_set_style_border_color(button, selected ? kAccent : kMainLine, 0);
  lv_obj_set_style_border_opa(button, selected ? LV_OPA_70 : LV_OPA_30, 0);
  auto *label = Label(button, title, &lv_font_montserrat_32,
                      selected ? kAccent : kText);
  lv_obj_center(label);
  OnClick(button, [state, type] { SelectProtocol(state, type); });
  return button;
}

void AddCacheCard(NasUi *state, int x, int y) {
  const bool enabled = state->snapshot.config.cache_mode == "data";
  auto *card = lv_button_create(state->scene.list);
  Panel(card, 30, enabled ? kAccentSoft : kMainSheet);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, 640, 160);
  lv_obj_set_style_transform_scale(card, 256, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(card, enabled ? 2 : 1, 0);
  lv_obj_set_style_border_color(card, enabled ? kAccent : kMainLine, 0);
  lv_obj_set_style_border_opa(card, enabled ? LV_OPA_50 : LV_OPA_30, 0);
  OnClick(card, [state] {
    if (state->snapshot.mounted) {
      Sheet(state->screen, "Network storage is mounted",
            "Unmount it before changing cache mode.");
      return;
    }
    NasConfig config = state->snapshot.config;
    config.cache_mode = config.cache_mode == "data" ? "off" : "data";
    if (SaveConfig(state, config)) Populate(state);
  });
  auto *name = Label(card, "Write cache", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(name, 30, 26);
  auto *copy = Label(card, enabled ? "Write-back enabled" : "Off",
                     &lv_font_montserrat_32, enabled ? kAccent : kText);
  lv_obj_set_pos(copy, 30, 78);
  auto *end = Label(card, enabled ? "On" : "Off", &lv_font_montserrat_24,
                    enabled ? kAccent : kMutedStrong);
  lv_obj_align(end, LV_ALIGN_RIGHT_MID, -30, 0);
  AnimateEnter(card, 20 + static_cast<uint32_t>(y / 10), 9);
}

void Populate(NasUi *state) {
  if (state == nullptr || state->scene.list == nullptr) return;
  lv_obj_clean(state->scene.list);
  state->snapshot = RecoveryNasStatus();
  if (!state->snapshot.supported) {
    Row(state->scene.list, 0, LV_SYMBOL_WARNING, "Network storage unavailable",
        "SFTP and SMB support are unavailable on this device.", [] {}, "");
    return;
  }
  auto *protocol = lv_obj_create(state->scene.list);
  Panel(protocol, 32, kMainSheet);
  lv_obj_set_pos(protocol, 0, 0);
  lv_obj_set_size(protocol, 1312, 168);
  lv_obj_set_style_border_width(protocol, 1, 0);
  lv_obj_set_style_border_color(protocol, kMainLine, 0);
  lv_obj_set_style_border_opa(protocol, LV_OPA_30, 0);
  auto *protocol_title = Label(protocol, "Connection protocol",
                               &lv_font_montserrat_32, kText);
  lv_obj_set_pos(protocol_title, 32, 34);
  auto *protocol_copy = Label(protocol, "Choose how AERA reaches your server",
                              &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(protocol_copy, 32, 94);
  ProtocolButton(state, protocol, 696, "sftp", "SFTP");
  ProtocolButton(state, protocol, 994, "smb", "SMB");
  AnimateEnter(protocol, 10, 9);

  int y = 190;
  AddField(state, 0, y, Field::kHost);
  AddField(state, 672, y, Field::kUser);
  y += 180;
  AddField(state, 0, y, Field::kPassword);
  if (state->snapshot.config.type == "sftp") {
    AddField(state, 672, y, Field::kPort);
  } else {
    AddField(state, 672, y, Field::kShare);
  }
  y += 180;
  AddField(state, 0, y, Field::kPath);
  if (state->snapshot.config.type == "smb") {
    AddField(state, 672, y, Field::kDomain);
    y += 180;
    AddCacheCard(state, 0, y);
  } else {
    AddCacheCard(state, 672, y);
  }
}

void Refresh(NasUi *state) {
  if (state == nullptr) return;
  state->snapshot = RecoveryNasStatus();
  const auto &status = state->snapshot;
  const std::string busy_title = state->busy ? BusyTitle(state) : std::string();
  const char *headline = !status.supported ? "Unavailable" :
      state->busy ? busy_title.c_str() : status.mounted ?
      (status.selected ? "Mounted and in use" : "Mounted") : "Not mounted";
  lv_label_set_text(state->scene.status, headline);
  std::string detail = status.status;
  if (detail.empty()) detail = status.mounted ? "/mnt/nas" :
      "Configure an SFTP or SMB connection below.";
  lv_label_set_text(state->scene.detail, detail.c_str());
  SetActivity(state, state->busy);
  lv_obj_set_style_border_color(state->hero,
      status.mounted ? kGreen : state->busy ? kAccent : kMainLine, 0);
  lv_obj_set_style_border_opa(state->hero,
      status.mounted || state->busy ? LV_OPA_60 : LV_OPA_30, 0);
  lv_obj_set_style_text_color(state->status_icon,
      status.mounted ? kGreen : kAccent, 0);

  auto *primary_label = lv_obj_get_child(state->scene.primary, 0);
  lv_label_set_text(primary_label,
      status.mounted ? (status.selected ? "NAS in use" : "Use NAS")
                     : "Mount & Use");
  if (!status.supported || state->busy || (status.mounted && status.selected))
    lv_obj_add_state(state->scene.primary, LV_STATE_DISABLED);
  else
    lv_obj_remove_state(state->scene.primary, LV_STATE_DISABLED);
  if (!status.mounted || state->busy)
    lv_obj_add_state(state->scene.secondary, LV_STATE_DISABLED);
  else
    lv_obj_remove_state(state->scene.secondary, LV_STATE_DISABLED);
}

void Timer(lv_timer_t *timer) {
  auto *state = static_cast<NasUi *>(lv_timer_get_user_data(timer));
  if (state->busy) ++state->busy_phase;
  Refresh(state);
}

}  // namespace

NasScene BuildNasScene(lv_obj_t *screen, ActionCallback callback,
                       void *context) {
  auto *state = new NasUi;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  state->snapshot = RecoveryNasStatus();
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    auto *state = static_cast<NasUi *>(lv_event_get_user_data(event));
    if (state->timer != nullptr) lv_timer_delete(state->timer);
    delete state;
  }, LV_EVENT_DELETE, state);

  Header(screen, "Network Storage", "SFTP & SMB connections", callback, context);
  const bool landscape = Landscape(screen);
  auto *panel = lv_obj_create(screen);
  Panel(panel, 36, kMainSheet);
  state->hero = panel;
  lv_obj_set_pos(panel, 64, landscape ? 350 : 420);
  lv_obj_set_size(panel, landscape ? 1450 : 1312,
                  landscape ? 520 : 352);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, kMainLine, 0);
  lv_obj_set_style_border_opa(panel, LV_OPA_30, 0);

  auto *status_plate = lv_obj_create(panel);
  Panel(status_plate, 34, kAccentSoft);
  lv_obj_set_pos(status_plate, 40, 38);
  lv_obj_set_size(status_plate, 124, 124);
  lv_obj_set_style_border_width(status_plate, 1, 0);
  lv_obj_set_style_border_color(status_plate, kAccent, 0);
  lv_obj_set_style_border_opa(status_plate, LV_OPA_40, 0);
  state->status_icon = Label(status_plate, LV_SYMBOL_DRIVE,
                             &lv_font_montserrat_48, kAccent);
  lv_obj_center(state->status_icon);

  state->scene.status = Label(panel, "", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(state->scene.status, 198, 34);
  lv_obj_set_width(state->scene.status, 1000);
  lv_label_set_long_mode(state->scene.status, LV_LABEL_LONG_DOT);
  state->scene.detail = Label(panel, "", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(state->scene.detail, 198, 108);
  lv_obj_set_width(state->scene.detail, 960);
  lv_label_set_long_mode(state->scene.detail, LV_LABEL_LONG_DOT);
  state->activity = Label(panel, LV_SYMBOL_REFRESH,
                          &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(state->activity, 1190, 62);
  lv_obj_add_flag(state->activity, LV_OBJ_FLAG_HIDDEN);
  state->scene.primary = Button(panel, "Mount & Use", [state] {
    Dispatch(state, state->snapshot.mounted ? NasOperation::kUse
                                            : NasOperation::kMountAndUse);
  }, true);
  lv_obj_set_pos(state->scene.primary, 40, 210);
  lv_obj_set_size(state->scene.primary, 760, 104);
  lv_obj_set_style_radius(state->scene.primary, 30, 0);
  state->scene.secondary = Button(panel, "Unmount", [state] {
    Dispatch(state, NasOperation::kUnmount);
  });
  lv_obj_set_pos(state->scene.secondary, 824, 210);
  lv_obj_set_size(state->scene.secondary, 414, 104);
  lv_obj_set_style_radius(state->scene.secondary, 30, 0);
  lv_obj_set_style_opa(state->scene.primary, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_set_style_opa(state->scene.secondary, LV_OPA_40, LV_STATE_DISABLED);

  auto *caption = Label(screen, "CONNECTION  /  SAVED ON DEVICE",
                        &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(caption, 3, 0);
  lv_obj_set_pos(caption, landscape ? 1580 : 80,
                 landscape ? 306 : 826);
  state->scene.list = Scroll(screen, landscape ? 350 : 880,
                             landscape ? 900 : 1964);
  if (landscape) {
    lv_obj_set_x(state->scene.list, 1560);
    lv_obj_set_width(state->scene.list, 1544);
  }
  lv_obj_set_style_pad_bottom(state->scene.list, landscape ? 190 : 242, 0);
  state->scene.state = state;
  Navigation(screen, Action::kSettings, callback, context);
  Populate(state);
  Refresh(state);
  AnimateEnter(panel, 10, 14);
  state->timer = lv_timer_create(Timer, 260, state);
  return state->scene;
}

NasRequest GetNasRequest() { return gRequest; }

void SetNasBusy(const NasScene &scene, const NasRequest &request) {
  auto *state = static_cast<NasUi *>(scene.state);
  if (state == nullptr) return;
  state->busy = true;
  state->running = request.operation;
  state->busy_phase = 0;
  Refresh(state);
}

void CompleteNasOperation(const NasScene &scene, bool success) {
  auto *state = static_cast<NasUi *>(scene.state);
  if (state == nullptr) return;
  state->busy = false;
  Refresh(state);
  Populate(state);
  if (!success) {
    const auto status = RecoveryNasStatus();
    Sheet(state->screen, "Network storage action failed",
          status.error.empty() ?
              "Check Wi-Fi, server settings and the recovery log, then try again."
              : status.error);
  }
}

}  // namespace recovery_ui2
