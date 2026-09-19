/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
#include <string>

#include <lvgl.h>

#include "design.hpp"
#include "matrix_engraving.hpp"
#include "plugins/plugin_manager.hpp"
#include "retroarch_icon.hpp"
#include "ui_components.hpp"
#include "webkit_icon.hpp"

namespace recovery_ui2 {
namespace {
using namespace design;
using namespace widgets;

// Decorative objects created with lv_obj_create() are clickable by default.
// When an icon is placed inside a button that makes the artwork win hit
// testing, leaving a dead spot in the middle of the card.  Keep the complete
// artwork tree transparent to input so every point on a Home card reaches the
// card itself.
void MakeDecorationPassThrough(lv_obj_t *object) {
  if (object == nullptr) return;
  lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  const uint32_t child_count = lv_obj_get_child_count(object);
  for (uint32_t index = 0; index < child_count; ++index)
    MakeDecorationPassThrough(lv_obj_get_child(object, index));
}

lv_obj_t *MaterialTerminalIcon(lv_obj_t *parent, lv_color_t color, int size) {
  auto *art = lv_obj_create(parent);
  Clear(art);
  lv_obj_set_size(art, size, size);

  auto *frame = lv_obj_create(art);
  Clear(frame);
  lv_obj_set_size(frame, 104, 82);
  lv_obj_center(frame);
  lv_obj_set_style_radius(frame, 12, 0);
  lv_obj_set_style_border_width(frame, 8, 0);
  lv_obj_set_style_border_color(frame, color, 0);
  lv_obj_set_style_border_opa(frame, LV_OPA_COVER, 0);

  static constexpr lv_point_precise_t kChevron[] = {
      {44, 57}, {61, 72}, {44, 87},
  };
  auto *chevron = lv_line_create(art);
  lv_line_set_points(chevron, kChevron, 3);
  lv_obj_set_style_line_width(chevron, 8, 0);
  lv_obj_set_style_line_color(chevron, color, 0);
  lv_obj_set_style_line_rounded(chevron, true, 0);

  auto *cursor = lv_obj_create(art);
  Clear(cursor);
  lv_obj_set_pos(cursor, 74, 82);
  lv_obj_set_size(cursor, 34, 8);
  lv_obj_set_style_radius(cursor, 4, 0);
  lv_obj_set_style_bg_color(cursor, color, 0);
  lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
  return art;
}

lv_obj_t *AppIconPlate(lv_obj_t *parent, const char *symbol,
                       lv_color_t accent, int size,
                       bool retroarch_icon = false,
                       bool terminal_icon = false,
                       bool webkit_icon = false) {
  auto *plate = lv_obj_create(parent);
  Clear(plate);
  lv_obj_set_size(plate, size, size);
  if (terminal_icon) {
    auto *mark = MaterialTerminalIcon(plate, accent, size);
    lv_obj_center(mark);
  } else if (webkit_icon) {
    auto *mark = WebKitIconPlate(plate, accent, 112);
    lv_obj_center(mark);
  } else if (retroarch_icon) {
    auto *mark = RetroArchIconPlate(plate, accent, size - 14);
    lv_obj_center(mark);
  } else {
    auto *mark = Label(plate, symbol, &lv_font_montserrat_48, accent);
    lv_obj_set_style_transform_scale(mark, 352, 0);
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

const char *PluginSummary(const plugins::Plugin &plugin) {
  if (plugin.entry == "browser") return "Browse the web with mobile WebKit.";
  if (plugin.entry == "retroarch") return "Play classic games with RetroArch.";
  if (plugin.entry == "doom") return "Play native Doom with touch controls.";
  if (plugin.entry == "telegram") return "Secure messaging and file sharing.";
  if (plugin.entry == "gallery") return "Browse, preview and zoom your photos.";
  if (plugin.entry == "media") return "Play music and video from storage.";
  if (plugin.entry == "streams") return "Stream videos and music without ads.";
  if (plugin.entry == "recorder") return "Record the recovery screen to video.";
  if (plugin.entry == "appvault") return "Back up and restore installed apps.";
  if (plugin.id == "mirror") return "View and control AERA from another screen.";
  return plugin.description.c_str();
}

void MarkUpdateAvailable(lv_obj_t *card, bool compact = false) {
  if (card == nullptr) return;
  lv_obj_set_style_border_width(card, 3, 0);
  lv_obj_set_style_border_color(card, kAccent, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
  auto *badge = Kicker(card, "UPDATE AVAILABLE", kAccent);
  if (compact)
    lv_obj_align(badge, LV_ALIGN_BOTTOM_MID, 0, -18);
  else
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -36, 28);
  MakeDecorationPassThrough(badge);

  // Draw attention once when Home opens without leaving a permanently
  // blinking control behind.
  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, card);
  lv_anim_set_values(&pulse, LV_OPA_60, LV_OPA_COVER);
  lv_anim_set_duration(&pulse, 520);
  lv_anim_set_delay(&pulse, 360);
  lv_anim_set_playback_duration(&pulse, 620);
  lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&pulse, [](void *target, int32_t opacity) {
    lv_obj_set_style_border_opa(static_cast<lv_obj_t *>(target),
                                static_cast<lv_opa_t>(opacity), 0);
  });
  lv_anim_start(&pulse);
}

lv_obj_t *AppCard(lv_obj_t *screen, int x, int y, int width,
                  const char *icon, const char *name, const char *description,
                  lv_color_t accent, Handler action,
                  bool retroarch_icon = false,
                  bool terminal_engraving = false,
                  bool webkit_icon = false) {
  auto *card = lv_button_create(screen);
  Panel(card, 44, kMainPanel);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, 360);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
  OnClick(card, std::move(action));
  if (terminal_engraving) {
    lv_obj_set_style_clip_corner(card, true, 0);
    AddMatrixEngraving(card, width, 360, kAccent);
  }
  auto *plate = AppIconPlate(card, icon, accent, 144, retroarch_icon,
                             terminal_engraving, webkit_icon);
  lv_obj_set_pos(plate, 28, 22);
  MakeDecorationPassThrough(plate);
  // 40 px is the Normal Home identity size. The shared scale maps it down for
  // Small and up to 48 px for Large, keeping all three modes visibly distinct.
  auto *title = Label(card, name, &lv_font_montserrat_40, kText);
  lv_obj_set_pos(title, 36, 188);
  FitLabelToLines(title, width - 72, 1,
                  {&lv_font_montserrat_40, &lv_font_montserrat_36,
                   &lv_font_montserrat_32, &lv_font_montserrat_28});
  auto *copy = Label(card, description, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 36, 250);
  FitLabelToLines(copy, width - 110, 2,
                  {&lv_font_montserrat_24, &lv_font_montserrat_20,
                   &lv_font_montserrat_18, &lv_font_montserrat_16});
  auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kDim);
  lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, -38, 74);
  return card;
}

