/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
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

lv_obj_t *AppIconPlate(lv_obj_t *parent, const char *symbol,
                       lv_color_t accent, lv_color_t fill, int size,
                       bool retroarch_icon = false) {
  auto *plate = lv_obj_create(parent);
  Panel(plate, size / 3, fill);
  lv_obj_set_size(plate, size, size);
  lv_obj_set_style_border_width(plate, 1, 0);
  lv_obj_set_style_border_color(plate, accent, 0);
  lv_obj_set_style_border_opa(plate, LV_OPA_30, 0);
  if (retroarch_icon) {
    auto *mark = RetroArchIconPlate(plate, accent, size - 22);
    lv_obj_center(mark);
  } else {
    auto *mark = Label(plate, symbol, &lv_font_montserrat_48, accent);
    lv_obj_center(mark);
  }
  return plate;
}

const char *GenericPluginIcon(const plugins::Plugin &plugin) {
  if (plugin.id == "mirror") return LV_SYMBOL_VIDEO;
  if (plugin.id.find("settings") != std::string::npos)
    return LV_SYMBOL_SAVE;
  return LV_SYMBOL_SETTINGS;
}

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
  auto *plate = AppIconPlate(card, icon, accent, kMainSheet, 112,
                             retroarch_icon);
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

lv_obj_t *PluginTile(lv_obj_t *parent, int x, int y, int width, int height,
                     const char *icon, const plugins::Plugin &plugin,
                     lv_color_t accent, Handler action,
                     bool retroarch_icon = false) {
  auto *card = lv_button_create(parent);
  Panel(card, 34, kMainSheet);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
  auto *plate = AppIconPlate(card, icon, accent, kMainPanel, 138,
                             retroarch_icon);
  lv_obj_align(plate, LV_ALIGN_TOP_MID, 0, 14);
  if (!retroarch_icon) {
    auto *mark = lv_obj_get_child(plate, 0);
    lv_obj_set_style_transform_scale(mark, 288, 0);
  }
  auto *title = Label(card, plugin.name.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_width(title, width - 40);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 180);
  OnClick(card, std::move(action));
  return card;
}

struct PluginPagerState {
  lv_obj_t *view = nullptr;
  lv_obj_t *dots = nullptr;
  int pages = 0;
  int page_width = 0;
};

void UpdatePluginPageDots(PluginPagerState *state) {
  if (state == nullptr || state->view == nullptr || state->dots == nullptr)
    return;
  const int scroll_x = lv_obj_get_scroll_x(state->view);
  const int offset = scroll_x < 0 ? -scroll_x : scroll_x;
  const int selected = std::clamp(
      (offset + state->page_width / 2) / state->page_width,
      0, state->pages - 1);
  std::string dots;
  for (int page = 0; page < state->pages; ++page) {
    if (page != 0) dots += "   ";
    dots += page == selected ? "●" : "○";
  }
  lv_label_set_text(state->dots, dots.c_str());
}

void PluginPagerEvent(lv_event_t *event) {
  auto *state = static_cast<PluginPagerState *>(lv_event_get_user_data(event));
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete state;
    return;
  }
  if (lv_event_get_code(event) == LV_EVENT_SCROLL_END)
    UpdatePluginPageDots(state);
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
                   lv_color_t accent, Action action, bool retro_icon = false,
                   const std::string &plugin_id = std::string()) {
      if (index >= 8) return;
      constexpr int kWidth = 736;
      constexpr int kGap = 16;
      const int column = index % 4;
      const int row = index / 4;
      const int x = 64 + column * (kWidth + kGap);
      const int y = 356 + row * 378;
      auto *card = AppCard(screen, x, y, kWidth, icon, name, description,
                           accent, [=] {
                             if (!plugin_id.empty())
                               SetSelectedPluginId(plugin_id);
                             callback(action, context);
                           },
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
        "A real recovery shell built into AERA.", kAccent,
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
      else if (plugins::IsGeneric(plugin))
        add(GenericPluginIcon(plugin), plugin.name.c_str(),
            plugin.description.c_str(), kAccent, Action::kPluginApp, false,
            plugin.id);
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
                           "Run recovery commands in the built-in AERA shell.",
                           kGreen, [=] { callback(Action::kTerminal, context); });
  AnimateEnter(files, 20, 14);
  AnimateEnter(store, 55, 14);
  AnimateEnter(terminal, 80, 14);

  auto *extensions = Label(screen, "INSTALLED PLUGINS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(extensions, 3, 0);
  lv_obj_set_pos(extensions, 80, 1410);
  constexpr int kGridWidth = 1312;
  constexpr int kGridHeight = 1050;
  constexpr int kTileWidth = 416;
  constexpr int kTileHeight = 318;
  constexpr int kGapX = 32;
  constexpr int kGapY = 30;
  auto *pager = lv_obj_create(screen);
  lv_obj_set_pos(pager, 64, 1468);
  lv_obj_set_size(pager, kGridWidth, kGridHeight);
  lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pager, 0, 0);
  lv_obj_set_style_pad_all(pager, 0, 0);
  lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_CENTER);
  lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_remove_flag(pager, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  const int page_count = std::max(1, static_cast<int>((installed.size() + 8) / 9));
  int visible_index = 0;
  lv_obj_t *page = nullptr;
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
    } else if (plugins::IsGeneric(plugin)) {
      action = Action::kPluginApp;
      icon = GenericPluginIcon(plugin);
      accent = kAccent;
    } else {
      continue;
    }
    const int page_index = visible_index / 9;
    const int slot = visible_index % 9;
    if (slot == 0) {
      page = lv_obj_create(pager);
      Clear(page);
      lv_obj_set_pos(page, page_index * kGridWidth, 0);
      lv_obj_set_size(page, kGridWidth, kGridHeight);
      lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
    }
    const int column = slot % 3;
    const int row = slot / 3;
    auto *card = PluginTile(page, column * (kTileWidth + kGapX),
                            row * (kTileHeight + kGapY),
                            kTileWidth, kTileHeight, icon, plugin, accent,
                            [=] {
                              if (action == Action::kPluginApp)
                                SetSelectedPluginId(plugin.id);
                              callback(action, context);
                            }, plugin.entry == "retroarch");
    AnimateEnter(card, 90 + slot * 18, 10);
    ++visible_index;
  }
  if (installed.empty()) {
    lv_obj_add_flag(pager, LV_OBJ_FLAG_HIDDEN);
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
  } else if (page_count > 1) {
    auto *dots = Label(screen, "", &lv_font_montserrat_24, kAccent);
    lv_obj_set_width(dots, 600);
    lv_obj_set_style_text_align(dots, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(dots, 420, 2540);
    auto *pager_state =
        new PluginPagerState{pager, dots, page_count, kGridWidth};
    lv_obj_add_event_cb(pager, PluginPagerEvent, LV_EVENT_ALL, pager_state);
    UpdatePluginPageDots(pager_state);
  }
  Navigation(screen, Action::kBackHome, callback, context);
}

}  // namespace recovery_ui2
