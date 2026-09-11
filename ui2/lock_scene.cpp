/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "design.hpp"
#include "recovery_ui2/status_bar.hpp"

namespace recovery_ui2 {
namespace {
using namespace design;

constexpr int32_t kSliderWidth = 1000;
constexpr int32_t kSliderHeight = 156;
constexpr int32_t kKnobSize = kSliderHeight;
constexpr int32_t kSliderInset = 0;
constexpr int32_t kSliderTravel =
    kSliderWidth - kKnobSize - (kSliderInset * 2);

#include "aera_lock_logo.inc"
#include "aera_lock_inner_dark.inc"
#include "aera_lock_inner_light.inc"

static const lv_image_dsc_t kLockLogoImage = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = 900,
               .h = 684,
               .stride = 900},
    .data_size = sizeof(_tmp_aera_lock_logo_raw),
    .data = _tmp_aera_lock_logo_raw,
};

static const lv_image_dsc_t kLockLogoInnerDarkImage = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = 900,
               .h = 684,
               .stride = 900},
    .data_size = sizeof(_tmp_aera_lock_inner_dark_raw),
    .data = _tmp_aera_lock_inner_dark_raw,
};

static const lv_image_dsc_t kLockLogoInnerLightImage = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = 900,
               .h = 684,
               .stride = 900},
    .data_size = sizeof(_tmp_aera_lock_inner_light_raw),
    .data = _tmp_aera_lock_inner_light_raw,
};

struct LockState {
  lv_obj_t *content = nullptr;
  lv_obj_t *clock = nullptr;
  lv_obj_t *date = nullptr;
  lv_obj_t *slider = nullptr;
  lv_obj_t *slider_fill = nullptr;
  lv_obj_t *slider_knob = nullptr;
  lv_obj_t *slider_copy = nullptr;
  lv_timer_t *timer = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  int32_t touch_origin_x = 0;
  int32_t slider_offset = 0;
  bool dragging = false;
  bool unlocking = false;
  bool landscape = false;
};

void RefreshClock(LockState *state) {
  if (state == nullptr) return;
  const time_t now = time(nullptr);
  struct tm local {};
  char clock[16] = "--:--";
  char date[64] = "AERA Recovery Project";
  if (localtime_r(&now, &local) != nullptr) {
    int hour = local.tm_hour % 12;
    if (hour == 0) hour = 12;
    snprintf(clock, sizeof(clock), "%d:%02d", hour, local.tm_min);
    strftime(date, sizeof(date), "%A, %e %B", &local);
  }
  lv_label_set_text(state->clock, clock);
  lv_label_set_text(state->date, date);
  // Text transforms default to a top-left pivot in LVGL. Refresh the pivot
  // after the glyph width changes so every time remains optically centered.
  lv_obj_update_layout(state->clock);
  lv_obj_set_style_transform_pivot_x(state->clock,
                                      lv_obj_get_width(state->clock) / 2, 0);
  lv_obj_set_style_transform_pivot_y(state->clock,
                                      lv_obj_get_height(state->clock) / 2, 0);
  lv_obj_align(state->clock, LV_ALIGN_TOP_MID,
               state->landscape ? 700 : 0, 250);
}

