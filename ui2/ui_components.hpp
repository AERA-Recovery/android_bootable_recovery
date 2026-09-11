/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include "design.hpp"
#include "scene.hpp"
#include "recovery_ui2/status_bar.hpp"

namespace recovery_ui2::widgets {
using namespace design;
using Handler = std::function<void()>;
inline char kModalMarker;
inline int kPreviousNavigationIndex = -1;

inline bool Landscape(lv_obj_t *object) {
  lv_obj_t *screen = object == nullptr ? nullptr : lv_obj_get_screen(object);
  return screen != nullptr && lv_obj_get_width(screen) > lv_obj_get_height(screen);
}

inline bool DismissModal(lv_obj_t *screen) {
  for (int i = static_cast<int>(lv_obj_get_child_count(screen)) - 1; i >= 0; --i) {
    auto *child = lv_obj_get_child(screen, i);
    if (lv_obj_get_user_data(child) == &kModalMarker &&
        !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_delete_async(child);
      return true;
    }
  }
  return false;
}

inline void OnClick(lv_obj_t *object, Handler callback) {
  auto *handler = new Handler(std::move(callback));
  lv_obj_add_event_cb(object, [](lv_event_t *event) {
    auto *fn = static_cast<Handler *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) delete fn;
    else if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
      RecoveryVibrate(Haptic::kTouch);
      (*fn)();
    }
  }, LV_EVENT_ALL, handler);
}

