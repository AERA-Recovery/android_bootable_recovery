/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <cutils/sockets.h>

#include "design.hpp"
#include "ui_components.hpp"

namespace recovery_ui2 {
namespace {
using namespace design;
using namespace widgets;

struct TelemetryState {
  std::string phase = "idle";
  std::string target;
  uint64_t current = 0;
  uint64_t total = 0;
  int result = -1;
  uint32_t updated_ms = 0;
  bool dirty = true;
};

struct TelemetryView {
  lv_obj_t *screen = nullptr;
  lv_obj_t *pulse = nullptr;
  lv_obj_t *icon = nullptr;
  lv_obj_t *title = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *percent = nullptr;
};

TelemetryState g_telemetry;
TelemetryView g_view;
int g_server_fd = -1;
int g_client_fd = -1;
std::string g_input;

std::string Bytes(uint64_t value) {
  char result[48];
  if (value >= 1024ULL * 1024ULL * 1024ULL) {
    snprintf(result, sizeof(result), "%.2f GiB",
             static_cast<double>(value) / (1024.0 * 1024.0 * 1024.0));
  } else if (value >= 1024ULL * 1024ULL) {
    snprintf(result, sizeof(result), "%.1f MiB",
             static_cast<double>(value) / (1024.0 * 1024.0));
  } else {
    snprintf(result, sizeof(result), "%.1f KiB",
             static_cast<double>(value) / 1024.0);
  }
  return result;
}

void ParseTelemetry(const std::string &line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (start <= line.size()) {
    const size_t end = line.find('|', start);
    fields.emplace_back(line.substr(start, end == std::string::npos
                                               ? std::string::npos
                                               : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (fields.size() != 6 || fields[0] != "AERA1") return;
  char *end = nullptr;
  const uint64_t current = strtoull(fields[2].c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return;
  const uint64_t total = strtoull(fields[3].c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return;
  const long result = strtol(fields[4].c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || result < -1 || result > 1) return;
  g_telemetry.phase = fields[1];
  g_telemetry.target = fields[5];
  g_telemetry.current = current;
  g_telemetry.total = total;
  g_telemetry.result = static_cast<int>(result);
  g_telemetry.updated_ms = lv_tick_get();
  g_telemetry.dirty = true;
}

void PollSocket() {
  if (g_server_fd < 0) {
    g_server_fd = android_get_control_socket("recovery");
    if (g_server_fd < 0) return;
    const int flags = fcntl(g_server_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(g_server_fd, F_SETFL, flags | O_NONBLOCK);
    if (listen(g_server_fd, 4) != 0) {
      g_server_fd = -1;
      return;
    }
  }

  for (;;) {
    const int connection = accept(g_server_fd, nullptr, nullptr);
    if (connection < 0) break;
    if (g_client_fd >= 0) close(g_client_fd);
    g_client_fd = connection;
    const int flags = fcntl(g_client_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(g_client_fd, F_SETFL, flags | O_NONBLOCK);
    g_input.clear();
  }
  if (g_client_fd < 0) return;

  char buffer[512];
  for (;;) {
    const ssize_t count = recv(g_client_fd, buffer, sizeof(buffer), 0);
    if (count > 0) {
      g_input.append(buffer, static_cast<size_t>(count));
      while (true) {
        const size_t newline = g_input.find('\n');
        if (newline == std::string::npos) break;
        ParseTelemetry(g_input.substr(0, newline));
        g_input.erase(0, newline + 1);
      }
      if (g_input.size() > 4096) g_input.clear();
      continue;
    }
    if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      close(g_client_fd);
      g_client_fd = -1;
      g_input.clear();
    }
    break;
  }
}

void ApplyTelemetry() {
  if (!g_telemetry.dirty || g_view.screen == nullptr ||
      !lv_obj_is_valid(g_view.screen)) return;
  g_telemetry.dirty = false;

  std::string title = "Ready for fastboot commands";
  std::string detail =
      "Use the fastboot client on your computer to flash, erase, resize or\n"
      "inspect dynamic partitions. Keep the USB cable connected during writes.";
  const char *icon = LV_SYMBOL_USB;
  lv_color_t color = kAccent;
  bool show_progress = false;

  const bool failed = g_telemetry.result == 1;
  const bool finished = g_telemetry.result == 0;
  const std::string target = g_telemetry.target.empty()
      ? std::string("partition") : g_telemetry.target;
  if (failed) {
    title = g_telemetry.target.empty() ? "Transfer failed"
                                       : "Operation failed · " + target;
    detail = "Fastbootd reported an error. Check the host output before retrying.";
    icon = LV_SYMBOL_CLOSE;
    color = kRed;
  } else if (g_telemetry.phase == "receiving") {
    title = "Receiving image";
    detail = g_telemetry.total == 0
        ? "Receiving the image over USB. The target partition follows next."
        : Bytes(g_telemetry.current) + " of " + Bytes(g_telemetry.total) +
              " received over USB. The target partition follows next.";
    icon = LV_SYMBOL_DOWNLOAD;
    show_progress = g_telemetry.total != 0;
  } else if (g_telemetry.phase == "received") {
    title = "Image received";
    detail = Bytes(g_telemetry.total) +
        " received. Waiting for the host to name the target partition.";
    icon = LV_SYMBOL_OK;
    color = kGreen;
  } else if (g_telemetry.phase == "flashing") {
    title = (finished ? "Flashed " : "Flashing ") + target;
    detail = finished
        ? "The partition was written successfully."
        : "Writing " + target + ". Keep the USB cable connected.";
    icon = finished ? LV_SYMBOL_OK : LV_SYMBOL_SAVE;
    color = finished ? kGreen : kAccent;
    show_progress = g_telemetry.total != 0;
  } else if (g_telemetry.phase == "erasing") {
    title = (finished ? "Erased " : "Erasing ") + target;
    detail = finished ? "The partition was erased successfully."
                      : "Erasing " + target + ". Keep the USB cable connected.";
    icon = finished ? LV_SYMBOL_OK : LV_SYMBOL_TRASH;
    color = finished ? kGreen : kAccent;
  } else if (g_telemetry.phase == "updating") {
    title = (finished ? "Updated " : "Updating ") + target;
    detail = finished ? "Dynamic partition metadata was updated successfully."
                      : "Applying dynamic partition metadata. Do not disconnect USB.";
    icon = finished ? LV_SYMBOL_OK : LV_SYMBOL_REFRESH;
    color = finished ? kGreen : kAccent;
  }

  lv_label_set_text(g_view.title, title.c_str());
  lv_label_set_text(g_view.detail, detail.c_str());
  lv_label_set_text(g_view.icon, icon);
  lv_obj_set_style_text_color(g_view.icon, color, 0);
  lv_obj_set_style_border_color(g_view.pulse, color, 0);
  if (show_progress) {
    const int percent = static_cast<int>(std::min<uint64_t>(
        100, g_telemetry.current * 100 / std::max<uint64_t>(1, g_telemetry.total)));
    lv_bar_set_value(g_view.progress, percent, LV_ANIM_ON);
    char value[8];
    snprintf(value, sizeof(value), "%d%%", percent);
    lv_label_set_text(g_view.percent, value);
    lv_obj_remove_flag(g_view.progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_view.percent, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_view.progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_view.percent, LV_OBJ_FLAG_HIDDEN);
  }
}

void Pulse(void *object, int32_t value) {
  auto *target = static_cast<lv_obj_t *>(object);
  lv_obj_set_style_transform_scale(target, value, 0);
  lv_obj_set_style_opa(target,
      static_cast<lv_opa_t>(std::clamp(360 - value / 2, 80, 230)), 0);
}

}  // namespace

void PollFastbootTelemetry() {
  PollSocket();
  if (g_telemetry.result >= 0 &&
      lv_tick_elaps(g_telemetry.updated_ms) > 2400) {
    g_telemetry = TelemetryState{};
  }
  ApplyTelemetry();
}

void BuildFastbootScene(lv_obj_t *screen, ActionCallback callback,
                        void *context) {
  const bool landscape = Landscape(screen);
  MainBackground(screen);
  // Fastbootd owns USB. Keep the normal pull-down controls disabled so Wi-Fi,
  // rotation and recovery jobs cannot be started in this restricted mode.
  AttachStatusBar(screen, nullptr, nullptr, StatusBarAction::kNone, true);

  auto *mode = Kicker(screen, "USERSPACE FASTBOOT", kAccent);
  lv_obj_set_pos(mode, 80, landscape ? 210 : 252);
  auto *title = Label(screen, "Fastbootd", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 80, landscape ? 264 : 314);

  auto *hero = lv_obj_create(screen);
  Panel(hero, 52, kMainSheet);
  lv_obj_set_pos(hero, 64, landscape ? 330 : 430);
  lv_obj_set_size(hero, landscape ? 1400 : 1312,
                  landscape ? 760 : 1160);
  lv_obj_set_style_border_width(hero, 1, 0);
  lv_obj_set_style_border_color(hero, kMainLine, 0);
  lv_obj_set_style_border_opa(hero, LV_OPA_50, 0);

  auto *pulse = lv_obj_create(hero);
  Clear(pulse);
  lv_obj_set_size(pulse, 300, 300);
  lv_obj_align(pulse, LV_ALIGN_TOP_MID, 0, landscape ? 90 : 160);
  lv_obj_set_style_radius(pulse, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(pulse, kAccentSoft, 0);
  lv_obj_set_style_bg_opa(pulse, LV_OPA_60, 0);
  lv_obj_set_style_border_width(pulse, 3, 0);
  lv_obj_set_style_border_color(pulse, kAccent, 0);
  lv_obj_set_style_border_opa(pulse, LV_OPA_40, 0);
  auto *usb = Label(pulse, LV_SYMBOL_USB, &lv_font_montserrat_48, kAccent);
  lv_obj_center(usb);

  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, pulse);
  lv_anim_set_values(&animation, 244, 262);
  lv_anim_set_duration(&animation, 1050);
  lv_anim_set_playback_duration(&animation, 1050);
  lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&animation, Pulse);
  lv_anim_start(&animation);

  auto *ready = Label(hero, "Ready for fastboot commands",
                      &lv_font_montserrat_48, kText);
  lv_obj_align(ready, LV_ALIGN_TOP_MID, 0, landscape ? 440 : 540);
  auto *detail = Label(
      hero,
      "Use the fastboot client on your computer to flash, erase, resize or\n"
      "inspect dynamic partitions. Keep the USB cable connected during writes.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_width(detail, landscape ? 1120 : 1160);
  lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, landscape ? 520 : 630);

  auto *progress = lv_bar_create(hero);
  lv_obj_set_size(progress, landscape ? 1080 : 1120, 24);
  lv_obj_align(progress, LV_ALIGN_TOP_MID, 0, landscape ? 628 : 770);
  lv_obj_set_style_radius(progress, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(progress, kInset, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress, kAccent, LV_PART_INDICATOR);
  lv_obj_set_style_radius(progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_bar_set_value(progress, 0, LV_ANIM_OFF);
  lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
  auto *percent = Label(hero, "0%", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_align(percent, LV_ALIGN_TOP_MID, 0, landscape ? 666 : 812);
  lv_obj_add_flag(percent, LV_OBJ_FLAG_HIDDEN);

  g_view = {screen, pulse, usb, ready, detail, progress, percent};
  g_telemetry.dirty = true;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    if (g_view.screen == lv_event_get_target_obj(event)) g_view = {};
  }, LV_EVENT_DELETE, nullptr);
  PollFastbootTelemetry();

  const std::string slot = RecoverySlot();
  auto *transport = Kicker(hero, "USB TRANSPORT  ACTIVE", kGreen);
  lv_obj_align(transport, LV_ALIGN_BOTTOM_LEFT, 58, -54);
  auto *slot_label = Kicker(
      hero, ("ACTIVE SLOT  " + (slot.empty() ? std::string("—") : slot)).c_str(),
      kMutedStrong);
  lv_obj_align(slot_label, LV_ALIGN_BOTTOM_RIGHT, -58, -54);

  auto *actions = lv_obj_create(screen);
  Clear(actions);
  lv_obj_set_pos(actions, landscape ? 1510 : 64,
                 landscape ? 330 : 1640);
  lv_obj_set_size(actions, landscape ? 1594 : 1312,
                  landscape ? 1050 : 1300);

  struct Destination {
    const char *icon;
    const char *name;
    const char *detail;
    Action action;
  };
  constexpr std::array<Destination, 5> destinations{{
      {LV_SYMBOL_HOME, "Android", "Boot the operating system",
       Action::kRebootSystem},
      {LV_SYMBOL_REFRESH, "Recovery", "Return to full AERA recovery",
       Action::kRebootRecovery},
      {LV_SYMBOL_SETTINGS, "Bootloader", "Open hardware fastboot",
       Action::kRebootBootloader},
      {LV_SYMBOL_POWER, "Power off", "Shut down the device",
       Action::kPowerOff},
      {LV_SYMBOL_TRASH, "Format Data", "Erase internal storage and encryption",
       Action::kFormatData},
  }};
  for (size_t index = 0; index < destinations.size(); ++index) {
    const auto destination = destinations[index];
    const bool format = destination.action == Action::kFormatData;
    const int column = static_cast<int>(index % 2);
    const int row = static_cast<int>(index / 2);
    const int gap = landscape ? 26 : 24;
    const int card_width = format ? (landscape ? 1594 : 1312)
                                  : (landscape ? 784 : 644);
    const int card_height = format ? (landscape ? 220 : 250)
                                   : (landscape ? 232 : 270);
    auto *card = lv_button_create(actions);
    Panel(card, 38, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_size(card, card_width, card_height);
    if (format) {
      lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -30);
    } else {
      lv_obj_set_pos(card, column * (card_width + gap),
                     row * (card_height + (landscape ? 26 : 24)));
    }
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, kMainLine, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
    OnClick(card, [screen, callback, context, destination] {
      if (destination.action == Action::kFormatData) {
        callback(destination.action, context);
        return;
      }
      const std::string title = destination.action == Action::kPowerOff
          ? "Power off device?"
          : std::string("Reboot to ") + destination.name + "?";
      const std::string detail = std::string(destination.detail) +
          ".\n\nSwipe only when you are ready to leave Fastbootd.";
      Sheet(screen, title, detail, [callback, context, destination] {
        callback(destination.action, context);
      });
    });
    auto *icon = Label(card, destination.icon, &lv_font_montserrat_48,
        kAccent);
    lv_obj_set_pos(icon, 38, format ? 70 : (landscape ? 32 : 32));
    auto *name = Label(card, destination.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 122, format ? 52 : (landscape ? 40 : 40));
    auto *copy = Label(card, destination.detail, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(copy, format ? 122 : 38,
                   format ? 118 : (landscape ? 126 : 126));
    lv_obj_set_width(copy, card_width - (format ? 160 : 76));
    AnimateEnter(card, 50 + static_cast<uint32_t>(index) * 35, 12);
  }

  auto *warning = Label(screen, "Do not disconnect USB while a command is writing data.",
                        &lv_font_montserrat_24, kAmber);
  lv_obj_align(warning, LV_ALIGN_BOTTOM_MID, 0, landscape ? -38 : -180);
}

}  // namespace recovery_ui2
