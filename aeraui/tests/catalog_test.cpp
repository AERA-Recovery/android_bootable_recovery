/* SPDX-License-Identifier: Apache-2.0 */
#include <aeraui/i18n.hpp>
#include "localization/catalog/catalog.hpp"

#include <json/json.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace aeraui::fonts {
void SetFallbackLanguage(const std::string &) {}
}

int main(int argc, char **argv) {
  assert(argc == 2);
  std::ifstream source(argv[1]);
  Json::Value catalog;
  source >> catalog;
  assert(catalog["schema"].asInt() == 1);
  const char *required_theme_tags[]{
    "Reboot to %s?", "Rebooting to %s", "Active slot: %s", "%d s", "24-hour clock",
    "Swipe up to unlock", "Hold to unlock", "Unlock gesture", "Slider",
    "Swipe up", "Hold", "AOD brightness", "Always-on clock", "Clock style",
    "Clock position", "Clock colour", "Date style", "Date position",
    "Surface style", "Solid", "Frosted", "Outline", "Elevated",
    "Typography", "Density", "Wallpaper", "Lock screen", "Text Only",
    "Choose lock-screen wallpaper", "Edit owner text",
    "Dim clock on black. Uses more power than screen off. Wake with Power."
  };
  for (const auto *tag : required_theme_tags) {
    assert(catalog["strings"].isMember(tag));
    for (const auto &language : catalog["languages"]) {
      assert(catalog["strings"][tag].isMember(language.asString()));
      assert(!catalog["strings"][tag][language.asString()].asString().empty());
    }
  }
  lv_init();
  lv_display_create(1440, 3168);
  aeraui::i18n::Initialize("en");
  assert(aeraui::i18n::RegisterCatalog());
  const auto &languages = aeraui::i18n::AvailableLanguages();
#ifdef AERA_EXTRA_LANGUAGES
  assert(languages.size() == catalog["languages"].size());
#else
  assert(languages.size() == 1 && std::strcmp(languages[0].code, "en") == 0);
#endif
  size_t checked = 0;
  const char *retained = nullptr;
  std::string retained_copy;
  auto *label = lv_label_create(lv_screen_active());
  auto *reference = lv_label_create(lv_screen_active());
  aeraui::i18n::BindLabel(label, "Paste");
  for (const auto &language : languages) {
    assert(aeraui::i18n::SetLanguage(language.code));
    assert(aeraui::i18n::CurrentLanguage() == std::string(language.code));
    assert(std::strcmp(lv_translation_get_language(), language.code) == 0);
    const auto &strings = catalog["strings"];
    for (const auto &tag : strings.getMemberNames()) {
      std::string expected = std::strcmp(language.code, "en") == 0 ? tag :
          strings[tag].get(language.code, tag).asString();
      if (expected.empty()) expected = tag;
      const auto *actual = aeraui::i18n::Translate(tag.c_str());
      if (expected != actual) {
        std::fprintf(stderr, "Translation mismatch: %s / %s\n", language.code, tag.c_str());
        return 1;
      }
      ++checked;
    }
    // LVGL shapes Arabic/Persian text before storing it in a label.
    lv_label_set_text(reference, aeraui::i18n::Translate("Paste"));
    assert(std::strcmp(lv_label_get_text(label), lv_label_get_text(reference)) == 0);
    if (retained) assert(retained_copy == retained);
    else { retained = aeraui::i18n::Translate("Paste"); retained_copy = retained; }
  }
#ifdef AERA_EXTRA_LANGUAGES
  assert(aeraui::i18n::SetLanguage("ru"));
  assert(aeraui::i18n::Format("Reboot to %s?", "Android") == "\u041f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c\u0441\u044f \u0432 Android?");
  assert(aeraui::i18n::Format("Active slot: %s", "B") == "\u0410\u043a\u0442\u0438\u0432\u043d\u044b\u0439 \u0441\u043b\u043e\u0442: B");
  assert(aeraui::i18n::Format("%d s", 120) == "120 \u0441");
  assert(aeraui::i18n::Format("%zu installed modules", size_t{10}) == "\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u043d\u044b\u0435 \u043c\u043e\u0434\u0443\u043b\u0438: 10");
  assert(std::strcmp(aeraui::i18n::Translate("Start ADB sideload"), "\u041d\u0430\u0447\u0430\u0442\u044c ADB sideload") == 0);
  assert(std::strcmp(aeraui::i18n::Translate("24-hour clock"), "24-\u0447\u0430\u0441\u043e\u0432\u043e\u0439 \u0444\u043e\u0440\u043c\u0430\u0442") == 0);
#endif
  assert(std::strcmp(aeraui::i18n::Translate("missing catalog tag"), "missing catalog tag") == 0);
  assert(std::strcmp(aeraui::i18n::Translate(nullptr), "") == 0);
  assert(aeraui::i18n::RegisterCatalog());
  std::printf("Packed catalog: %zu translations and live language switching passed.\n", checked);
  lv_deinit();
}
