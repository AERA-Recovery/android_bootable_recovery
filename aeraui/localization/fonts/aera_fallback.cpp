/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "aera_fallback.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#ifndef AERA_FONT_ROOT
#define AERA_FONT_ROOT "/twres/fonts"
#endif
#ifndef AERA_COMMON_FONT_ROOT
#define AERA_COMMON_FONT_ROOT AERA_FONT_ROOT
#endif

namespace aeraui::fonts {
namespace {

constexpr std::array<int, 9> kSizes{16, 18, 20, 24, 28, 32, 36, 40, 48};
constexpr std::array<const lv_font_t *, 9> kBaseFonts{
    &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20,
    &lv_font_montserrat_24, &lv_font_montserrat_28, &lv_font_montserrat_32,
    &lv_font_montserrat_36, &lv_font_montserrat_40, &lv_font_montserrat_48};

enum class Script : size_t {
  kGeneral,
  kArabic,
  kHebrew,
  kDevanagari,
  kBengali,
  kThai,
  kJapanese,
  kCjk,
  kCount,
};

constexpr std::array<const char *, static_cast<size_t>(Script::kCount)>
    kFontPaths{
        AERA_COMMON_FONT_ROOT "/Roboto-Regular.ttf",
        AERA_FONT_ROOT "/NotoNaskhArabicUI-Regular.ttf",
        AERA_FONT_ROOT "/NotoSansHebrew-Regular.ttf",
        AERA_FONT_ROOT "/NotoSansDevanagariUI-VF.ttf",
        AERA_FONT_ROOT "/NotoSansBengaliUI-VF.ttf",
        AERA_FONT_ROOT "/NotoSansThaiUI-Regular.ttf",
        AERA_FONT_ROOT "/NotoSansCJKjp-Regular.ttf",
        AERA_FONT_ROOT "/wqy-microhei.ttf",
    };

struct FontSize {
  lv_font_t wrapper{};
  std::array<lv_font_t *, static_cast<size_t>(Script::kCount)> fallback{};
  bool loaded = false;
};

std::array<FontSize, kSizes.size()> fonts;
std::map<uint32_t, lv_font_t *> display_fonts;
bool initialized = false;
std::string language = "en";

std::string BaseLanguage(std::string code) {
  std::replace(code.begin(), code.end(), '-', '_');
  const auto separator = code.find('_');
  if (separator != std::string::npos) code.resize(separator);
  std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return code;
}

Script PrimaryScript(const std::string &code) {
  const std::string base = BaseLanguage(code);
  if (base == "ar" || base == "fa") return Script::kArabic;
  if (base == "he") return Script::kHebrew;
  if (base == "hi") return Script::kDevanagari;
  if (base == "bn") return Script::kBengali;
  if (base == "th") return Script::kThai;
  if (base == "ja") return Script::kJapanese;
  if (base == "zh" || base == "ko") return Script::kCjk;
  return Script::kGeneral;
}

void LinkFallbacks(FontSize &set) {
  std::vector<Script> order{
      Script::kGeneral, Script::kArabic, Script::kHebrew,
      Script::kDevanagari, Script::kBengali, Script::kThai,
      Script::kJapanese, Script::kCjk};
  const Script primary = PrimaryScript(language);
  if (primary != Script::kGeneral) {
    order.erase(std::remove(order.begin(), order.end(), primary), order.end());
    order.insert(order.begin() + 1, primary);
  }
  lv_font_t *previous = nullptr;
  for (const Script script : order) {
    lv_font_t *font = set.fallback[static_cast<size_t>(script)];
    if (font == nullptr) continue;
    if (previous == nullptr)
      set.wrapper.fallback = font;
    else
      previous->fallback = font;
    previous = font;
  }
  if (previous != nullptr) previous->fallback = nullptr;
}

void LoadSize(size_t index) {
  auto &set = fonts[index];
  if (set.loaded) return;
  set.loaded = true;
  for (size_t script = 0; script < kFontPaths.size(); ++script) {
#ifndef AERA_EXTRA_LANGUAGES
    if (script != static_cast<size_t>(Script::kGeneral)) continue;
#endif
    set.fallback[script] = lv_freetype_font_create(
        kFontPaths[script], LV_FREETYPE_FONT_RENDER_MODE_BITMAP, kSizes[index],
        LV_FREETYPE_FONT_STYLE_NORMAL);
  }
  LinkFallbacks(set);
}

}  // namespace

void InitializeFallbackFonts() {
  if (initialized) return;
  initialized = true;
  for (size_t i = 0; i < kSizes.size(); ++i)
    fonts[i].wrapper = *kBaseFonts[i];
}

void SetFallbackLanguage(const std::string &requested) {
  language = requested;
  InitializeFallbackFonts();
  for (auto &font : fonts)
    if (font.loaded) LinkFallbacks(font);
}

const lv_font_t *WithLanguageFallback(const lv_font_t *font) {
  if (font == nullptr) return nullptr;
  InitializeFallbackFonts();
  for (size_t i = 0; i < kBaseFonts.size(); ++i) {
    if (font == kBaseFonts[i]) {
      LoadSize(i);
      return &fonts[i].wrapper;
    }
  }
  return font;
}

const lv_font_t *DisplayFont(uint32_t size) {
  InitializeFallbackFonts();
  const auto existing = display_fonts.find(size);
  if (existing != display_fonts.end()) return existing->second;

  lv_font_t *font = lv_freetype_font_create(
      kFontPaths[static_cast<size_t>(Script::kGeneral)],
      LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size,
      LV_FREETYPE_FONT_STYLE_NORMAL);
  if (font == nullptr) return WithLanguageFallback(&lv_font_montserrat_48);
  display_fonts.emplace(size, font);
  return font;
}

}  // namespace aeraui::fonts
