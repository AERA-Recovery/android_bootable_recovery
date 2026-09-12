/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene.hpp"
#include "phone_keyboard.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <lvgl.h>

#include "design.hpp"
#include "recovery_ui2/engine.hpp"
#include "recovery_ui2/status_bar.hpp"

namespace recovery_ui2 {
namespace {
using namespace design;

constexpr int kMaxPatternDots = 36;

struct DecryptState {
  ActionCallback callback = nullptr;
  void *context = nullptr;
  int credential_type = 0;
  int grid_size = 3;
  lv_obj_t *input = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *submit = nullptr;
  lv_obj_t *submit_label = nullptr;
  lv_obj_t *keyboard = nullptr;
  lv_obj_t *pattern_area = nullptr;
  lv_obj_t *pattern_line = nullptr;
  std::array<lv_obj_t *, kMaxPatternDots> nodes{};
  std::array<bool, kMaxPatternDots> selected{};
  std::array<int, kMaxPatternDots> sequence{};
  std::array<lv_point_precise_t, kMaxPatternDots> points{};
  int sequence_length = 0;
  bool tracking = false;
  bool busy = false;
};

void DeleteState(lv_event_t *event) {
  delete static_cast<DecryptState *>(lv_event_get_user_data(event));
}

void Dispatch(lv_event_t *event) {
  auto *state = static_cast<DecryptState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->callback == nullptr || state->busy) return;
  const auto action = static_cast<Action>(reinterpret_cast<uintptr_t>(
      lv_obj_get_user_data(lv_event_get_target_obj(event))));
  state->callback(action, state->context);
}

void Bind(lv_obj_t *object, Action action, DecryptState *state) {
  lv_obj_set_user_data(
      object, reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
  lv_obj_add_event_cb(object, Dispatch, LV_EVENT_CLICKED, state);
}

void RefreshPattern(DecryptState *state) {
  if (state == nullptr) return;
  for (int i = 0; i < state->grid_size * state->grid_size; ++i) {
    const bool active = state->selected[i];
    lv_obj_set_style_bg_color(state->nodes[i],
                              active ? kAccent : kMainPanel, 0);
    lv_obj_set_style_border_color(state->nodes[i],
                                  active ? kAccent : kLineBright, 0);
    lv_obj_set_style_transform_scale(state->nodes[i], active ? 116 : 100, 0);
  }
  lv_line_set_points(state->pattern_line, state->points.data(),
                     static_cast<uint32_t>(state->sequence_length));
  lv_obj_invalidate(state->pattern_area);

  if (state->sequence_length == 0) {
    lv_label_set_text(state->status, "Draw your unlock pattern");
  } else {
    char copy[64];
    lv_snprintf(copy, sizeof(copy), "%d dots connected", state->sequence_length);
    lv_label_set_text(state->status, copy);
  }
}

void ResetPattern(DecryptState *state) {
  state->selected.fill(false);
  state->sequence_length = 0;
  RefreshPattern(state);
}

void AddPatternDot(DecryptState *state, int index) {
  if (index < 0 || index >= state->grid_size * state->grid_size ||
      state->selected[index] || state->sequence_length >= kMaxPatternDots)
    return;

  if (state->sequence_length > 0) {
    const int previous = state->sequence[state->sequence_length - 1];
    int px = previous % state->grid_size;
    int py = previous / state->grid_size;
    const int nx = index % state->grid_size;
    const int ny = index / state->grid_size;
    int dx = nx > px ? 1 : -1;
    int dy = ny > py ? 1 : -1;
    if (px == nx) dx = 0;
    else if (py == ny) dy = 0;
    else if (std::abs(px - nx) != std::abs(py - ny)) dx = dy = 2;
    if (dx != 2) {
      while ((dx == 0 || px != nx - dx) && (dy == 0 || py != ny - dy)) {
        px += dx;
        py += dy;
        const int intermediate = py * state->grid_size + px;
        if (!state->selected[intermediate] &&
            state->sequence_length < kMaxPatternDots) {
          state->selected[intermediate] = true;
          state->sequence[state->sequence_length] = intermediate;
          state->points[state->sequence_length] = {
              static_cast<lv_value_precise_t>(lv_obj_get_x(state->nodes[intermediate]) + 34),
              static_cast<lv_value_precise_t>(lv_obj_get_y(state->nodes[intermediate]) + 34)};
          ++state->sequence_length;
        }
      }
    }
  }

  state->selected[index] = true;
  state->sequence[state->sequence_length] = index;
  state->points[state->sequence_length] = {
      static_cast<lv_value_precise_t>(lv_obj_get_x(state->nodes[index]) + 34),
      static_cast<lv_value_precise_t>(lv_obj_get_y(state->nodes[index]) + 34)};
  ++state->sequence_length;
  RefreshPattern(state);
}

int PatternDotAt(DecryptState *state, const lv_point_t &point) {
  lv_area_t area{};
  lv_obj_get_coords(state->pattern_area, &area);
  const int local_x = point.x - area.x1;
  const int local_y = point.y - area.y1;
  int closest = -1;
  int closest_distance = 1000000;
  const int threshold = state->grid_size <= 3 ? 112 : 76;
  for (int i = 0; i < state->grid_size * state->grid_size; ++i) {
    const int dx = local_x - (lv_obj_get_x(state->nodes[i]) + 34);
    const int dy = local_y - (lv_obj_get_y(state->nodes[i]) + 34);
    const int distance = dx * dx + dy * dy;
    if (distance < threshold * threshold && distance < closest_distance) {
      closest = i;
      closest_distance = distance;
    }
  }
  return closest;
}

void PatternTouch(lv_event_t *event) {
  auto *state = static_cast<DecryptState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->busy) return;
  lv_indev_t *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED) {
    ResetPattern(state);
    state->tracking = true;
  }
  if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) &&
      state->tracking)
    AddPatternDot(state, PatternDotAt(state, point));
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    state->tracking = false;
    if (state->sequence_length > 0)
      lv_label_set_text(state->status, "Pattern ready - swipe again to redraw");
  }
}

