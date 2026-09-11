/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <mutex>
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

struct State {
  lv_obj_t *screen = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  PluginScene scene{};
  plugins::Job active_job = plugins::Job::kRefresh;
  bool busy = false;
};

std::mutex gRequestMutex;
plugins::Request gRequest;

void Select(const plugins::Request &request) {
  std::lock_guard<std::mutex> lock(gRequestMutex);
  gRequest = request;
}

const plugins::Plugin *InstalledVersion(
    const std::vector<plugins::Plugin> &installed, const std::string &id) {
  const auto found = std::find_if(installed.begin(), installed.end(),
      [&](const plugins::Plugin &item) { return item.id == id; });
  return found == installed.end() ? nullptr : &*found;
}

void Request(State *state, plugins::Job job, const std::string &id) {
  if (!state || state->busy) return;
  // Do not enter the plugin worker's download/retry path when recovery is
  // plainly offline. Besides giving immediate feedback, this keeps Back,
  // navigation and the rest of AERA responsive instead of appearing frozen
  // while wget waits for DNS/network timeouts.
  const bool network_job = job == plugins::Job::kRefresh ||
      job == plugins::Job::kInstallStorage ||
      job == plugins::Job::kInstallMemory;
  if (network_job && !RecoveryWifiConnection().connected) {
    lv_label_set_text(state->scene.status,
                      "Offline — connect to Wi-Fi before refreshing the store.");
    Sheet(state->screen, "Plugin Store is offline",
          "Connect AERA to Wi-Fi, then try Refresh store again.");
    return;
  }
  Select({job, id});
  state->callback(Action::kRunPluginOperation, state->context);
}

void AddButton(lv_obj_t *parent, const char *text, int x, int width,
               std::function<void()> action, bool accent = false) {
  auto *button = Button(parent, text, std::move(action), accent);
  lv_obj_set_pos(button, x, 238);
  lv_obj_set_size(button, width, 104);
  lv_obj_set_style_radius(button, 30, 0);
}

