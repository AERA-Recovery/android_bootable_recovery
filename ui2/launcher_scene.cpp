/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <string>

#include <lvgl.h>

#include "design.hpp"
#include "plugins/plugin_manager.hpp"
#include "retroarch_icon.hpp"
#include "ui_components.hpp"

namespace recovery_ui2 {
namespace {
using namespace design;
using namespace widgets;

lv_obj_t *AppCard(lv_obj_t *screen, int x, int y, int width,
                  const char *icon, const char *name, const char *description,
                  lv_color_t accent, Handler action,
                  bool retroarch_icon = false) {
  auto *card = lv_button_create(screen);
  Panel(card, 44, kMainPanel);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, 360);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
  OnClick(card, std::move(action));
  auto *plate = retroarch_icon
      ? RetroArchIconPlate(card, kText, 104)
      : IconPlate(card, icon, accent, kMainSheet, 104);
  lv_obj_set_pos(plate, 36, 38);
  auto *title = Label(card, name, &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 36, 178);
  auto *copy = Label(card, description, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 36, 242);
  lv_obj_set_width(copy, width - 110);
  auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kDim);
  lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, -38, 74);
  return card;
}

}  // namespace

void BuildHomeScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  Header(screen, "Home", "Your recovery apps, files and extensions.", callback, context);
  const auto installed = plugins::Installed();
  const std::string summary = std::to_string(installed.size()) +
      (installed.size() == 1 ? " plugin installed" : " plugins installed");
  const bool landscape = lv_obj_get_width(screen) > lv_obj_get_height(screen);
  if (landscape) {
    auto *status = Kicker(screen, summary.c_str(), installed.empty() ? kMuted : kGreen);
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -80, 306);
    auto *apps = Label(screen, "AERA APPS", &lv_font_montserrat_18, kMuted);
    lv_obj_set_style_text_letter_space(apps, 3, 0);
    lv_obj_set_pos(apps, 80, 306);

    int index = 0;
    auto add = [&](const char *icon, const char *name, const char *description,
                   lv_color_t accent, Action action, bool retro_icon = false) {
      if (index >= 8) return;
      constexpr int kWidth = 736;
      constexpr int kGap = 16;
      const int column = index % 4;
      const int row = index / 4;
      const int x = 64 + column * (kWidth + kGap);
      const int y = 356 + row * 378;
      auto *card = AppCard(screen, x, y, kWidth, icon, name, description,
                           accent, [=] { callback(action, context); },
                           retro_icon);
      AnimateEnter(card, 20 + index * 22, 12);
      ++index;
    };
    add(LV_SYMBOL_DIRECTORY, "Files",
        "Browse storage, preview images and install ZIPs.", kAccent,
        Action::kFiles);
    add(LV_SYMBOL_DOWNLOAD, "Plugin Manager",
        "Discover and install signed AERA extensions.", kCyan,
        Action::kPlugins);
    add(LV_SYMBOL_EDIT, "Terminal",
        "A real recovery shell powered by OrangeFox libvterm.", kAccent,
        Action::kTerminal);
    for (const auto &plugin : installed) {
      if (index >= 8) break;
      if (plugin.entry == "browser")
        add(LV_SYMBOL_GPS, plugin.name.c_str(), plugin.description.c_str(),
            kCyan, Action::kWeb);
      else if (plugin.entry == "retroarch")
        add(LV_SYMBOL_PLAY, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kRetroArch, true);
      else if (plugin.entry == "telegram")
        add(LV_SYMBOL_ENVELOPE, plugin.name.c_str(), plugin.description.c_str(),
            kCyan, Action::kTelegram);
      else if (plugin.entry == "gallery")
        add(LV_SYMBOL_IMAGE, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kGallery);
      else if (plugin.entry == "media")
        add(LV_SYMBOL_PLAY, plugin.name.c_str(), plugin.description.c_str(),
            kCyan, Action::kMedia);
      else if (plugin.entry == "recorder")
        add(LV_SYMBOL_VIDEO, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kRecorder);
      else if (plugin.entry == "appvault")
        add(LV_SYMBOL_SAVE, plugin.name.c_str(), plugin.description.c_str(),
            kCyan, Action::kAppVault);
    }
    Navigation(screen, Action::kBackHome, callback, context);
    return;
  }
  auto *status = Kicker(screen, summary.c_str(), installed.empty() ? kMuted : kGreen);
  lv_obj_set_pos(status, 80, 426);

  auto *apps = Label(screen, "SYSTEM APPS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(apps, 3, 0);
  lv_obj_set_pos(apps, 80, 516);
  auto *files = AppCard(screen, 64, 574, 636, LV_SYMBOL_DIRECTORY, "Files",
                        "Browse storage, preview images and install ZIPs.",
                        kAccent, [=] { callback(Action::kFiles, context); });
  auto *store = AppCard(screen, 740, 574, 636, LV_SYMBOL_DOWNLOAD,
                        "Plugin Manager",
                        "Discover signed apps and install them to storage or RAM.",
                        kCyan, [=] { callback(Action::kPlugins, context); });
  auto *terminal = AppCard(screen, 64, 974, 1312, LV_SYMBOL_EDIT, "Terminal",
                           "Run recovery commands in the built-in OrangeFox shell.",
                           kGreen, [=] { callback(Action::kTerminal, context); });
  AnimateEnter(files, 20, 14);
  AnimateEnter(store, 55, 14);
  AnimateEnter(terminal, 80, 14);

  auto *extensions = Label(screen, "INSTALLED PLUGINS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(extensions, 3, 0);
  lv_obj_set_pos(extensions, 80, 1410);
  int x = 64;
  int y = 1468;
  for (const auto &plugin : installed) {
    Action action = Action::kNone;
    const char *icon = LV_SYMBOL_GPS;
    lv_color_t accent = kCyan;
    if (plugin.entry == "browser") {
      action = Action::kWeb;
    } else if (plugin.entry == "retroarch") {
      action = Action::kRetroArch;
      icon = LV_SYMBOL_PLAY;
      accent = kAccent;
    } else if (plugin.entry == "telegram") {
      action = Action::kTelegram;
      icon = LV_SYMBOL_ENVELOPE;
      accent = kCyan;
    } else if (plugin.entry == "gallery") {
      action = Action::kGallery;
      icon = LV_SYMBOL_IMAGE;
      accent = kAccent;
    } else if (plugin.entry == "media") {
      action = Action::kMedia;
      icon = LV_SYMBOL_PLAY;
      accent = kCyan;
    } else if (plugin.entry == "recorder") {
      action = Action::kRecorder;
      icon = LV_SYMBOL_VIDEO;
      accent = kAccent;
    } else if (plugin.entry == "appvault") {
      action = Action::kAppVault;
      icon = LV_SYMBOL_SAVE;
      accent = kCyan;
    } else {
      continue;
    }
    auto *card = AppCard(screen, x, y, 636, icon, plugin.name.c_str(),
                         plugin.description.c_str(), accent,
                         [=] { callback(action, context); },
                         plugin.entry == "retroarch");
    auto *location = Kicker(card, plugins::LocationLabel(plugin.location), kGreen);
    lv_obj_align(location, LV_ALIGN_TOP_RIGHT, -82, 72);
    AnimateEnter(card, 90, 14);
    x = x == 64 ? 740 : 64;
    if (x == 64) y += 400;
  }
  if (installed.empty()) {
    auto *empty = lv_obj_create(screen);
    Panel(empty, 40, kMainSheet);
    lv_obj_set_pos(empty, 64, 1468);
    lv_obj_set_size(empty, 1312, 310);
    auto *title = Label(empty, "Build AERA your way", &lv_font_montserrat_48, kText);
    lv_obj_set_pos(title, 42, 48);
    auto *copy = Label(empty,
        "Install the browser now. Audio and GPU extensions can follow without growing recovery.img.",
        &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(copy, 42, 120);
    lv_obj_set_width(copy, 1180);
    auto *open = Button(empty, "Open Plugin Manager", [=] {
      callback(Action::kPlugins, context);
    }, true);
    lv_obj_set_pos(open, 42, 196);
    lv_obj_set_size(open, 540, 92);
    lv_obj_set_style_radius(open, 28, 0);
    AnimateEnter(empty, 90, 14);
  }
  Navigation(screen, Action::kBackHome, callback, context);
}

}  // namespace recovery_ui2