lv_obj_t *MakeButton(lv_obj_t *parent, const char *text, lv_color_t fill,
                     lv_color_t foreground) {
  lv_obj_t *button = lv_button_create(parent);
  NoScroll(button);
  lv_obj_set_style_radius(button, 28, 0);
  lv_obj_set_style_bg_color(button, fill, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_transform_scale(button, 252, LV_STATE_PRESSED);
  lv_obj_t *label = Label(button, text, &lv_font_montserrat_32, foreground);
  lv_obj_center(label);
  return button;
}

void MakePattern(DecryptState *state, lv_obj_t *panel) {
  state->pattern_area = lv_obj_create(panel);
  NoScroll(state->pattern_area);
  lv_obj_set_pos(state->pattern_area, 86, 500);
  lv_obj_set_size(state->pattern_area, 1012, 1012);
  lv_obj_set_style_radius(state->pattern_area, 34, 0);
  lv_obj_set_style_bg_color(state->pattern_area, kInset, 0);
  lv_obj_set_style_bg_opa(state->pattern_area, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(state->pattern_area, 1, 0);
  lv_obj_set_style_border_color(state->pattern_area, kLine, 0);
  lv_obj_set_style_pad_all(state->pattern_area, 0, 0);
  lv_obj_add_flag(state->pattern_area, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(state->pattern_area, PatternTouch, LV_EVENT_PRESSED, state);
  lv_obj_add_event_cb(state->pattern_area, PatternTouch, LV_EVENT_PRESSING, state);
  lv_obj_add_event_cb(state->pattern_area, PatternTouch, LV_EVENT_RELEASED, state);
  lv_obj_add_event_cb(state->pattern_area, PatternTouch, LV_EVENT_PRESS_LOST, state);

  state->pattern_line = lv_line_create(state->pattern_area);
  lv_obj_set_size(state->pattern_line, 1012, 1012);
  lv_obj_set_pos(state->pattern_line, 0, 0);
  lv_obj_set_style_line_color(state->pattern_line, kAccent, 0);
  lv_obj_set_style_line_width(state->pattern_line, 18, 0);
  lv_obj_set_style_line_rounded(state->pattern_line, true, 0);

  const int count = state->grid_size;
  const int margin = count <= 3 ? 128 : 72;
  const int span = 1012 - margin * 2 - 68;
  for (int row = 0; row < count; ++row) {
    for (int column = 0; column < count; ++column) {
      const int index = row * count + column;
      lv_obj_t *node = lv_obj_create(state->pattern_area);
      state->nodes[index] = node;
      NoScroll(node);
      lv_obj_remove_flag(node, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(node, 68, 68);
      lv_obj_set_pos(node, margin + (count == 1 ? 0 : column * span / (count - 1)),
                     margin + (count == 1 ? 0 : row * span / (count - 1)));
      lv_obj_set_style_radius(node, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(node, kMainPanel, 0);
      lv_obj_set_style_bg_opa(node, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(node, 5, 0);
      lv_obj_set_style_border_color(node, kLineBright, 0);
    }
  }
  RefreshPattern(state);
}

void PinKeyPressed(lv_event_t *event) {
  auto *state = static_cast<DecryptState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->busy) return;
  const auto key = reinterpret_cast<uintptr_t>(
      lv_obj_get_user_data(lv_event_get_target_obj(event)));
  if (key == 10) {
    lv_textarea_set_text(state->input, "");
  } else if (key == 11) {
    lv_textarea_set_cursor_pos(state->input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_delete_char(state->input);
  } else {
    lv_textarea_set_cursor_pos(state->input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_char(state->input, static_cast<uint32_t>('0' + key));
  }
}

void MakePinPad(DecryptState *state, lv_obj_t *panel) {
  state->keyboard = lv_obj_create(panel);
  Clear(state->keyboard);
  lv_obj_set_pos(state->keyboard, 86, 720);
  lv_obj_set_size(state->keyboard, 1012, 1010);
  constexpr std::array<const char *, 12> labels{
      "1", "2", "3", "4", "5", "6", "7", "8", "9", "Clear", "0",
      LV_SYMBOL_BACKSPACE};
  constexpr std::array<uintptr_t, 12> keys{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11};
  static const lv_style_prop_t press_properties[] = {
      LV_STYLE_BG_COLOR, LV_STYLE_TRANSFORM_SCALE_X,
      LV_STYLE_TRANSFORM_SCALE_Y, LV_STYLE_PROP_INV};
  static lv_style_transition_dsc_t press_transition;
  lv_style_transition_dsc_init(&press_transition, press_properties,
                               lv_anim_path_ease_out, 110, 0, nullptr);
  lv_obj_set_pos(state->keyboard, 86, 420);
  for (size_t i = 0; i < labels.size(); ++i) {
    const bool utility = i == 9 || i == 11;
    lv_obj_t *key = MakeButton(state->keyboard, labels[i],
                               kMainPanel, kText);
    if (utility) lv_obj_set_style_bg_opa(key, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(key, 50 + static_cast<int32_t>(i % 3) * 344,
                   static_cast<int32_t>(i / 3) * 256);
    lv_obj_set_size(key, 224, 224);
    lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(key, kAccentSoft, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(key, 238, LV_STATE_PRESSED);
    lv_obj_set_style_transition(key, &press_transition, 0);
    lv_obj_set_style_border_width(key, 0, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(key, 0),
                               UiFont(i == 9 ? &lv_font_montserrat_32
                                             : &lv_font_montserrat_48), 0);
    if (!utility)
      lv_obj_set_style_transform_scale(lv_obj_get_child(key, 0), 352, 0);
    lv_obj_set_user_data(key, reinterpret_cast<void *>(keys[i]));
    lv_obj_add_event_cb(key, PinKeyPressed, LV_EVENT_CLICKED, state);
  }
}

void MakeKeyboard(DecryptState *state, lv_obj_t *panel, bool pin) {
  state->input = lv_textarea_create(panel);
  lv_obj_set_pos(state->input, 86, 470);
  lv_obj_set_size(state->input, 1012, 160);
  lv_textarea_set_one_line(state->input, true);
  lv_textarea_set_password_mode(state->input, true);
  lv_textarea_set_password_show_time(state->input, 0);
  lv_textarea_set_text(state->input, "");
  lv_textarea_set_placeholder_text(state->input,
                                   pin ? "Enter PIN" : "Enter password");
  lv_textarea_set_max_length(state->input, 128);
  if (pin) lv_textarea_set_accepted_chars(state->input, "0123456789");
  lv_obj_set_style_text_font(state->input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(state->input, kText, 0);
  lv_obj_set_style_text_color(state->input, kMuted, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(state->input, kMainBottom, 0);
  lv_obj_set_style_border_color(state->input, kLineBright, 0);
  lv_obj_set_style_border_color(state->input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(state->input, 2, 0);
  lv_obj_set_style_radius(state->input, 28, 0);
  lv_obj_set_style_pad_all(state->input, 38, 0);

  if (pin) {
    lv_obj_set_style_text_font(state->input, UiFont(&lv_font_montserrat_48), 0);
    lv_obj_set_pos(state->input, 86, 90);
    lv_obj_set_style_text_align(state->input, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(state->input, 12, 0);
    lv_obj_set_style_bg_opa(state->input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->input, 0, 0);
    lv_obj_set_style_border_width(state->input, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_opa(state->input, LV_OPA_TRANSP, LV_PART_CURSOR);
    MakePinPad(state, panel);
    return;
  }

  state->keyboard = lv_keyboard_create(panel);
  phone_keyboard::Apply(state->keyboard);
  // Keyboard widgets default to BOTTOM_MID. Coordinates below are relative
  // to the panel's top left, just like the credential field and action buttons.
  lv_obj_set_align(state->keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(state->keyboard, 36, 720);
  lv_obj_set_size(state->keyboard, 1112, pin ? 1010 : 820);
  lv_keyboard_set_mode(state->keyboard,
                       pin ? LV_KEYBOARD_MODE_NUMBER
                           : LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(state->keyboard, state->input);
  lv_obj_set_style_bg_color(state->keyboard, kMainBottom, 0);
  lv_obj_set_style_bg_opa(state->keyboard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(state->keyboard, 0, 0);
  lv_obj_set_style_pad_all(state->keyboard, 16, 0);
  lv_obj_set_style_pad_row(state->keyboard, 14, 0);
  lv_obj_set_style_pad_column(state->keyboard, 10, 0);
  lv_obj_set_style_bg_color(state->keyboard, kMainPanel, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(state->keyboard, kAccentSoft,
                            LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(state->keyboard, kText, LV_PART_ITEMS);
  lv_obj_set_style_text_font(state->keyboard, UiFont(&lv_font_montserrat_24),
                             LV_PART_ITEMS);
  lv_obj_set_style_radius(state->keyboard, 20, LV_PART_ITEMS);
  lv_obj_set_style_border_width(state->keyboard, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(state->keyboard, kLine, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(state->keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_pad_all(state->keyboard, 20, LV_PART_ITEMS);
}

}  // namespace

DecryptScene BuildDecryptScene(lv_obj_t *screen, int credential_type,
                               bool file_based, int user_id,
                               int pattern_grid_size,
                               ActionCallback callback, void *context) {
  auto *state = new DecryptState;
  state->callback = callback;
  state->context = context;
  state->credential_type = credential_type;
  state->grid_size = std::clamp(pattern_grid_size, 3, 6);
  lv_obj_add_event_cb(screen, DeleteState, LV_EVENT_DELETE, state);
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);

  (void)file_based;
  (void)user_id;
  const bool pin = credential_type == 3;

  // A small native lock illustration keeps startup lightweight.
  lv_obj_t *emblem = lv_obj_create(screen);
  Panel(emblem, LV_RADIUS_CIRCLE, kAccentSoft);
  lv_obj_set_size(emblem, 208, 208);
  lv_obj_align(emblem, LV_ALIGN_TOP_MID, 0, 310);
  lv_obj_set_style_pad_all(emblem, 0, 0);
  lv_obj_t *shackle = lv_obj_create(emblem);
  Clear(shackle);
  lv_obj_set_size(shackle, 74, 86);
  lv_obj_set_pos(shackle, 67, 44);
  lv_obj_set_style_radius(shackle, 38, 0);
  lv_obj_set_style_border_width(shackle, 9, 0);
  lv_obj_set_style_border_color(shackle, kAccent, 0);
  lv_obj_t *lock_body = lv_obj_create(emblem);
  Panel(lock_body, 16, kAccent);
  lv_obj_set_size(lock_body, 104, 80);
  lv_obj_set_pos(lock_body, 52, 94);
  lv_obj_set_style_pad_all(lock_body, 0, 0);
  lv_obj_t *keyhole = lv_obj_create(lock_body);
  Panel(keyhole, 8, kAccentSoft);
  lv_obj_set_size(keyhole, 12, 28);
  lv_obj_center(keyhole);

  lv_obj_t *title = Label(screen, "Unlock your storage", &lv_font_montserrat_48, kText);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 584);
  lv_obj_t *subtitle = Label(
      screen,
      pin ? "Use your phone's PIN to continue." :
      (credential_type == 2 ? "Draw your phone's unlock pattern." :
                              "Use your phone's password to continue."),
      &lv_font_montserrat_32, kMuted);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 672);

  lv_obj_t *panel = lv_obj_create(screen);
  Clear(panel);
  lv_obj_set_pos(panel, 128, pin ? 780 : 740);
  lv_obj_set_size(panel, 1184, 2260);
  lv_obj_set_style_pad_all(panel, 0, 0);

  state->status = Label(panel,
                        credential_type == 2 ? "Draw your unlock pattern" :
                                               "",
                        &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->status, 86, pin ? 286 : 350);
  lv_obj_set_width(state->status, 1012);
  lv_obj_set_style_text_align(state->status, LV_TEXT_ALIGN_CENTER, 0);

  if (credential_type == 2)
    MakePattern(state, panel);
  else
    MakeKeyboard(state, panel, credential_type == 3);

  state->submit = MakeButton(panel, "Unlock storage   " LV_SYMBOL_RIGHT, kAccent, kCanvas);
  lv_obj_set_pos(state->submit, 86, pin ? 1510 : 1818);
  lv_obj_set_size(state->submit, 1012, 142);
  lv_obj_set_style_radius(state->submit, 71, 0);
  state->submit_label = lv_obj_get_child(state->submit, 0);
  Bind(state->submit, Action::kDecryptSubmit, state);

  lv_obj_t *skip = MakeButton(panel, "Skip for now", kCanvas,
                              kMutedStrong);
  lv_obj_set_style_bg_opa(skip, LV_OPA_TRANSP, 0);
  lv_obj_set_pos(skip, 86, pin ? 1690 : 1990);
  lv_obj_set_size(skip, 1012, 132);
  Bind(skip, Action::kDecryptSkip, state);

  lv_obj_t *hint = Label(
      screen,
      "Skipping keeps internal storage locked.",
      &lv_font_montserrat_24, kDim);
  lv_obj_set_width(hint, 1184);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, pin ? -340 : -70);

  AnimateEnter(emblem, 0, 16);
  AnimateEnter(title, 30, 20);
  AnimateEnter(panel, 70, 28);
  return {state->status, state->input, state->submit, state};
}

std::string GetDecryptCredential(const DecryptScene &scene) {
  auto *state = static_cast<DecryptState *>(scene.state);
  if (state == nullptr) return {};
  if (state->credential_type != 2)
    return state->input == nullptr ? std::string()
                                   : lv_textarea_get_text(state->input);
  std::string passphrase;
  for (int i = 0; i < state->sequence_length; ++i)
    passphrase += std::to_string(state->sequence[i] + 1);
  return passphrase;
}

void SetDecryptBusy(const DecryptScene &scene) {
  auto *state = static_cast<DecryptState *>(scene.state);
  if (state == nullptr) return;
  state->busy = true;
  lv_label_set_text(state->status, "Unlocking and preparing /data...");
  lv_label_set_text(state->submit_label, "Unlocking...");
  lv_obj_add_state(state->submit, LV_STATE_DISABLED);
  if (state->keyboard != nullptr) lv_obj_add_state(state->keyboard, LV_STATE_DISABLED);
}

void CompleteDecryptAttempt(const DecryptScene &scene, bool success) {
  auto *state = static_cast<DecryptState *>(scene.state);
  if (state == nullptr) return;
  if (success) {
    lv_label_set_text(state->status, "Data unlocked successfully");
    lv_obj_set_style_text_color(state->status, kGreen, 0);
    return;
  }
  state->busy = false;
  lv_label_set_text(state->status,
                    "That credential did not unlock data. Please try again.");
  lv_obj_set_style_text_color(state->status, kRed, 0);
  lv_label_set_text(state->submit_label, "Try again");
  lv_obj_remove_state(state->submit, LV_STATE_DISABLED);
  if (state->keyboard != nullptr) lv_obj_remove_state(state->keyboard, LV_STATE_DISABLED);
  if (state->input != nullptr) {
    lv_textarea_set_text(state->input, "");
    lv_obj_send_event(state->input, LV_EVENT_FOCUSED, nullptr);
  } else {
    ResetPattern(state);
  }
}

void BuildPreparingScene(lv_obj_t *screen, bool decrypted) {
  MainBackground(screen);
  AttachStatusBar(screen, nullptr, nullptr, StatusBarAction::kNone, true);
  lv_obj_t *icon = IconPlate(screen, decrypted ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                             decrypted ? kGreen : kAmber,
                             decrypted ? kGreenSoft : kAmberSoft, 156);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -250);
  lv_obj_t *title = Label(screen,
                          decrypted ? "Data unlocked" : "Continuing encrypted",
                          &lv_font_montserrat_48, kText);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);
  lv_obj_t *copy = Label(
      screen,
      decrypted ? "Finishing recovery startup and mounting your storage..."
                : "Finishing recovery startup with internal storage locked...",
      &lv_font_montserrat_24, kMuted);
  lv_obj_align(copy, LV_ALIGN_CENTER, 0, 20);
  lv_obj_t *bar = lv_bar_create(screen);
  lv_obj_set_size(bar, 680, 18);
  lv_obj_align(bar, LV_ALIGN_CENTER, 0, 150);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 100, LV_ANIM_ON);
  lv_obj_set_style_bg_color(bar, kPanelStrong, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, decrypted ? kGreen : kAmber,
                            LV_PART_INDICATOR);
  AnimateEnter(icon, 0, 24);
  AnimateEnter(title, 60, 20);
}

}  // namespace recovery_ui2