lv_obj_t *PluginTile(lv_obj_t *parent, int x, int y, int width, int height,
                     const char *icon, const plugins::Plugin &plugin,
                     lv_color_t accent, Handler action,
                     bool retroarch_icon = false) {
  if (width >= 600) {
    auto *card = AppCard(parent, x, y, width, icon, plugin.name.c_str(),
                         PluginSummary(plugin), accent, std::move(action),
                         retroarch_icon, false, plugin.entry == "browser");
    lv_obj_set_height(card, height);
    return card;
  }
  auto *card = lv_button_create(parent);
  Panel(card, 34, kMainSheet);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
  const bool webkit_icon = plugin.entry == "browser";
  auto *plate = AppIconPlate(card, icon, accent, 154, retroarch_icon, false,
                             webkit_icon);
  lv_obj_align(plate, LV_ALIGN_TOP_MID, 0, 18);
  if (!retroarch_icon && !webkit_icon) {
    auto *mark = lv_obj_get_child(plate, 0);
    lv_obj_set_style_transform_scale(mark, 384, 0);
  }
  MakeDecorationPassThrough(plate);
  auto *title = Label(card, plugin.name.c_str(), &lv_font_montserrat_32, kText);
  FitLabelToLines(title, width - 40, 1,
                  {&lv_font_montserrat_32, &lv_font_montserrat_28,
                   &lv_font_montserrat_24, &lv_font_montserrat_20,
                   &lv_font_montserrat_18, &lv_font_montserrat_16});
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 214);
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
  for (int page = 0; page < state->pages; ++page) {
    auto *dot = lv_obj_get_child(state->dots, page);
    if (dot == nullptr) continue;
    const bool active = page == selected;
    lv_obj_set_size(dot, active ? 34 : 14, 14);
    lv_obj_set_style_bg_color(dot, active ? kAccent : kMainLine, 0);
    lv_obj_set_style_bg_opa(dot, active ? LV_OPA_COVER : LV_OPA_70, 0);
  }
}