void Render(State *state) {
  lv_obj_clean(state->scene.list);
  const auto catalog = plugins::Catalog();
  const auto installed = plugins::Installed();
  const int card_width = std::max(600, static_cast<int>(
      lv_obj_get_width(state->scene.list)));
  int y = 0;
  auto *official_heading = Kicker(state->scene.list, "OFFICIAL APPS", kGreen);
  lv_obj_set_pos(official_heading, 8, y + 8);
  y += 64;
  for (const auto &plugin : catalog) {
    const auto *local = InstalledVersion(installed, plugin.id);
    if (local && local->trust != plugins::Trust::kOfficial) local = nullptr;
    auto *card = lv_obj_create(state->scene.list);
    Panel(card, 42, kMainPanel);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, card_width, 370);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, kMainLine, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_40, 0);

    const auto accent = (plugin.id == "browser" || plugin.id == "media")
        ? kCyan : kAccent;
    const char *symbol = plugin.id == "browser" ? LV_SYMBOL_GPS :
        plugin.id == "gallery" ? LV_SYMBOL_IMAGE :
        plugin.id == "media" ? LV_SYMBOL_PLAY : LV_SYMBOL_SETTINGS;
    auto *plate = plugin.id == "retroarch"
        ? RetroArchIconPlate(card, kText, 92)
        : IconPlate(card, symbol, accent, kMainSheet, 92);
    lv_obj_set_pos(plate, 34, 36);
    auto *name = Label(card, plugin.name.c_str(), &lv_font_montserrat_48, kText);
    lv_obj_set_pos(name, 154, 32);
    auto *description = Label(card, plugin.description.c_str(),
                              &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(description, 154, 94);
    lv_obj_set_width(description, card_width - 230);
    const std::string version = "Version " + plugin.version;
    auto *version_label = Label(card, version.c_str(), &lv_font_montserrat_18, kDim);
    lv_obj_set_pos(version_label, 154, 156);

    if (local) {
      auto *badge = Kicker(card,
          (std::string("INSTALLED / ") + plugins::LocationLabel(local->location)).c_str(),
          kGreen);
      lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -34, 40);
      if (local->entry == "browser" || local->entry == "retroarch" ||
          local->entry == "telegram" || local->entry == "gallery" ||
          local->entry == "media" || local->entry == "recorder" ||
          local->entry == "appvault") {
        const auto action = local->entry == "browser" ? Action::kWeb :
            local->entry == "retroarch" ? Action::kRetroArch :
            local->entry == "telegram" ? Action::kTelegram :
            local->entry == "gallery" ? Action::kGallery :
            local->entry == "media" ? Action::kMedia :
            local->entry == "recorder" ? Action::kRecorder : Action::kAppVault;
        const int gap = 24;
        const int third = (card_width - 68 - gap * 2) / 3;
        AddButton(card, "Open", 34, third,
                  [state, action] { state->callback(action, state->context); }, true);
        const bool update = local->version != plugin.version;
        AddButton(card, update ? "Update on storage" : "Reinstall",
                  34 + third + gap, third,
                  [state, id = plugin.id] {
                    Request(state, plugins::Job::kInstallStorage, id);
                  });
        AddButton(card, "Remove", 34 + (third + gap) * 2, third,
                  [state, id = plugin.id] {
                    Request(state, plugins::Job::kRemove, id);
                  });
      } else {
        const bool update = local->version != plugin.version;
        const int gap = 24;
        const int half = (card_width - 68 - gap) / 2;
        AddButton(card, update ? "Update on storage" : "Reinstall", 34,
                  half, [state, id = plugin.id] {
                    Request(state, plugins::Job::kInstallStorage, id);
                  });
        AddButton(card, "Remove", 34 + half + gap, half,
                  [state, id = plugin.id] {
                    Request(state, plugins::Job::kRemove, id);
                  });
      }
    } else {
      auto *badge = Kicker(card, "AVAILABLE", kAccent);
      lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -34, 40);
      const int gap = 24;
      const int half = (card_width - 68 - gap) / 2;
      AddButton(card, "Install on storage", 34, half, [state, id = plugin.id] {
        Request(state, plugins::Job::kInstallStorage, id);
      }, true);
      AddButton(card, "Load into RAM", 34 + half + gap, half, [state, id = plugin.id] {
        Request(state, plugins::Job::kInstallMemory, id);
      });
    }
    AnimateEnter(card, 30 + static_cast<uint32_t>(y / 12), 12);
    y += 394;
  }
  std::vector<plugins::Plugin> unofficial;
  std::copy_if(installed.begin(), installed.end(),
               std::back_inserter(unofficial), [](const plugins::Plugin &plugin) {
    return plugin.trust == plugins::Trust::kUnofficial;
  });
  if (!unofficial.empty()) {
    y += 36;
    auto *heading = Kicker(state->scene.list, "UNOFFICIAL APPS", kAmber);
    lv_obj_set_pos(heading, 8, y + 8);
    y += 64;
    for (const auto &plugin : unofficial) {
      auto *card = lv_obj_create(state->scene.list);
      Panel(card, 42, kMainPanel);
      lv_obj_set_pos(card, 0, y);
      lv_obj_set_size(card, card_width, 370);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_color(card, kAmber, 0);
      lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
      const char *symbol = plugin.entry == "browser" ? LV_SYMBOL_GPS :
          plugin.entry == "gallery" ? LV_SYMBOL_IMAGE :
          plugin.entry == "media" ? LV_SYMBOL_PLAY : LV_SYMBOL_SETTINGS;
      auto *plate = plugin.entry == "retroarch"
          ? RetroArchIconPlate(card, kText, 92)
          : IconPlate(card, symbol, kAmber, kMainSheet, 92);
      lv_obj_set_pos(plate, 34, 36);
      auto *name = Label(card, plugin.name.c_str(), &lv_font_montserrat_48, kText);
      lv_obj_set_pos(name, 154, 32);
      auto *description = Label(card, plugin.description.c_str(),
                                &lv_font_montserrat_24, kMuted);
      lv_obj_set_pos(description, 154, 94);
      lv_obj_set_width(description, card_width - 230);
      const std::string version = "Version " + plugin.version +
          "  /  Installed from a local package";
      auto *version_label = Label(card, version.c_str(),
                                  &lv_font_montserrat_18, kDim);
      lv_obj_set_pos(version_label, 154, 156);
      auto *badge = Kicker(card, "UNVERIFIED", kAmber);
      lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -34, 40);

      Action action = Action::kNone;
      if (plugin.entry == "browser") action = Action::kWeb;
      else if (plugin.entry == "retroarch") action = Action::kRetroArch;
      else if (plugin.entry == "telegram") action = Action::kTelegram;
      else if (plugin.entry == "gallery") action = Action::kGallery;
      else if (plugin.entry == "media") action = Action::kMedia;
      else if (plugin.entry == "recorder") action = Action::kRecorder;
      else if (plugin.entry == "appvault") action = Action::kAppVault;
      const int gap = 24;
      const int half = (card_width - 68 - gap) / 2;
      AddButton(card, "Open", 34, half, [state, action] {
        if (action != Action::kNone) state->callback(action, state->context);
      });
      AddButton(card, "Remove", 34 + half + gap, half,
                [state, id = plugin.id] {
        Request(state, plugins::Job::kRemove, id);
      });
      AnimateEnter(card, 30 + static_cast<uint32_t>(y / 12), 12);
      y += 394;
    }
  }
  if (catalog.empty()) {
    auto *empty = Label(state->scene.list,
        "No plugins are currently published in the signed catalog.",
        &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 160);
  }
}

}  // namespace

plugins::Request GetPluginRequest() {
  std::lock_guard<std::mutex> lock(gRequestMutex);
  return gRequest;
}

