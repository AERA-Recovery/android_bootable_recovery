/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "lvgl.h"
#include "recovery_ui2/backend.hpp"

namespace recovery_ui2::phone_keyboard {

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

inline void Apply(lv_obj_t *keyboard) {
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER,
                      kLower, kTextControls);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER,
                      kUpper, kTextControls);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL,
                      kSpecial, kSpecialControls);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_popovers(keyboard, true);
  lv_obj_add_event_cb(keyboard, [](lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED)
      RecoveryVibrate(Haptic::kKeyboard);
  }, LV_EVENT_VALUE_CHANGED, nullptr);
}

}  // namespace recovery_ui2::phone_keyboard