inline lv_obj_t *Button(lv_obj_t *parent, const char *text, Handler action,
                        bool primary = false) {
  auto *button = lv_button_create(parent);
  Clear(button);
  lv_obj_set_style_bg_color(button, primary ? kAccent : kMainPanel, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, primary ? kAccentPressed : kMainSelected,
                            LV_STATE_PRESSED);
  lv_obj_set_style_radius(button, 64, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_transform_scale(button, 250, LV_STATE_PRESSED);
  auto *label = Label(button, text, &lv_font_montserrat_32,
                      primary ? kOnAccent : kText);
  lv_obj_center(label);
  OnClick(button, std::move(action));
  return button;
}

inline void Header(lv_obj_t *screen, const char *title, const char *subtitle,
                    ActionCallback callback, void *context) {
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  auto *heading = Label(screen, title, &lv_font_montserrat_48, kText);
  const bool landscape = Landscape(screen);
  lv_obj_set_pos(heading, 80, landscape ? 210 : 252);
  auto *copy = Label(screen, subtitle, &lv_font_montserrat_32, kMuted);
  lv_obj_set_pos(copy, landscape ? 600 : 80, landscape ? 220 : 338);
  lv_obj_set_width(copy, landscape ? lv_obj_get_width(screen) - 680 : 1280);
  AnimateEnter(heading, 0, 12);
}

inline lv_obj_t *Navigation(lv_obj_t *screen, Action active,
                            ActionCallback callback, void *context,
                            bool app_surface = false) {
  struct Tab { const char *icon; const char *text; Action action; };
  constexpr std::array<Tab, 4> tabs{{
    {LV_SYMBOL_HOME, "Home", Action::kBackHome},
    {LV_SYMBOL_SAVE, "Backup", Action::kBackup},
    {LV_SYMBOL_TRASH, "Wipe", Action::kWipe},
    {LV_SYMBOL_LIST, "Menu", Action::kSettings}}};
  // Style 27, refined: a single floating glass dock. The page background runs
  // all the way to the panel edge; only the selected icon and label use accent.
  const bool landscape = Landscape(screen);
  const DockLayout layout = RecoveryDockLayout();
  const bool compact = layout == DockLayout::kCompact;
  const bool minimal = layout == DockLayout::kMinimal;
  const int bar_width = landscape ? (minimal ? 1360 : compact ? 1600 : 1800)
                                  : (minimal ? 1120 : compact ? 1280 : 1440);
  const int bar_height = landscape ? (minimal ? 138 : 160)
                                   : (minimal ? 170 : compact ? 188 : 226);
  const int shelf_x = landscape || compact || minimal ? 0 : 120;
  const int shelf_y = minimal ? 4 : landscape ? 8 : 18;
  const int shelf_width = landscape || compact || minimal ? bar_width : 1200;
  const int shelf_height = minimal ? bar_height - 8 :
      landscape ? 140 : compact ? 156 : 180;
  auto *bar = lv_obj_create(screen);
  Clear(bar);
  lv_obj_set_size(bar, bar_width, bar_height);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  if (app_surface && RecoveryDockHideInApps()) {
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    return bar;
  }

  // One translucent surface, rather than several nested pills. LVGL has no
  // cheap backdrop blur here, so a restrained tint, border and shadow give the
  // dock depth without hiding the textured page beneath it.
  auto *shelf = lv_obj_create(bar);
  Clear(shelf);
  lv_obj_remove_flag(shelf, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(shelf, shelf_x, shelf_y);
  lv_obj_set_size(shelf, shelf_width, shelf_height);
  lv_obj_set_style_radius(shelf, shelf_height / 2, 0);
  lv_obj_set_style_bg_color(shelf,
      IsLightMode() ? Color(0xffffff) : Color(0x666b73), 0);
  lv_obj_set_style_bg_grad_color(shelf,
      IsLightMode() ? Color(0xd9dde2) : Color(0x292d33), 0);
  lv_obj_set_style_bg_grad_dir(shelf, LV_GRAD_DIR_VER, 0);
  const int transparency = std::clamp(RecoveryDockTransparency(), 0, 100);
  const lv_opa_t surface_opa = static_cast<lv_opa_t>(
      (100 - transparency) * LV_OPA_COVER / 100);
  lv_obj_set_style_bg_opa(shelf, minimal ? LV_OPA_TRANSP : surface_opa, 0);
  lv_obj_set_style_border_width(shelf, minimal ? 0 : 1, 0);
  lv_obj_set_style_border_color(shelf,
      IsLightMode() ? Color(0x8d949c) : Color(0xb8bec6), 0);
  lv_obj_set_style_border_opa(shelf, LV_OPA_20, 0);
  lv_obj_set_style_shadow_color(shelf, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(shelf, 12, 0);
  lv_obj_set_style_shadow_offset_y(shelf, 6, 0);
  lv_obj_set_style_shadow_opa(shelf, LV_OPA_10, 0);
  const int blur = std::clamp(RecoveryDockBlur(), 0, 100);
  lv_obj_set_style_blur_backdrop(shelf, blur > 0 && !minimal, 0);
  lv_obj_set_style_blur_radius(shelf, blur * 18 / 100, 0);
  lv_obj_set_style_blur_quality(shelf, LV_BLUR_QUALITY_SPEED, 0);

  const int slot_width = shelf_width / static_cast<int>(tabs.size());

  for (size_t i = 0; i < tabs.size(); ++i) {
    const auto tab = tabs[i];
    const bool selected = active == tab.action;
    auto *button = lv_button_create(bar);
    Clear(button);
    lv_obj_set_pos(button, shelf_x + static_cast<int>(i) * slot_width,
                   shelf_y);
    lv_obj_set_size(button, slot_width, shelf_height);
    lv_obj_set_style_radius(button, minimal ? 34 : shelf_height / 2, 0);
    if (minimal && selected) {
      lv_obj_set_style_bg_color(button, kAccentSoft, 0);
      lv_obj_set_style_bg_opa(button, LV_OPA_50, 0);
    }
    lv_obj_set_style_bg_color(button, Color(0xffffff), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(button, 250, LV_STATE_PRESSED);
    OnClick(button, [=] { callback(tab.action, context); });

    auto *icon = Label(button, tab.icon, &lv_font_montserrat_48,
                        selected ? kAccent : kMutedStrong);
    if (compact) lv_obj_align(icon, LV_ALIGN_LEFT_MID, 38, 0);
    else lv_obj_align(icon, LV_ALIGN_TOP_MID, 0,
                      minimal ? 14 : landscape ? 18 : 28);
    auto *text = Label(button, tab.text, &lv_font_montserrat_32,
                        selected ? kAccent : kMutedStrong);
    if (compact) lv_obj_align(text, LV_ALIGN_LEFT_MID, 112, 0);
    else lv_obj_align(text, LV_ALIGN_BOTTOM_MID, 0,
                      minimal ? -10 : landscape ? -12 : -20);
    if (selected) {
      lv_obj_set_style_transform_scale(icon, 192, 0);
      lv_obj_set_style_translate_y(icon, 8, 0);
      lv_anim_t icon_pop;
      lv_anim_init(&icon_pop);
      lv_anim_set_var(&icon_pop, icon);
      lv_anim_set_values(&icon_pop, 192, 256);
      lv_anim_set_duration(&icon_pop, 380);
      lv_anim_set_delay(&icon_pop, 55);
      lv_anim_set_path_cb(&icon_pop, lv_anim_path_overshoot);
      lv_anim_set_exec_cb(&icon_pop, [](void *target, int32_t value) {
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t *>(target), value, 0);
      });
      lv_anim_start(&icon_pop);
      lv_anim_t rise;
      lv_anim_init(&rise);
      lv_anim_set_var(&rise, icon);
      lv_anim_set_values(&rise, 8, 0);
      lv_anim_set_duration(&rise, 300);
      lv_anim_set_delay(&rise, 35);
      lv_anim_set_path_cb(&rise, lv_anim_path_ease_out);
      lv_anim_set_exec_cb(&rise, [](void *target, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(target), value, 0);
      });
      lv_anim_start(&rise);
    }
    AnimateEnter(button, 45 + static_cast<uint32_t>(i) * 22, 7);
  }
  return bar;
}

inline lv_obj_t *Scroll(lv_obj_t *parent, int y, int height) {
  auto *list = lv_obj_create(parent);
  Clear(list);
  const bool landscape = Landscape(parent);
  lv_obj_set_pos(list, 64, y);
  lv_obj_set_size(list, landscape ? lv_obj_get_width(lv_obj_get_screen(parent)) - 128
                                  : 1312,
                  height);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 5, LV_PART_SCROLLBAR);
  return list;
}

inline lv_obj_t *Row(lv_obj_t *parent, int y, const char *symbol,
                     const std::string &title, const std::string &detail,
                     Handler action, const char *trailing = LV_SYMBOL_RIGHT) {
  auto *row = Button(parent, "", std::move(action));
  lv_obj_set_pos(row, 0, y);
  // Newly-created scroll containers have not necessarily completed an LVGL
  // layout pass yet. Reading their width too early returns zero and collapses
  // preference labels into a one-character-wide column.
  lv_obj_update_layout(parent);
  const int width = std::max(320, static_cast<int>(lv_obj_get_width(parent)));
  lv_obj_set_size(row, width, 174);
  // Rows retain color feedback, without a scaled temporary layer on touch.
  lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(row, 24, 0);
  auto *icon = Label(row, symbol, &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(icon, 32, 63);
  auto *name = Label(row, title.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 116, 34);
  lv_obj_set_width(name, width - 272);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  auto *copy = Label(row, detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 116, 96);
  lv_obj_set_width(copy, width - 272);
  lv_label_set_long_mode(copy, LV_LABEL_LONG_DOT);
  auto *end = Label(row, trailing, &lv_font_montserrat_32, kMuted);
  lv_obj_align(end, LV_ALIGN_RIGHT_MID, -28, 0);
  return row;
}

inline std::string Size(uint64_t bytes) {
  char result[64];
  if (bytes >= 1073741824ULL)
    snprintf(result, sizeof(result), "%.1f GB", bytes / 1073741824.0);
  else if (bytes >= 1048576ULL)
    snprintf(result, sizeof(result), "%.1f MB", bytes / 1048576.0);
  else if (bytes >= 1024)
    snprintf(result, sizeof(result), "%.1f KB", bytes / 1024.0);
  else snprintf(result, sizeof(result), "%llu bytes", (unsigned long long)bytes);
  return result;
}

inline std::string ReadLog() {
  FILE *file = fopen("/tmp/recovery.log", "re");
  if (!file) return "Recovery log is not available yet.";
  fseek(file, 0, SEEK_END);
  const long length = ftell(file);
  const long start = std::max(0L, length - 10000);
  fseek(file, start, SEEK_SET);
  char buffer[10001];
  const size_t count = fread(buffer, 1, 10000, file);
  fclose(file);
  std::string result(buffer, count);
  if (start > 0) {
    const auto line = result.find('\n');
    if (line != std::string::npos) result.erase(0, line + 1);
  }
  return result.empty() ? "No output yet." : result;
}

constexpr int32_t kConfirmTrackWidth = 1140;
constexpr int32_t kConfirmTrackHeight = 128;
constexpr int32_t kConfirmKnobSize = 104;
constexpr int32_t kConfirmKnobInset = 12;
constexpr int32_t kConfirmFillInset = 6;
constexpr int32_t kConfirmTravel =
    kConfirmTrackWidth - kConfirmKnobSize - 2 * kConfirmKnobInset;

struct SheetState {
  lv_obj_t *overlay = nullptr;
  Handler confirm;
  lv_obj_t *fill = nullptr;
  lv_obj_t *knob = nullptr;
  lv_obj_t *slider_copy = nullptr;
  int32_t touch_origin_x = 0;
  int32_t slider_offset = 0;
  bool dragging = false;
  bool confirming = false;
};

inline void SetConfirmOffset(void *object, int32_t value) {
  auto *state = static_cast<SheetState *>(object);
  if (state == nullptr || state->knob == nullptr) return;
  state->slider_offset = std::clamp(value, 0, kConfirmTravel);
  lv_obj_set_x(state->knob, kConfirmKnobInset + state->slider_offset);
  if (state->slider_offset == 0) {
    lv_obj_add_flag(state->fill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(state->fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(state->fill,
                     kConfirmKnobInset + kConfirmKnobSize / 2 +
                         state->slider_offset - kConfirmFillInset);
  }
  const int32_t opacity =
      std::clamp(255 - state->slider_offset * 220 / kConfirmTravel, 25, 255);
  lv_obj_set_style_text_opa(state->slider_copy,
                            static_cast<lv_opa_t>(opacity), 0);
}

inline void FinishConfirmation(lv_anim_t *animation) {
  auto *state = static_cast<SheetState *>(lv_anim_get_user_data(animation));
  if (state == nullptr || !state->confirm) return;
  lv_obj_add_flag(state->overlay, LV_OBJ_FLAG_HIDDEN);
  state->confirm();
}

inline void ConfirmSliderTouch(lv_event_t *event) {
  auto *state = static_cast<SheetState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->confirming) return;
  auto *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);

  switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED: {
      lv_area_t bounds{};
      lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
      state->dragging = point.x <= bounds.x1 + kConfirmKnobSize + 48;
      state->touch_origin_x = point.x - state->slider_offset;
      lv_anim_delete(state, SetConfirmOffset);
      break;
    }
    case LV_EVENT_PRESSING:
      if (state->dragging)
        SetConfirmOffset(state, point.x - state->touch_origin_x);
      break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
      if (!state->dragging) break;
      state->dragging = false;
      const bool accepted = state->slider_offset >= kConfirmTravel * 82 / 100;
      lv_anim_t settle;
      lv_anim_init(&settle);
      lv_anim_set_var(&settle, state);
      lv_anim_set_values(&settle, state->slider_offset,
                         accepted ? kConfirmTravel : 0);
      lv_anim_set_duration(&settle, accepted ? 140 : 280);
      lv_anim_set_path_cb(&settle, lv_anim_path_ease_out);
      lv_anim_set_exec_cb(&settle, SetConfirmOffset);
      if (accepted) {
        state->confirming = true;
        lv_anim_set_user_data(&settle, state);
        lv_anim_set_completed_cb(&settle, FinishConfirmation);
      }
      lv_anim_start(&settle);
      break;
    }
    default:
      break;
  }
}