void SetPluginRequest(const plugins::Request &request) {
  Select(request);
}

PluginScene BuildPluginScene(lv_obj_t *screen, ActionCallback callback,
                             void *context) {
  auto *state = new State;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  state->scene.state = state;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<State *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, state);

  Header(screen, "Plugin Manager", "Signed apps and extensions for AERA.",
         callback, context);
  const bool landscape = Landscape(screen);
  auto *official = Kicker(screen, "OFFICIAL AERA STORE", kGreen);
  lv_obj_set_pos(official, 80, landscape ? 306 : 426);
  state->scene.status = Label(screen, "Ready", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->scene.status, 80, landscape ? 358 : 488);
  auto *refresh = Button(screen, LV_SYMBOL_REFRESH "  Refresh store", [state] {
    Request(state, plugins::Job::kRefresh, "");
  });
  lv_obj_set_pos(refresh, landscape ? 2724 : 996,
                 landscape ? 292 : 414);
  lv_obj_set_size(refresh, 380, 116);
  lv_obj_set_style_radius(refresh, 34, 0);
  state->scene.refresh = refresh;
  state->scene.progress = lv_bar_create(screen);
  lv_obj_set_pos(state->scene.progress, 80, landscape ? 426 : 560);
  lv_obj_set_size(state->scene.progress,
                  landscape ? lv_obj_get_width(screen) - 160 : 1296, 12);
  lv_bar_set_range(state->scene.progress, 0, 100);
  lv_bar_set_value(state->scene.progress, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(state->scene.progress, kMainPanel, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->scene.progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(state->scene.progress, LV_OBJ_FLAG_HIDDEN);
  state->scene.list = Scroll(screen, landscape ? 466 : 612,
                             landscape ? 974 : 2556);
  lv_obj_set_style_pad_bottom(state->scene.list, landscape ? 190 : 242, 0);
  Navigation(screen, Action::kNone, callback, context);
  Render(state);
  return state->scene;
}

void SetPluginBusy(const PluginScene &scene, const plugins::Request &request) {
  auto *state = static_cast<State *>(scene.state);
  if (!state) return;
  state->busy = true;
  state->active_job = request.job;
  lv_obj_add_state(scene.refresh, LV_STATE_DISABLED);
  lv_obj_remove_flag(scene.progress, LV_OBJ_FLAG_HIDDEN);
  lv_bar_set_value(scene.progress, 8, LV_ANIM_ON);
  const char *text = request.job == plugins::Job::kRefresh ? "Refreshing signed store..." :
      request.job == plugins::Job::kRemove ? "Removing plugin..." :
      request.job == plugins::Job::kInstallLocalMemory ? "Loading local plugin into RAM..." :
      request.job == plugins::Job::kInstallLocalStorage ? "Installing local plugin..." :
      request.job == plugins::Job::kInstallMemory ? "Loading plugin into RAM..." :
      "Installing plugin on storage...";
  lv_label_set_text(scene.status, text);
}

void UpdatePluginProgress(const PluginScene &scene, unsigned value,
                          uint64_t downloaded_bytes, uint64_t total_bytes) {
  auto *state = static_cast<State *>(scene.state);
  value = std::min(value, 100U);
  if (scene.progress) lv_bar_set_value(scene.progress, value, LV_ANIM_ON);
  if (!state || !scene.status || !state->busy) return;

  char message[160];
  if (total_bytes && value <= 78) {
    constexpr double kMiB = 1024.0 * 1024.0;
    const unsigned download_percent = static_cast<unsigned>(
        std::min<uint64_t>(downloaded_bytes, total_bytes) * 100 / total_bytes);
    std::snprintf(message, sizeof(message),
        "Downloading AERA Browser  %.1f / %.1f MB  -  %u%%",
        downloaded_bytes / kMiB, total_bytes / kMiB, download_percent);
  } else if (total_bytes && value < 100) {
    std::snprintf(message, sizeof(message),
                  "Verifying and installing  -  %u%%", value);
  } else if (state->active_job == plugins::Job::kRefresh) {
    std::snprintf(message, sizeof(message),
                  "Refreshing signed store  -  %u%%", value);
  } else {
    std::snprintf(message, sizeof(message), "Preparing download  -  %u%%", value);
  }
  lv_label_set_text(scene.status, message);
}

void CompletePluginOperation(const PluginScene &scene, bool success,
                             const char *message) {
  auto *state = static_cast<State *>(scene.state);
  if (!state) return;
  state->busy = false;
  lv_obj_remove_state(scene.refresh, LV_STATE_DISABLED);
  lv_bar_set_value(scene.progress, success ? 100 : 0, LV_ANIM_ON);
  lv_label_set_text(scene.status, message && *message ? message :
                    (success ? "Plugin operation completed." : "Plugin operation failed."));
  Render(state);
}

}  // namespace recovery_ui2
