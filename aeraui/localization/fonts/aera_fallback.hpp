/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <string>

#include <lvgl.h>

namespace aeraui::fonts {

void InitializeFallbackFonts();
void SetFallbackLanguage(const std::string &language);
const lv_font_t *WithLanguageFallback(const lv_font_t *font);

}  // namespace aeraui::fonts
