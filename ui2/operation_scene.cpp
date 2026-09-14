/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "ui_components.hpp"

namespace recovery_ui2 {
namespace {
using namespace widgets;

JobRequest gRequest;

struct FriendlyProgress {
  std::string title;
  std::string explanation;
  std::string amount;
  std::string files;
  std::string activity;
  int step = 0;
};

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::vector<std::string> Lines(const std::string &text) {
  std::vector<std::string> result;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t end = text.find('\n', begin);
    auto line = text.substr(begin, end == std::string::npos ? end : end - begin);
    if (!line.empty()) result.push_back(std::move(line));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
}

std::string CleanMetric(std::string value) {
  if (!value.empty() && value.back() == ',') value.pop_back();
  for (const char *unit : {"MB", "GB", "KB"}) {
    size_t pos = 0;
    while ((pos = value.find(unit, pos)) != std::string::npos) {
      if (pos > 0 && std::isdigit(static_cast<unsigned char>(value[pos - 1]))) {
        value.insert(pos, " ");
        ++pos;
      }
      pos += 2;
    }
  }
  return value;
}

std::string CleanInstallerStatus(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  if (!value.empty() && value.front() == '-') {
    value.erase(value.begin());
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())))
      value.erase(value.begin());
  }
  return value;
}

std::string CleanInstallerHistory(const std::string &value,
                                  size_t visible_lines) {
  const auto lines = Lines(value);
  std::string history;
  const size_t first = lines.size() > visible_lines
      ? lines.size() - visible_lines : 0;
  for (size_t i = first; i < lines.size(); ++i) {
    auto line = lines[i];
    line = CleanInstallerStatus(std::move(line));
    if (line.empty()) continue;
    if (!history.empty()) history += '\n';
    history += line;
  }
  return history;
}

const char *InitialTitle(Job job) {
  switch (job) {
    case Job::kFlashImage: return "Preparing image flash";
    case Job::kBackup: return "Preparing your backup";
    case Job::kRestore: return "Preparing to restore";
    case Job::kWipe: return "Preparing to wipe";
    case Job::kFormatData: return "Preparing data format";
    case Job::kMount: return "Mounting storage";
    case Job::kUnmount: return "Unmounting storage";
    default: return "Preparing installation";
  }
}

const char *OperationSymbol(Job job) {
  switch (job) {
    case Job::kFlashImage: return LV_SYMBOL_UPLOAD;
    case Job::kBackup: return LV_SYMBOL_SAVE;
    case Job::kRestore: return LV_SYMBOL_REFRESH;
    case Job::kWipe:
    case Job::kFormatData: return LV_SYMBOL_TRASH;
    case Job::kMount:
    case Job::kUnmount: return LV_SYMBOL_DRIVE;
    default: return LV_SYMBOL_DOWNLOAD;
  }
}

void SetActivityBorderOpacity(void *target, int32_t opacity) {
  lv_obj_set_style_border_opa(static_cast<lv_obj_t *>(target), opacity, 0);
}

void SetProgressPulseOpacity(void *target, int32_t opacity) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(target),
                       static_cast<lv_opa_t>(opacity), 0);
}

void SetIndeterminateProgressX(void *target, int32_t x) {
  lv_obj_set_x(static_cast<lv_obj_t *>(target), x);
}