void SetSliderOffset(void *object, int32_t value) {
  auto *state = static_cast<LockState *>(object);
  if (state == nullptr || state->slider_knob == nullptr) return;
  state->slider_offset = std::clamp(value, 0, kSliderTravel);
  lv_obj_set_x(state->slider_knob, kSliderInset + state->slider_offset);
  if (state->slider_offset == 0) {
    lv_obj_add_flag(state->slider_fill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(state->slider_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(state->slider_fill,
                     kKnobSize / 2 + state->slider_offset);
  }
  const int32_t copy_opacity =
      std::clamp(255 - state->slider_offset * 230 / kSliderTravel, 20, 255);
  lv_obj_set_style_text_opa(state->slider_copy,
                            static_cast<lv_opa_t>(copy_opacity), 0);
}

void FinishUnlock(lv_anim_t *animation) {
  auto *state = static_cast<LockState *>(lv_anim_get_user_data(animation));
  if (state != nullptr && state->callback != nullptr)
    state->callback(Action::kUnlock, state->context);
}

void SliderTouch(lv_event_t *event) {
  auto *state = static_cast<LockState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->unlocking) return;
  lv_indev_t *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);

  switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
      lv_anim_delete(state, SetSliderOffset);
      state->touch_origin_x = point.x - state->slider_offset;
      state->dragging = true;
      break;
    case LV_EVENT_PRESSING:
      if (state->dragging)
        SetSliderOffset(state, point.x - state->touch_origin_x);
      break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
      if (!state->dragging) break;
      state->dragging = false;
      const bool accepted = state->slider_offset >= kSliderTravel * 72 / 100;
      lv_anim_t settle;
      lv_anim_init(&settle);
      lv_anim_set_var(&settle, state);
      lv_anim_set_values(&settle, state->slider_offset,
                         accepted ? kSliderTravel : 0);
      lv_anim_set_duration(&settle, accepted ? 150 : 300);
      lv_anim_set_path_cb(&settle, accepted ? lv_anim_path_ease_in
                                            : lv_anim_path_ease_out);
      lv_anim_set_exec_cb(&settle, SetSliderOffset);
      if (accepted) {
        state->unlocking = true;
        lv_anim_set_user_data(&settle, state);
        lv_anim_set_completed_cb(&settle, FinishUnlock);
      }
      lv_anim_start(&settle);
      break;
    }
    default:
      break;
  }
}

lv_obj_t *LogoLayer(lv_obj_t *parent, const lv_image_dsc_t *source,
                    int32_t x, int32_t y,
                    lv_color_t color, lv_opa_t opacity) {
  auto *image = lv_image_create(parent);
  lv_image_set_src(image, source);
  lv_image_set_antialias(image, true);
  lv_obj_set_style_image_recolor(image, color, 0);
  lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
  lv_obj_set_style_image_opa(image, opacity, 0);
  lv_obj_align(image, LV_ALIGN_TOP_MID, x, y);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
  return image;
}

void DeleteState(lv_event_t *event) {
  auto *state = static_cast<LockState *>(lv_event_get_user_data(event));
  if (state == nullptr) return;
  lv_anim_delete(state, SetSliderOffset);
  if (state->timer != nullptr) lv_timer_delete(state->timer);
  delete state;
}
}  // namespace