void InvalidatePluginPagerGutters(PluginPagerState *state) {
  auto *screen = lv_obj_get_screen(state->view);
  lv_area_t screen_area{};
  lv_area_t pager_area{};
  lv_obj_get_coords(screen, &screen_area);
  lv_obj_get_coords(state->view, &pager_area);

  if (pager_area.x1 > screen_area.x1) {
    lv_area_t left{screen_area.x1, pager_area.y1,
                   pager_area.x1 - 1, pager_area.y2};
    lv_obj_invalidate_area(screen, &left);
  }
  if (pager_area.x2 < screen_area.x2) {
    lv_area_t right{pager_area.x2 + 1, pager_area.y1,
                    screen_area.x2, pager_area.y2};
    lv_obj_invalidate_area(screen, &right);
  }
}

void PluginPagerEvent(lv_event_t *event) {
  auto *state = static_cast<PluginPagerState *>(lv_event_get_user_data(event));
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete state;
    return;
  }
  if (lv_event_get_code(event) == LV_EVENT_SCROLL ||
      lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
    UpdatePluginPageDots(state);
    // The pager itself invalidates only its own bounds. Explicitly refresh the
    // two narrow outer gutters while transformed icon glyphs are moving so a
    // GPU frame cannot retain pixels that crossed the clipping edge.
    InvalidatePluginPagerGutters(state);
  }
  if (lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
    // Scaled icon glyphs can temporarily extend beyond the pager's invalidated
    // strip while GPU scrolling. Redraw the containing scene once after the
    // snap completes so no edge pixels survive outside the viewport.
    lv_obj_invalidate(lv_obj_get_screen(state->view));
  }
}

}  // namespace

void BuildHomeScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  Header(screen, "Home", "Your recovery apps, files and extensions.", callback, context);
  const auto installed = plugins::Installed();
  const auto updates = plugins::AvailableUpdates(installed);
  const auto has_update = [&](const std::string &id) {
    return std::any_of(updates.begin(), updates.end(),
                       [&](const plugins::PluginUpdate &update) {
                         return update.installed.id == id;
                       });
  };
  const bool landscape = lv_obj_get_width(screen) > lv_obj_get_height(screen);
  if (landscape) {
    auto *apps = Label(screen, "AERA APPS", &lv_font_montserrat_18, kMuted);
    lv_obj_set_style_text_letter_space(apps, 3, 0);
    lv_obj_set_pos(apps, 80, 306);

    int index = 0;
    auto add = [&](const char *icon, const char *name, const char *description,
                   lv_color_t accent, Action action, bool retro_icon = false,
                   const std::string &plugin_id = std::string(),
                   bool webkit_icon = false,
                   bool update_available = false) {
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
                           retro_icon, action == Action::kTerminal,
                           webkit_icon);
      if (update_available) MarkUpdateAvailable(card);
      AnimateEnter(card, 20 + index * 22, 12);
      ++index;
    };
    add(LV_SYMBOL_DIRECTORY, "Files",
        "Browse storage, preview images and install ZIPs.", kAccent,
        Action::kFiles);
    add(LV_SYMBOL_DOWNLOAD, "Plugin Manager",
        updates.empty() ? "Discover and install signed AERA extensions."
                        : i18n::Format("%zu updates available",
                                       updates.size()).c_str(),
        kAccent, Action::kPlugins, false, std::string(), false,
        !updates.empty());
    add(LV_SYMBOL_EDIT, "Terminal",
        "A real recovery shell built into AERA.", kAccent,
        Action::kTerminal);
    for (const auto &plugin : installed) {
      if (index >= 8) break;
      if (plugin.entry == "browser")
        add(LV_SYMBOL_GPS, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kWeb, false, plugin.id, true,
            has_update(plugin.id));
      else if (plugin.entry == "retroarch")
        add(LV_SYMBOL_PLAY, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kRetroArch, true, plugin.id, false,
            has_update(plugin.id));
      else if (plugin.entry == "doom")
        add(LV_SYMBOL_PLAY, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kDoom, false, plugin.id, false,
            has_update(plugin.id));
      else if (plugin.entry == "telegram")
        add(LV_SYMBOL_GPS, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kTelegram, false, plugin.id, false,
            has_update(plugin.id));
      else if (plugin.entry == "gallery")
        add(LV_SYMBOL_IMAGE, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kGallery, false, plugin.id, false,
            has_update(plugin.id));
      else if (plugin.entry == "media")
        add(LV_SYMBOL_PLAY, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kMedia, false, plugin.id, false,
            has_update(plugin.id));
      else if (plugin.entry == "streams")
        add(LV_SYMBOL_VIDEO, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kStreams, false, plugin.id, false,
            has_update(plugin.id));
      else if (plugin.entry == "recorder")
        add(LV_SYMBOL_VIDEO, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kRecorder, false, plugin.id, false,
            has_update(plugin.id));
      else if (plugin.entry == "appvault")
        add(LV_SYMBOL_SAVE, plugin.name.c_str(), plugin.description.c_str(),
            kAccent, Action::kAppVault, false, plugin.id, false,
            has_update(plugin.id));
      else if (plugins::IsGeneric(plugin))
        add(GenericPluginIcon(plugin), plugin.name.c_str(),
            plugin.description.c_str(), kAccent, Action::kPluginApp, false,
            plugin.id, false, has_update(plugin.id));
    }
    Navigation(screen, Action::kBackHome, callback, context);
    return;
  }
  auto *apps = Label(screen, "SYSTEM APPS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(apps, 3, 0);
  lv_obj_set_pos(apps, 80, 426);
  auto *files = AppCard(screen, 64, 484, 636, LV_SYMBOL_DIRECTORY, "Files",
                        "Browse storage, preview images and install ZIPs.",
                        kAccent, [=] { callback(Action::kFiles, context); });
  const std::string store_summary = updates.empty()
      ? "Discover signed apps and install them to storage or RAM."
      : i18n::Format("%zu updates available", updates.size());
  auto *store = AppCard(screen, 740, 484, 636, LV_SYMBOL_DOWNLOAD,
                        "Plugin Manager", store_summary.c_str(),
                        kAccent, [=] { callback(Action::kPlugins, context); });
  auto *terminal = AppCard(screen, 64, 884, 1312, LV_SYMBOL_EDIT, "Terminal",
                           "Run recovery commands in the built-in AERA shell.",
                           kAccent, [=] { callback(Action::kTerminal, context); },
                           false, true);
  AnimateEnter(files, 20, 14);
  AnimateEnter(store, 55, 14);
  AnimateEnter(terminal, 80, 14);
  if (!updates.empty()) MarkUpdateAvailable(store);

  auto *extensions = Label(screen, "INSTALLED PLUGINS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(extensions, 3, 0);
  lv_obj_set_pos(extensions, 80, 1320);
  constexpr int kGridWidth = 1312;
  constexpr int kGridHeight = 1050;
  constexpr int kTileHeight = 318;
  constexpr int kGapY = 30;
  const int grid_columns = RecoveryHomeGridColumns();
  const int slots_per_page = grid_columns * 3;
  const int tile_width = grid_columns == 2 ? 636 : 416;
  const int gap_x = grid_columns == 2 ? 40 : 32;
  auto *pager = lv_obj_create(screen);
  lv_obj_set_pos(pager, 64, 1378);
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
  const int page_count = std::max(
      1, static_cast<int>((installed.size() + slots_per_page - 1) /
                          slots_per_page));
  int visible_index = 0;
  lv_obj_t *page = nullptr;
  for (const auto &plugin : installed) {
    Action action = Action::kNone;
    const char *icon = LV_SYMBOL_GPS;
    lv_color_t accent = kAccent;
    if (plugin.entry == "browser") {
      action = Action::kWeb;
      accent = kAccent;
    } else if (plugin.entry == "retroarch") {
      action = Action::kRetroArch;
      icon = LV_SYMBOL_PLAY;
      accent = kAccent;
    } else if (plugin.entry == "doom") {
      action = Action::kDoom;
      icon = LV_SYMBOL_PLAY;
      accent = kAccent;
    } else if (plugin.entry == "telegram") {
      action = Action::kTelegram;
      icon = LV_SYMBOL_GPS;
      accent = kAccent;
    } else if (plugin.entry == "gallery") {
      action = Action::kGallery;
      icon = LV_SYMBOL_IMAGE;
      accent = kAccent;
    } else if (plugin.entry == "media") {
      action = Action::kMedia;
      icon = LV_SYMBOL_PLAY;
      accent = kAccent;
    } else if (plugin.entry == "streams") {
      action = Action::kStreams;
      icon = LV_SYMBOL_VIDEO;
      accent = kAccent;
    } else if (plugin.entry == "recorder") {
      action = Action::kRecorder;
      icon = LV_SYMBOL_VIDEO;
      accent = kAccent;
    } else if (plugin.entry == "appvault") {
      action = Action::kAppVault;
      icon = LV_SYMBOL_SAVE;
      accent = kAccent;
    } else if (plugins::IsGeneric(plugin)) {
      action = Action::kPluginApp;
      icon = GenericPluginIcon(plugin);
      accent = kAccent;
    } else {
      continue;
    }
    const int page_index = visible_index / slots_per_page;
    const int slot = visible_index % slots_per_page;
    if (slot == 0) {
      page = lv_obj_create(pager);
      Clear(page);
      lv_obj_set_pos(page, page_index * kGridWidth, 0);
      lv_obj_set_size(page, kGridWidth, kGridHeight);
      lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
    }
    const int column = slot % grid_columns;
    const int row = slot / grid_columns;
    auto *card = PluginTile(page, column * (tile_width + gap_x),
                            row * (kTileHeight + kGapY),
                            tile_width, kTileHeight, icon, plugin, accent,
                            [=] {
                              if (action == Action::kPluginApp)
                                SetSelectedPluginId(plugin.id);
                              callback(action, context);
                            }, plugin.entry == "retroarch");
    if (has_update(plugin.id))
      MarkUpdateAvailable(card, tile_width < 600);
    AnimateEnter(card, 90 + slot * 18, 10);
    ++visible_index;
  }
  if (installed.empty()) {
    lv_obj_add_flag(pager, LV_OBJ_FLAG_HIDDEN);
    auto *empty = lv_obj_create(screen);
    Panel(empty, 40, kMainSheet);
    lv_obj_set_pos(empty, 64, 1378);
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
    auto *dots = lv_obj_create(screen);
    Clear(dots);
    lv_obj_set_pos(dots, 570, 2450);
    lv_obj_set_size(dots, 300, 42);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 14, 0);
    for (int page_index = 0; page_index < page_count; ++page_index) {
      auto *dot = lv_obj_create(dots);
      Clear(dot);
      lv_obj_set_size(dot, 14, 14);
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(dot, kMainLine, 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_70, 0);
      MakeDecorationPassThrough(dot);
    }
    MakeDecorationPassThrough(dots);
    auto *pager_state =
        new PluginPagerState{pager, dots, page_count, kGridWidth};
    lv_obj_add_event_cb(pager, PluginPagerEvent, LV_EVENT_ALL, pager_state);
    UpdatePluginPageDots(pager_state);
  }
  Navigation(screen, Action::kBackHome, callback, context);
}

}  // namespace recovery_ui2