FriendlyProgress Explain(const std::string &raw, Job job, int progress) {
  FriendlyProgress value;
  const auto lines = Lines(raw);
  const std::string headline = lines.empty() ? "" : lines[0];
  const std::string all = Lower(raw);
  std::string subject;
  const size_t slash = headline.find(" / ");
  if (slash != std::string::npos) subject = headline.substr(slash + 3);
  if (subject.empty()) subject = "selected partitions";
  if (lines.size() > 1) value.amount = CleanMetric(lines[1]);
  if (lines.size() > 2) value.files = CleanMetric(lines[2]);

  if (all.find("nas upload") != std::string::npos ||
      all.find("uploading") != std::string::npos) {
    value.title = "Uploading to Network Storage";
    value.explanation = "Your local backup is ready. AERA is securely transferring it to your network storage.";
    value.activity = "Keep Wi-Fi connected and leave the device powered on until the upload completes.";
    value.step = 3;
  } else if (all.find("digest") != std::string::npos ||
             all.find("checksum") != std::string::npos ||
             all.find("verify") != std::string::npos) {
    value.title = "Verifying the backup";
    value.explanation = "AERA is checking the backup so it can be restored reliably later.";
    value.activity = "Reading the completed archive and validating its integrity.";
    value.step = 2;
  } else if (job == Job::kBackup || all.find("backing") != std::string::npos) {
    value.title = i18n::Format("Backing up %s", subject.c_str());
    value.explanation = "AERA is safely copying the selected data into your new backup.";
    value.activity = i18n::Format(
        "Copying files from %s. Large partitions can take a few minutes.",
        subject.c_str());
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kRestore || all.find("restor") != std::string::npos) {
    value.title = i18n::Format("Restoring %s", subject.c_str());
    value.explanation = "AERA is writing the selected backup data back to the device.";
    value.activity = i18n::Format(
        "Restoring files for %s. Do not reboot during this operation.",
        subject.c_str());
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kFormatData || all.find("format") != std::string::npos) {
    value.title = "Formatting data";
    value.explanation = "AERA is recreating the data volume and internal storage.";
    value.activity = "Removing encryption metadata and preparing a clean data volume.";
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kWipe || all.find("wip") != std::string::npos) {
    value.title = i18n::Format("Wiping %s", subject.c_str());
    value.explanation = "AERA is clearing the selected partition safely.";
    value.activity = i18n::Format(
        "Cleaning %s and preparing it for use.", subject.c_str());
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kFlashImage) {
    value.title = progress > 1 ? "Flashing partition image" :
                                 "Preparing image flash";
    value.explanation = i18n::Format(
        "AERA is writing the selected image directly to %s.",
        subject.c_str());
    value.activity = "Do not reboot or disconnect the device while the image is being written.";
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kInstall) {
    value.title = progress > 1 ? "Installing package" : "Preparing installation";
    value.explanation = "AERA is applying the selected package to your device.";
    value.activity = "Processing the package and updating the required partitions.";
    value.step = progress > 1 ? 1 : 0;
  } else {
    value.title = InitialTitle(job);
    value.explanation = "AERA is completing the requested recovery operation.";
    value.activity = headline.empty() ? "Waiting for the recovery backend to begin." : headline;
    value.step = progress > 1 ? 1 : 0;
  }
  return value;
}