// A restrained review sheet using the same reliable custom drag model as the
// lock screen. Confirmation is impossible unless the gesture starts at the
// handle and crosses most of the track.
inline void Sheet(lv_obj_t *screen, const std::string &title,
                   const std::string &copy, Handler confirm = {}) {
  auto *overlay = lv_obj_create(screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
  auto *state = new SheetState;
  state->overlay = overlay;
  state->confirm = std::move(confirm);
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    auto *state = static_cast<SheetState *>(lv_event_get_user_data(event));
    lv_anim_delete(state, SetConfirmOffset);
    delete state;
  }, LV_EVENT_DELETE, state);

  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_style_bg_grad_dir(sheet, LV_GRAD_DIR_NONE, 0);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_20, 0);
  const int sheet_width = Landscape(screen)
      ? std::min(2200, static_cast<int>(lv_obj_get_width(screen)) - 128)
      : 1312;
  const int sheet_height = Landscape(screen)
      ? std::min(state->confirm ? 1180 : 1000,
                 static_cast<int>(lv_obj_get_height(screen)) - 80)
      : (state->confirm ? 1180 : 1000);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_set_style_pad_all(sheet, 56, 0);

  auto *grabber = lv_obj_create(sheet);
  Clear(grabber);
  lv_obj_set_size(grabber, 112, 8);
  lv_obj_align(grabber, LV_ALIGN_TOP_MID, 0, -27);
  lv_obj_set_style_radius(grabber, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(grabber, kMutedStrong, 0);
  lv_obj_set_style_bg_opa(grabber, LV_OPA_30, 0);

  auto *heading = Label(sheet, title.c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 0, 34);
  lv_obj_set_width(heading, sheet_width - 112);
  lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);

  auto *area = lv_obj_create(sheet);
  Clear(area);
  lv_obj_set_pos(area, 0, 132);
  lv_obj_set_size(area, sheet_width - 112,
                  state->confirm ? sheet_height - 670 : sheet_height - 310);
  lv_obj_add_flag(area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(area, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(area, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(area, 5, LV_PART_SCROLLBAR);
  auto *body = Label(area, copy.c_str(), &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(body, sheet_width - 152);
  lv_obj_set_style_text_line_space(body, 16, 0);

  if (state->confirm) {
    auto *divider = lv_obj_create(sheet);
    Clear(divider);
    lv_obj_set_size(divider, sheet_width - 112, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, -360);
    lv_obj_set_style_bg_color(divider, kMainLine, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_40, 0);

    auto *slider = lv_obj_create(sheet);
    Clear(slider);
    lv_obj_set_size(slider, kConfirmTrackWidth, kConfirmTrackHeight);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -190);
    lv_obj_set_style_radius(slider, kConfirmTrackHeight / 2, 0);
    lv_obj_set_style_bg_color(slider, kInset, 0);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(slider, 1, 0);
    lv_obj_set_style_border_color(slider, kMainLine, 0);
    lv_obj_set_style_border_opa(slider, LV_OPA_50, 0);
    lv_obj_set_style_clip_corner(slider, true, 0);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slider, ConfirmSliderTouch, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(slider, ConfirmSliderTouch, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(slider, ConfirmSliderTouch, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(slider, ConfirmSliderTouch, LV_EVENT_PRESS_LOST, state);

    state->fill = lv_obj_create(slider);
    Clear(state->fill);
    lv_obj_set_pos(state->fill, kConfirmFillInset, kConfirmFillInset);
    lv_obj_set_size(state->fill, 1,
                    kConfirmTrackHeight - 2 * kConfirmFillInset);
    lv_obj_set_style_radius(state->fill,
                            (kConfirmTrackHeight - 2 * kConfirmFillInset) / 2,
                            0);
    // Use the real theme accent as a restrained tint. This keeps the fill
    // visually tied to the handle and avoids the muddy, offset-looking block
    // produced by the opaque accent-soft surface.
    lv_obj_set_style_bg_color(state->fill, kAccent, 0);
    lv_obj_set_style_bg_opa(state->fill, LV_OPA_30, 0);
    lv_obj_add_flag(state->fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(state->fill, LV_OBJ_FLAG_CLICKABLE);

    state->slider_copy =
        Label(slider, "Swipe to confirm", &lv_font_montserrat_32, kMutedStrong);
    lv_obj_align(state->slider_copy, LV_ALIGN_CENTER, 36, 0);
    lv_obj_remove_flag(state->slider_copy, LV_OBJ_FLAG_CLICKABLE);

    state->knob = lv_obj_create(slider);
    Clear(state->knob);
    lv_obj_set_size(state->knob, kConfirmKnobSize, kConfirmKnobSize);
    lv_obj_set_pos(state->knob, kConfirmKnobInset, kConfirmKnobInset);
    lv_obj_set_style_radius(state->knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(state->knob, kAccent, 0);
    lv_obj_set_style_bg_opa(state->knob, LV_OPA_COVER, 0);
    lv_obj_remove_flag(state->knob, LV_OBJ_FLAG_CLICKABLE);
    auto *chevron = Label(state->knob, LV_SYMBOL_RIGHT,
                           &lv_font_montserrat_48, Color(0x071116));
    lv_obj_center(chevron);
    lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
  }

  auto *close = Button(sheet, state->confirm ? "Cancel" : "Close", [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_size(close, 1140, 100);
  lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(close, LV_OPA_10, LV_STATE_PRESSED);
  AnimateEnter(sheet, 0, 52);
}
}  // namespace recovery_ui2::widgets
