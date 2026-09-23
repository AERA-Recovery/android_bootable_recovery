/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>
#include <cstring>

#include "lvgl.h"
#include "design.hpp"
#include "aeraui/backend.hpp"

namespace aeraui::phone_keyboard {

constexpr lv_buttonmatrix_ctrl_t Key(unsigned width = 1,
                                     unsigned flags = 0) {
  return static_cast<lv_buttonmatrix_ctrl_t>((width & 0x0fU) | flags);
}

constexpr unsigned kLetter = LV_BUTTONMATRIX_CTRL_POPOVER;
constexpr unsigned kUtility = LV_BUTTONMATRIX_CTRL_NO_REPEAT;

inline constexpr const char *kLower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "?123", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kUpper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "?123", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kLowerQwertz[] = {
    "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    LV_SYMBOL_UP, "y", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "?123", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kUpperQwertz[] = {
    "Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    LV_SYMBOL_UP, "Y", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "?123", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kSpecial[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    " ", "@", "#", "$", "&", "*", "(", ")", "'", "\"", " ", "\n",
    "!", "?", "/", ":", ";", "-", "+", "=", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr lv_buttonmatrix_ctrl_t kTextControls[] = {
    Key(1, kLetter), Key(1, kLetter), Key(1, kLetter), Key(1, kLetter),
    Key(1, kLetter), Key(1, kLetter), Key(1, kLetter), Key(1, kLetter),
    Key(1, kLetter), Key(1, kLetter),
    Key(1, LV_BUTTONMATRIX_CTRL_HIDDEN),
    Key(2, kLetter), Key(2, kLetter), Key(2, kLetter), Key(2, kLetter),
    Key(2, kLetter), Key(2, kLetter), Key(2, kLetter), Key(2, kLetter),
    Key(2, kLetter), Key(1, LV_BUTTONMATRIX_CTRL_HIDDEN),
    Key(2, kUtility), Key(1, kLetter), Key(1, kLetter), Key(1, kLetter),
    Key(1, kLetter), Key(1, kLetter), Key(1, kLetter), Key(1, kLetter),
    Key(2, kUtility),
    Key(2, kUtility), Key(1), Key(8), Key(1), Key(2, kUtility)};

inline constexpr lv_buttonmatrix_ctrl_t kSpecialControls[] = {
    Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(),
    Key(1, LV_BUTTONMATRIX_CTRL_HIDDEN),
    Key(2), Key(2), Key(2), Key(2), Key(2), Key(2), Key(2), Key(2), Key(2),
    Key(1, LV_BUTTONMATRIX_CTRL_HIDDEN),
    Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(2, kUtility),
    Key(2, kUtility), Key(1), Key(8), Key(1), Key(2, kUtility)};

inline bool SameKey(const char *key, const char *expected) {
  return key != nullptr && std::strcmp(key, expected) == 0;
}

template <size_t N>
inline std::array<const char *, N> MultilineMap(const char *const (&source)[N]) {
  std::array<const char *, N> result{};
  for (size_t index = 0; index < N; ++index)
    result[index] = SameKey(source[index], LV_SYMBOL_OK)
                        ? LV_SYMBOL_NEW_LINE
                        : source[index];
  return result;
}

inline const auto kLowerMultiline = MultilineMap(kLower);
inline const auto kUpperMultiline = MultilineMap(kUpper);
inline const auto kLowerQwertzMultiline = MultilineMap(kLowerQwertz);
inline const auto kUpperQwertzMultiline = MultilineMap(kUpperQwertz);
inline const auto kSpecialMultiline = MultilineMap(kSpecial);

inline bool IsActionKey(const char *key) {
  return SameKey(key, LV_SYMBOL_OK) || SameKey(key, LV_SYMBOL_NEW_LINE);
}

inline bool IsModifierKey(const char *key) {
  return SameKey(key, LV_SYMBOL_UP) || SameKey(key, LV_SYMBOL_BACKSPACE) ||
         SameKey(key, "?123") || SameKey(key, "ABC");
}

inline void DrawKey(lv_event_t *event) {
  auto *keyboard = lv_event_get_target_obj(event);
  auto *task = lv_event_get_draw_task(event);
  if (keyboard == nullptr || task == nullptr) return;
  auto *base = static_cast<lv_draw_dsc_base_t *>(
      lv_draw_task_get_draw_dsc(task));
  if (base == nullptr || base->part != LV_PART_ITEMS) return;

  const char *key = lv_buttonmatrix_get_button_text(keyboard, base->id1);
  if (key == nullptr) return;
  const bool pressed =
      lv_keyboard_get_selected_button(keyboard) == base->id1 &&
      lv_obj_has_state(keyboard, LV_STATE_PRESSED);
  const bool action = IsActionKey(key);
  const bool modifier = IsModifierKey(key);

  if (auto *fill = lv_draw_task_get_fill_dsc(task)) {
    fill->color = action ? (pressed ? design::kAccentPressed : design::kAccent)
                  : modifier ? (pressed ? design::kMainSelected
                                        : design::kAccentSoft)
                  : pressed ? design::kMainSelected : design::kMainPanel;
  }
  if (auto *label = lv_draw_task_get_label_dsc(task)) {
    label->color = action ? design::kOnAccent
                   : modifier ? design::kAccent : design::kText;
  }
}

inline void Style(lv_obj_t *keyboard) {
  lv_obj_set_style_bg_color(keyboard, design::kMainBottom, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(keyboard, 30, LV_PART_MAIN);
  lv_obj_set_style_pad_all(keyboard, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_row(keyboard, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_column(keyboard, 10, LV_PART_MAIN);

  lv_obj_set_style_text_font(
      keyboard, design::UiFont(&lv_font_montserrat_48), LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, design::kText, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, design::kMainPanel, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, design::kMainSelected,
                            LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_radius(keyboard, 24, LV_PART_ITEMS);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);

  lv_obj_add_event_cb(keyboard, DrawKey, LV_EVENT_DRAW_TASK_ADDED, nullptr);
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
}

inline void Apply(lv_obj_t *keyboard, bool multiline = false) {
  const bool qwertz = RecoveryKeyboardLayout() == KeyboardLayout::kQwertz;
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER,
                      multiline
                          ? (qwertz ? kLowerQwertzMultiline.data()
                                   : kLowerMultiline.data())
                          : (qwertz ? kLowerQwertz : kLower),
                      kTextControls);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER,
                      multiline
                          ? (qwertz ? kUpperQwertzMultiline.data()
                                   : kUpperMultiline.data())
                          : (qwertz ? kUpperQwertz : kUpper),
                      kTextControls);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL,
                      multiline ? kSpecialMultiline.data() : kSpecial,
                      kSpecialControls);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_popovers(keyboard, true);
  Style(keyboard);
  lv_obj_add_event_cb(keyboard, [](lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED)
      RecoveryVibrate(Haptic::kKeyboard);
  }, LV_EVENT_VALUE_CHANGED, nullptr);
}

}  // namespace aeraui::phone_keyboard