void ShowTechnicalLog(lv_obj_t *screen) {
  auto *overlay = lv_obj_create(screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);

  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 44, kMainSheet);
  const bool landscape = Landscape(screen);
  lv_obj_set_size(sheet, landscape ? 2200 : 1312,
                  landscape ? 1250 : 2550);
  lv_obj_align(sheet, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_30, 0);

  auto *title = Label(sheet, "Technical details", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 48, 48);
  auto *copy = Label(sheet, "Raw recovery output for troubleshooting", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 48, 118);

  auto *area = lv_obj_create(sheet);
  Clear(area);
  lv_obj_set_pos(area, 48, 190);
  lv_obj_set_size(area, landscape ? 2104 : 1216,
                  landscape ? 820 : 2110);
  lv_obj_add_flag(area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(area, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(area, kInset, 0);
  lv_obj_set_style_bg_opa(area, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(area, 24, 0);
  lv_obj_set_style_pad_all(area, 32, 0);
  lv_obj_set_style_bg_color(area, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(area, 5, LV_PART_SCROLLBAR);
  auto *log = Label(area, ReadLog().c_str(), &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_width(log, landscape ? 2010 : 1120);
  lv_obj_set_style_text_line_space(log, 8, 0);

  auto *close = Button(sheet, "Close", [overlay] { lv_obj_delete_async(overlay); }, true);
  lv_obj_set_pos(close, 48, landscape ? 1060 : 2342);
  lv_obj_set_size(close, landscape ? 2104 : 1216, 136);
  AnimateEnter(sheet, 0, 36);
}

}  // namespace

void SetJobRequest(const JobRequest &request) { gRequest = request; }
JobRequest GetJobRequest() { return gRequest; }

OperationScene BuildJobScene(lv_obj_t *screen, const JobRequest &request,
                             ActionCallback callback, void *context) {
  Header(screen, request.title.c_str(), "Progress & activity", nullptr, nullptr);
  OperationScene result;
  result.job = request.job;
  result.format_data = request.job == Job::kFormatData;
  result.indeterminate_progress = request.job == Job::kInstall;
  result.started = lv_tick_get();
  const bool landscape = Landscape(screen);

  auto *card = lv_obj_create(screen);
  Panel(card, 40, kMainSheet);
  lv_obj_set_pos(card, 64, landscape ? 340 : 430);
  lv_obj_set_size(card, landscape ? 1450 : 1312,
                  landscape ? 850 : 756);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);

  // A calm status beacon replaces the continuously rotating spinner. The
  // icon remains stable while only its thin outline breathes very slowly.
  result.activity = lv_obj_create(card);
  Panel(result.activity, LV_RADIUS_CIRCLE, kAccentSoft);
  lv_obj_set_pos(result.activity, 48, 46);
  lv_obj_set_size(result.activity, 124, 124);
  lv_obj_set_style_border_width(result.activity, 3, 0);
  lv_obj_set_style_border_color(result.activity, kAccent, 0);
  lv_obj_set_style_border_opa(result.activity, LV_OPA_30, 0);
  auto *activity_icon = Label(result.activity, OperationSymbol(request.job),
                              &lv_font_montserrat_48, kAccent);
  lv_obj_center(activity_icon);
  lv_anim_t breathe;
  lv_anim_init(&breathe);
  lv_anim_set_var(&breathe, result.activity);
  lv_anim_set_values(&breathe, LV_OPA_20, LV_OPA_60);
  lv_anim_set_duration(&breathe, 1450);
  lv_anim_set_playback_duration(&breathe, 1450);
  lv_anim_set_repeat_count(&breathe, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&breathe, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&breathe, SetActivityBorderOpacity);
  lv_anim_start(&breathe);

  result.status = Label(card, InitialTitle(request.job), &lv_font_montserrat_48, kText);
  lv_obj_set_pos(result.status, 208, 42);
  lv_obj_set_width(result.status, 1010);
  lv_label_set_long_mode(result.status, LV_LABEL_LONG_DOT);
  result.detail = Label(card, "Waiting for the recovery backend to begin.", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(result.detail, 208, 112);
  lv_obj_set_width(result.detail, 1010);

  result.percent = Label(card, "0%", &lv_font_montserrat_48, kAccent);
  lv_obj_set_pos(result.percent, 48, 222);
  result.elapsed = Label(card, "0:00 elapsed", &lv_font_montserrat_24, kMuted);
  lv_obj_align(result.elapsed, LV_ALIGN_TOP_RIGHT, -48, 246);

  result.progress = lv_bar_create(card);
  lv_obj_set_pos(result.progress, 48, 310);
  lv_obj_set_size(result.progress, 1216, 32);
  lv_bar_set_range(result.progress, 0, 100);
  lv_obj_set_style_anim_duration(result.progress, 450, 0);
  lv_bar_set_value(result.progress, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(result.progress, 16, LV_PART_MAIN);
  lv_obj_set_style_radius(result.progress, 16, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(result.progress, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_bg_color(result.progress, kAccent, LV_PART_INDICATOR);

  const bool transfer =
      request.job == Job::kBackup || request.job == Job::kRestore;
  if (transfer || result.indeterminate_progress) {
    result.progress_pulse = lv_obj_create(card);
    Clear(result.progress_pulse);
    lv_obj_set_pos(result.progress_pulse, 48, 314);
    lv_obj_set_size(result.progress_pulse,
                    result.indeterminate_progress ? 240 : 24, 24);
    lv_obj_set_style_radius(result.progress_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(result.progress_pulse, kAccent, 0);
    lv_obj_set_style_bg_opa(result.progress_pulse, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(result.progress_pulse, kAccent, 0);
    lv_obj_set_style_shadow_width(result.progress_pulse, 18, 0);
    lv_obj_set_style_shadow_opa(result.progress_pulse, LV_OPA_40, 0);
    lv_obj_remove_flag(result.progress_pulse, LV_OBJ_FLAG_CLICKABLE);
    if (!result.indeterminate_progress)
      lv_obj_add_flag(result.progress_pulse, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, result.progress_pulse);
    lv_anim_set_values(&pulse,
        result.indeterminate_progress ? 48 : LV_OPA_50,
        result.indeterminate_progress ? 1024 : LV_OPA_COVER);
    lv_anim_set_duration(&pulse, result.indeterminate_progress ? 1100 : 760);
    lv_anim_set_playback_duration(&pulse,
                                  result.indeterminate_progress ? 1100 : 760);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&pulse, result.indeterminate_progress
        ? SetIndeterminateProgressX : SetProgressPulseOpacity);
    lv_anim_start(&pulse);
  }

  auto *stats = lv_obj_create(card);
  Panel(stats, 26, kMainPanel);
  lv_obj_set_pos(stats, 48, 390);
  lv_obj_set_size(stats, 1216, 180);
  auto *stats_title = Label(stats, "PROGRESS", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(stats_title, 30, 24);
  lv_obj_set_style_text_letter_space(stats_title, 3, 0);
  result.metrics = Label(stats, "Waiting for progress information", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(result.metrics, 30, 78);
  lv_obj_set_width(result.metrics, 740);
  lv_label_set_long_mode(result.metrics, LV_LABEL_LONG_DOT);
  result.files = Label(stats, "", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_align(result.files, LV_ALIGN_RIGHT_MID, -30, 28);
  lv_obj_set_width(result.files, 380);
  lv_obj_set_style_text_align(result.files, LV_TEXT_ALIGN_RIGHT, 0);

  const bool network = request.path.find("/mnt/nas") != std::string::npos;
  const std::string destination = network ? "Destination  /  Network Storage" :
      request.path.empty() ? "Destination  /  Recovery storage" : "Destination  /  " + request.path;
  result.destination = Label(card, destination.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(result.destination, 48, 650);
  lv_obj_set_width(result.destination, 1216);
  lv_label_set_long_mode(result.destination, LV_LABEL_LONG_DOT);

  const bool installer = request.job == Job::kInstall;
  const int activity_y = landscape ? 340 : 1230;
  const int activity_height = installer
      ? std::max(500, static_cast<int>(lv_obj_get_height(screen)) -
                          activity_y - (landscape ? 250 : 360))
      : (landscape ? 850 : 600);
  auto *activity_card = lv_obj_create(screen);
  Panel(activity_card, 40, kMainSheet);
  lv_obj_set_pos(activity_card, landscape ? 1560 : 64, activity_y);
  lv_obj_set_size(activity_card, landscape ? 1544 : 1312,
                  activity_height);
  lv_obj_set_style_border_width(activity_card, 1, 0);
  lv_obj_set_style_border_color(activity_card, kMainLine, 0);
  lv_obj_set_style_border_opa(activity_card, LV_OPA_20, 0);
  auto *activity_title = Label(activity_card,
      installer ? "INSTALLER OUTPUT" : "WHAT'S HAPPENING",
      &lv_font_montserrat_24, kAccent);
  lv_obj_set_pos(activity_title, 48, 42);
  lv_obj_set_style_text_letter_space(activity_title, 3, 0);
  const lv_font_t *activity_font = installer
      ? (landscape ? &lv_font_montserrat_20 : &lv_font_montserrat_24)
      : &lv_font_montserrat_32;
  const int activity_text_height = installer
      ? activity_height - 150 : LV_SIZE_CONTENT;
  result.activity_summary = Label(activity_card,
      installer ? "Waiting for installer output..." :
          "AERA is preparing the operation. Progress will appear here in plain language.",
      activity_font, kText);
  lv_obj_set_pos(result.activity_summary, 48, installer ? 100 : 112);
  lv_obj_set_size(result.activity_summary, landscape ? 1448 : 1216,
                  activity_text_height);
  if (installer) {
    const int line_space = landscape ? 0 : 2;
    const int line_height =
        static_cast<int>(lv_font_get_line_height(activity_font)) + line_space;
    result.installer_lines = static_cast<unsigned>(std::max(
        1, activity_text_height / line_height));
    lv_label_set_long_mode(result.activity_summary, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(result.activity_summary, line_space, 0);
  } else {
    result.notice = Label(activity_card,
        LV_SYMBOL_WARNING "  Keep the device powered on until the operation completes.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(result.notice, 48, 280);
    lv_obj_set_width(result.notice, landscape ? 1448 : 1216);
  }
  result.details = Button(activity_card, "View technical details", [screen] {
    ShowTechnicalLog(screen);
  });
  lv_obj_set_pos(result.details, 48,
                 installer ? activity_height - 200 :
                     (landscape ? 650 : 400));
  lv_obj_set_size(result.details, landscape ? 1448 : 1216, 132);
  if (installer) lv_obj_add_flag(result.details, LV_OBJ_FLAG_HIDDEN);

  const bool backup = request.job == Job::kBackup || request.job == Job::kRestore;
  const bool format = request.job == Job::kFormatData;
  const bool wipe = request.job == Job::kWipe;
  result.done = Button(screen,
      format ? "Reboot options" : backup ? "Back to backups" : wipe ? "Back to wipe" : "Done",
      [=] {
        callback(format ? Action::kOpenReboot : backup ? Action::kBackup :
                 wipe ? Action::kWipe : Action::kBackHome, context);
      }, true);
  lv_obj_set_size(result.done, landscape ? 1100 : 1280,
                  landscape ? 116 : 150);
  lv_obj_align(result.done, LV_ALIGN_BOTTOM_MID, 0,
               landscape ? -70 : -130);
  result.done_label = lv_obj_get_child(result.done, 0);
  lv_obj_add_flag(result.done, LV_OBJ_FLAG_HIDDEN);

  AnimateEnter(card, 45, 24);
  AnimateEnter(activity_card, 85, 24);
  return result;
}

OperationScene BuildOperationScene(lv_obj_t *screen, const char *path,
                                   ActionCallback callback, void *context) {
  JobRequest request;
  request.job = Job::kInstall;
  request.title = "Install ZIP";
  request.path = path;
  return BuildJobScene(screen, request, callback, context);
}

void RefreshOperationScene(const OperationScene &scene) {
  const int progress = RecoveryProgress();
  const bool indeterminate = scene.indeterminate_progress && progress <= 0;
  lv_bar_set_value(scene.progress, indeterminate ? 0 : progress, LV_ANIM_ON);
  i18n::BindLabel(scene.percent, indeterminate
      ? "Installing"
      : (std::to_string(progress) + "%").c_str());
  if (scene.progress_pulse && indeterminate) {
    lv_obj_remove_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN);
  } else if (scene.progress_pulse) {
    if (scene.indeterminate_progress)
      lv_anim_delete(scene.progress_pulse, SetIndeterminateProgressX);
    if (progress <= 0) {
      lv_obj_add_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN);
      const int x = 48 + std::clamp(progress * 1216 / 100 - 12, 0, 1192);
      lv_obj_set_x(scene.progress_pulse, x);
    }
  }

  const unsigned seconds = lv_tick_elaps(scene.started) / 1000;
  const std::string elapsed =
      i18n::Format("%u:%02u elapsed", seconds / 60, seconds % 60);
  i18n::BindLabel(scene.elapsed, elapsed.c_str());

  auto friendly = Explain(RecoveryOperationDetail(), scene.job, progress);
  if (scene.job == Job::kInstall) {
    const std::string history =
        CleanInstallerHistory(RecoveryInstallerStatus(),
                              std::max(1U, scene.installer_lines));
    const auto installer_lines = Lines(history);
    const std::string installer =
        installer_lines.empty() ? "" : installer_lines.back();
    if (!installer.empty()) {
      const std::string lower = Lower(installer);
      if (lower.find("installing aera to slot") != std::string::npos ||
          lower.find("flashing aera") != std::string::npos) {
        friendly.title = "Writing AERA recovery";
      } else if (lower.find("finished installing") != std::string::npos) {
        friendly.title = "Installation complete";
      } else if (lower.find("reboot") != std::string::npos ||
                 (installer.size() >= 2 &&
                  std::isdigit(static_cast<unsigned char>(installer[0])) &&
                  installer[1] == 's')) {
        friendly.title = "Restarting recovery";
      } else {
        friendly.title = "Installing AERA update";
      }
      friendly.explanation = installer;
      friendly.amount = installer;
      friendly.files.clear();
      friendly.activity = history;
    } else {
      friendly.amount = "Waiting for installer output";
      friendly.activity = "Waiting for installer output...";
    }
  }
  i18n::BindLabel(scene.status, friendly.title.c_str());
  i18n::BindLabel(scene.detail, friendly.explanation.c_str());
  i18n::BindLabel(scene.metrics,
      friendly.amount.empty() ? "Working..." : friendly.amount.c_str());
  i18n::BindLabel(scene.files, friendly.files.c_str());
  i18n::BindLabel(scene.activity_summary, friendly.activity.c_str());
}

void CompleteOperationScene(const OperationScene &scene, bool success,
                            const char *detail) {
  RecoveryVibrate(Haptic::kAction);
  RefreshOperationScene(scene);
  lv_anim_delete(scene.activity, SetActivityBorderOpacity);
  if (scene.progress_pulse)
    lv_anim_delete(scene.progress_pulse, SetProgressPulseOpacity);
  if (scene.progress_pulse)
    lv_anim_delete(scene.progress_pulse, SetIndeterminateProgressX);
  if (success) {
    lv_bar_set_value(scene.progress, 100, LV_ANIM_ON);
    i18n::BindLabel(scene.percent, "100%");
  }
  const auto result_color = success ? design::kGreen : design::kRed;
  lv_obj_set_style_bg_color(scene.activity,
      success ? design::kGreenSoft : design::kRedSoft, 0);
  lv_obj_set_style_border_color(scene.activity, result_color, 0);
  lv_obj_set_style_border_opa(scene.activity, LV_OPA_50, 0);
  if (lv_obj_get_child_count(scene.activity) > 0) {
    auto *icon = lv_obj_get_child(scene.activity, 0);
    i18n::BindLabel(icon, success ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(icon, result_color, 0);
  }
  lv_obj_set_style_bg_color(scene.progress, result_color, LV_PART_INDICATOR);
  if (scene.progress_pulse && !scene.indeterminate_progress) {
    lv_obj_remove_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(scene.progress_pulse, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scene.progress_pulse, result_color, 0);
    lv_obj_set_style_shadow_color(scene.progress_pulse, result_color, 0);
    if (success) lv_obj_set_x(scene.progress_pulse, 1240);
  } else if (scene.progress_pulse) {
    lv_obj_add_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN);
  }
  i18n::BindLabel(scene.status, success ? "Operation complete" : "Operation stopped");
  lv_obj_set_style_text_color(scene.status, result_color, 0);
  lv_obj_set_style_text_color(scene.percent, result_color, 0);
  if (scene.format_data && success) {
    i18n::BindLabel(scene.detail, "Data was formatted successfully. Reboot recovery before using /data again.");
    i18n::BindLabel(scene.activity_summary,
        "Android may need a moment to recreate shared storage on the next boot.");
  } else {
    i18n::BindLabel(scene.detail, detail && *detail ? detail :
        success ? "Everything finished successfully." : "Open technical details to see what went wrong.");
    i18n::BindLabel(scene.activity_summary, success ?
        "The requested operation completed successfully. It is now safe to continue." :
        "AERA could not finish this operation. Open technical details for troubleshooting information.");
  }
  if (scene.notice) {
    i18n::BindLabel(scene.notice, success ?
        LV_SYMBOL_OK "  Finished safely. You can now continue." :
        LV_SYMBOL_WARNING "  Review technical details before trying again.");
    lv_obj_set_style_text_color(scene.notice, result_color, 0);
  }
  if (scene.details)
    lv_obj_remove_flag(scene.details, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(scene.done, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace recovery_ui2