lv_obj_t *BuildLockScene(lv_obj_t *parent, ActionCallback callback,
                         void *context) {
  auto *state = new LockState;
  state->callback = callback;
  state->context = context;
  const int screen_width = lv_obj_get_width(parent);
  const int screen_height = lv_obj_get_height(parent);
  state->landscape = screen_width > screen_height;

  auto *root = lv_obj_create(parent);
  MainBackground(root);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_set_size(root, screen_width, screen_height);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(root, DeleteState, LV_EVENT_DELETE, state);

  AttachStatusBar(root, callback, context, StatusBarAction::kNone, true);

  auto *content = lv_obj_create(root);
  state->content = content;
  Clear(content);
  const int status_height = StatusBarHeight();
  lv_obj_set_pos(content, 0, status_height);
  lv_obj_set_size(content, screen_width, screen_height - status_height);
  lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);

  // A true deboss: the fill is darker than the wallpaper, the top-left inner
  // wall is shadowed, and only the lower-right inner wall catches light.
  // Every edge mask is clipped inside the A, so there are no offset ghosts.
  const int logo_x = state->landscape ? -700 : 0;
  const int logo_y = state->landscape ? 250 : 852;
  auto *deep_shadow = LogoLayer(content, &kLockLogoImage, logo_x, logo_y,
                                Color(0x191b1e), LV_OPA_40);
  auto *soft_shadow = LogoLayer(content, &kLockLogoInnerDarkImage, logo_x, logo_y,
                                lv_color_black(), LV_OPA_40);
  auto *accent_edge = LogoLayer(content, &kLockLogoInnerLightImage, logo_x, logo_y,
                                kMainLine, LV_OPA_20);
  auto *mark = LogoLayer(content, &kLockLogoImage, logo_x, logo_y,
                         kMainCanvas, LV_OPA_10);
  lv_obj_set_style_opa(deep_shadow, LV_OPA_0, 0);
  lv_obj_set_style_opa(soft_shadow, LV_OPA_0, 0);
  lv_obj_set_style_opa(accent_edge, LV_OPA_0, 0);
  lv_obj_set_style_opa(mark, LV_OPA_0, 0);
  lv_obj_fade_in(deep_shadow, 620, 90);
  lv_obj_fade_in(soft_shadow, 620, 120);
  lv_obj_fade_in(accent_edge, 680, 160);
  lv_obj_fade_in(mark, 680, 190);

  state->clock = Label(content, "--:--", &lv_font_montserrat_48, kText);
  lv_obj_set_style_transform_scale(state->clock, 800, 0);
  lv_obj_set_style_text_outline_stroke_color(state->clock, kText, 0);
  lv_obj_set_style_text_outline_stroke_width(state->clock, 1, 0);
  lv_obj_set_style_text_outline_stroke_opa(state->clock, LV_OPA_80, 0);
  lv_obj_set_style_text_letter_space(state->clock, 3, 0);
  lv_obj_align(state->clock, LV_ALIGN_TOP_MID,
               state->landscape ? 700 : 0, 250);

  state->date = Label(content, "", &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_style_text_letter_space(state->date, 1, 0);
  lv_obj_align(state->date, LV_ALIGN_TOP_MID,
               state->landscape ? 700 : 0, 535);

  auto *slider = lv_obj_create(content);
  state->slider = slider;
  Clear(slider);
  lv_obj_set_size(slider, kSliderWidth, kSliderHeight);
  lv_obj_align(slider, LV_ALIGN_BOTTOM_MID,
               state->landscape ? 700 : 0,
               state->landscape ? -180 : -245);
  lv_obj_set_style_radius(slider, kSliderHeight / 2, 0);
  lv_obj_set_style_bg_color(slider, kMainPanel, 0);
  lv_obj_set_style_bg_opa(slider, LV_OPA_80, 0);
  lv_obj_set_style_border_width(slider, 1, 0);
  lv_obj_set_style_border_color(slider, kMainLine, 0);
  lv_obj_set_style_border_opa(slider, LV_OPA_50, 0);
  lv_obj_set_style_clip_corner(slider, true, 0);
  lv_obj_add_flag(slider, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(slider, SliderTouch, LV_EVENT_PRESSED, state);
  lv_obj_add_event_cb(slider, SliderTouch, LV_EVENT_PRESSING, state);
  lv_obj_add_event_cb(slider, SliderTouch, LV_EVENT_RELEASED, state);
  lv_obj_add_event_cb(slider, SliderTouch, LV_EVENT_PRESS_LOST, state);

  auto *fill = lv_obj_create(slider);
  state->slider_fill = fill;
  Clear(fill);
  lv_obj_set_pos(fill, 0, 0);
  lv_obj_set_size(fill, 1, kSliderHeight);
  lv_obj_set_style_radius(fill, kSliderHeight / 2, 0);
  lv_obj_set_style_bg_color(fill, kAccentSoft, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
  lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

  state->slider_copy =
      Label(slider, "Slide to unlock", &lv_font_montserrat_32, kMutedStrong);
  lv_obj_align(state->slider_copy, LV_ALIGN_CENTER, 38, 0);
  lv_obj_remove_flag(state->slider_copy, LV_OBJ_FLAG_CLICKABLE);

  auto *knob = lv_obj_create(slider);
  state->slider_knob = knob;
  Clear(knob);
  lv_obj_set_size(knob, kKnobSize, kKnobSize);
  lv_obj_set_pos(knob, kSliderInset, kSliderInset);
  lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(knob, kAccent, 0);
  lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(knob, 1, 0);
  lv_obj_set_style_border_color(knob, lv_color_white(), 0);
  lv_obj_set_style_border_opa(knob, LV_OPA_20, 0);
  lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
  auto *chevron = Label(knob, LV_SYMBOL_RIGHT, &lv_font_montserrat_48,
                        Color(0x071116));
  lv_obj_center(chevron);
  lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);

  RefreshClock(state);
  state->timer = lv_timer_create([](lv_timer_t *timer) {
    RefreshClock(static_cast<LockState *>(lv_timer_get_user_data(timer)));
  }, 1000, state);

  lv_obj_set_style_opa(content, LV_OPA_0, 0);
  lv_obj_fade_in(content, 360, 60);
  lv_obj_move_foreground(root);
  return root;
}
}  // namespace recovery_ui2
