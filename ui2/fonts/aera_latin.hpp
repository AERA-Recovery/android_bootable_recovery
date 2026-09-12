/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(aera_latin_16);
LV_FONT_DECLARE(aera_latin_18);
LV_FONT_DECLARE(aera_latin_20);
LV_FONT_DECLARE(aera_latin_24);
LV_FONT_DECLARE(aera_latin_28);
LV_FONT_DECLARE(aera_latin_32);
LV_FONT_DECLARE(aera_latin_36);
LV_FONT_DECLARE(aera_latin_40);
LV_FONT_DECLARE(aera_latin_48);

#ifdef __cplusplus
}
#endif

namespace recovery_ui2::fonts {

inline lv_font_t WithFallback(const lv_font_t &base,
                              const lv_font_t &fallback) {
  lv_font_t font = base;
  font.fallback = &fallback;
  return font;
}

inline const lv_font_t *WithLatinFallback(const lv_font_t *font) {
  static lv_font_t font16 = WithFallback(lv_font_montserrat_16, aera_latin_16);
  static lv_font_t font18 = WithFallback(lv_font_montserrat_18, aera_latin_18);
  static lv_font_t font20 = WithFallback(lv_font_montserrat_20, aera_latin_20);
  static lv_font_t font24 = WithFallback(lv_font_montserrat_24, aera_latin_24);
  static lv_font_t font28 = WithFallback(lv_font_montserrat_28, aera_latin_28);
  static lv_font_t font32 = WithFallback(lv_font_montserrat_32, aera_latin_32);
  static lv_font_t font36 = WithFallback(lv_font_montserrat_36, aera_latin_36);
  static lv_font_t font40 = WithFallback(lv_font_montserrat_40, aera_latin_40);
  static lv_font_t font48 = WithFallback(lv_font_montserrat_48, aera_latin_48);
  if (font == &lv_font_montserrat_16) return &font16;
  if (font == &lv_font_montserrat_18) return &font18;
  if (font == &lv_font_montserrat_20) return &font20;
  if (font == &lv_font_montserrat_24) return &font24;
  if (font == &lv_font_montserrat_28) return &font28;
  if (font == &lv_font_montserrat_32) return &font32;
  if (font == &lv_font_montserrat_36) return &font36;
  if (font == &lv_font_montserrat_40) return &font40;
  if (font == &lv_font_montserrat_48) return &font48;
  return font;
}

}  // namespace recovery_ui2::fonts
