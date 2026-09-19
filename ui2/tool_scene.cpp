/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "phone_keyboard.hpp"
#include "ui_components.hpp"
#include <cmath>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace recovery_ui2 {
namespace {
using namespace widgets;
UserDecryptRequest selected_user_decrypt;
struct Tools {
  lv_obj_t *screen = nullptr, *list = nullptr, *summary = nullptr;
  lv_obj_t *selection_detail = nullptr, *review = nullptr;
  Action tool = Action::kNone;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  std::vector<Volume> volumes;
  std::set<std::string> selected;
  std::string restore_folder;
  bool compression = true;
  lv_obj_t *format_input = nullptr, *format_submit = nullptr;
};

void Open(Tools *state, Action action) { state->callback(action, state->context); }
void Run(Tools *state, const JobRequest &request) {
  SetJobRequest(request);
  Open(state, Action::kRunOperation);
}

std::string WipeDescription(const std::string &path) {
  if (path == "DALVIK") return "Generated app cache; rebuilt by Android";
  if (path == "INTERNAL") return "Deletes photos, downloads and files";
  if (path == "/data") return "Erase apps and settings on the data partition";
  if (path == "/metadata") return "Encryption metadata; may make data inaccessible";
  return i18n::Format("Erase %s", path.c_str());
}

void UpdateSelection(Tools *state) {
  uint64_t bytes = 0;
  for (const auto &v : state->volumes)
    if (state->selected.count(v.path)) bytes += v.bytes;
  std::string summary =
      i18n::Format("%zu selected", state->selected.size());
  if (state->tool == Action::kBackup)
    summary = i18n::Format("Estimated backup: %s", Size(bytes).c_str());
  i18n::BindLabel(state->summary, summary.c_str());
  if (state->review) {
    if (state->selected.empty()) lv_obj_add_state(state->review, LV_STATE_DISABLED);
    else lv_obj_remove_state(state->review, LV_STATE_DISABLED);
  }
  if (state->selection_detail) {
    std::string detail = state->selected.size() == 1
        ? i18n::Format("%zu partition selected", state->selected.size())
        : i18n::Format("%zu partitions selected", state->selected.size());
    if (state->tool == Action::kBackup) {
      detail += " / ";
      detail += i18n::Translate(state->compression
          ? "Before compression; archive size varies"
          : "Archive overhead not included");
    }
    else if (!state->restore_folder.empty())
      detail = state->restore_folder.substr(state->restore_folder.find_last_of('/') + 1);
    i18n::BindLabel(state->selection_detail, detail.c_str());
  }
}

void PartitionRows(Tools *state) {
  lv_obj_clean(state->list);
  // Resolve the real scroll width before laying out rows. Without this pass,
  // LVGL can still report its creation-time width and the entire row collapses
  // into the 600 px fallback.
  lv_obj_update_layout(state->list);
  const int row_width = std::max(600, static_cast<int>(
      lv_obj_get_width(state->list)));
  for (size_t i = 0; i < state->volumes.size(); ++i) {
    const auto v = state->volumes[i];
    const bool selected = state->selected.count(v.path) != 0;
    {
      const bool wipe = state->tool == Action::kWipe;
      auto *row = Button(state->list, "", [] {});
      lv_obj_set_pos(row, 0, static_cast<int>(i) * 148);
      lv_obj_set_size(row, row_width, 142);
      lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_radius(row, 18, 0);
      auto *check = lv_checkbox_create(row);
      lv_checkbox_set_text(check, "");
      lv_obj_set_pos(check, 24, 46);
      lv_obj_set_style_pad_all(check, 0, LV_PART_MAIN);
      lv_obj_set_style_text_font(check, UiFont(&lv_font_montserrat_32), LV_PART_MAIN);
      lv_obj_set_style_pad_all(check, 8, LV_PART_INDICATOR);
      lv_obj_set_style_radius(check, 10, LV_PART_INDICATOR);
      lv_obj_set_style_border_width(check, 2, LV_PART_INDICATOR);
      lv_obj_set_style_border_color(check, kMainLine, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(check, kMainPanel, LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(check, LV_OPA_COVER, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(check, kAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_border_color(check, kAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_text_color(check, kCanvas, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_image_src(check, LV_SYMBOL_OK, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_image_recolor(check, kCanvas, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_image_opa(check, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
      if (selected) lv_obj_add_state(check, LV_STATE_CHECKED);
      auto *name = Label(row, v.name.c_str(), &lv_font_montserrat_36, kText);
      lv_obj_set_pos(name, 112, 14);
      lv_obj_set_width(name, wipe ? row_width - 150 : row_width - 430);
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      const auto detail = wipe ? WipeDescription(v.path) : v.path;
      auto *path = Label(row, detail.c_str(), &lv_font_montserrat_32,
          wipe && (v.path == "/metadata" || v.path == "INTERNAL") ? kAmber : kMuted);
      lv_obj_set_pos(path, 112, 76);
      lv_obj_set_width(path, wipe ? row_width - 150 : row_width - 430);
      lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
      if (!wipe) {
        auto *size = Label(row, Size(v.bytes).c_str(), &lv_font_montserrat_32, kMutedStrong);
        lv_obj_set_width(size, 270);
        lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(size, LV_ALIGN_RIGHT_MID, -28, 0);
      }
      OnClick(row, [state, v, check] {
        if (state->selected.erase(v.path)) lv_obj_remove_state(check, LV_STATE_CHECKED);
        else { state->selected.insert(v.path); lv_obj_add_state(check, LV_STATE_CHECKED); }
        UpdateSelection(state);
      });
    }
  }
  if (state->volumes.empty()) {
    auto *label = Label(state->list,
        "No available partitions. Check storage and encryption state.",
        &lv_font_montserrat_32, kMuted);
    lv_obj_set_width(label, 1250);
    lv_obj_set_pos(label, 16, 50);
  }
  UpdateSelection(state);
}

void Review(Tools *state) {
  if (state->selected.empty()) {
    Sheet(state->screen, "Select partitions", "Choose at least one partition first.");
    return;
  }
  JobRequest request;
  request.job = state->tool == Action::kBackup ? Job::kBackup :
                state->tool == Action::kRestore ? Job::kRestore : Job::kWipe;
  request.title = state->tool == Action::kBackup ? "Backup" :
                  state->tool == Action::kRestore ? "Restore" : "Wipe";
  request.path = state->tool == Action::kRestore ? state->restore_folder : RecoveryStorage();
  request.compression = state->compression;
  request.partitions.assign(state->selected.begin(), state->selected.end());
  std::string text;
  if (request.job == Job::kBackup) {
    uint64_t bytes = 0;
    for (const auto &v : state->volumes) if (state->selected.count(v.path)) bytes += v.bytes;
    text = "Destination\n" + RecoveryBackupRoot() +
           "\n\nA new dated backup folder will be created.\n\nEstimated size: " + Size(bytes) +
           "\nBefore compression; actual archive size may differ.\n";
    if (RecoveryDataLocked()) text += "\nStorage is locked. This is not a decrypted user-data backup.\n";
  } else if (request.job == Job::kRestore) {
    text = "Source\n" + request.path +
           "\n\nRestoring overwrites the selected partitions.\n";
  } else text = "This permanently erases the selected contents.\n";
  text += "\nSelected partitions\n";
  for (const auto &v : state->volumes) {
    if (state->selected.count(v.path))
      text += "\n" + v.name + "  (" + v.path + ")";
  }
  if (request.job == Job::kWipe) {
    text += "\n\nWhat will be erased\n";
    for (const auto &v : state->volumes)
      if (state->selected.count(v.path)) text += "\n" + v.name + ": " + WipeDescription(v.path);
  }
  text += "\n\nActive slot: " + RecoverySlot();
  if (request.job == Job::kBackup)
    text += state->compression ? "\nCompression on. Digests included." :
                                "\nCompression off. Digests included.";
  if (request.job == Job::kRestore) text += "\nDigest verification enabled.";
  Sheet(state->screen, "Review " + request.title, text,
        [state, request] { Run(state, request); });
}

void RestoreFolders(Tools *state) {
  state->restore_folder.clear();
  state->selected.clear();
  state->volumes.clear();
  lv_obj_clean(state->list);
  if (state->review) lv_obj_add_state(state->review, LV_STATE_DISABLED);
  const auto root = RecoveryBackupRoot();
  std::vector<std::string> folders;
  DIR *directory = opendir(root.c_str());
  if (directory) {
    while (auto *entry = readdir(directory)) {
      if (entry->d_name[0] == '.') continue;
      struct stat info{};
      const std::string path = root + "/" + entry->d_name;
      if (!stat(path.c_str(), &info) && S_ISDIR(info.st_mode)) folders.push_back(path);
    }
    closedir(directory);
  }
  std::sort(folders.rbegin(), folders.rend());
  const std::string summary =
      i18n::Format("%zu saved backups", folders.size());
  i18n::BindLabel(state->summary, summary.c_str());
  if (state->selection_detail) i18n::BindLabel(state->selection_detail, "Choose a backup, then select partitions to restore");
  for (size_t i = 0; i < folders.size(); ++i) {
    const auto path = folders[i];
    Row(state->list, static_cast<int>(i) * 190, LV_SYMBOL_SAVE,
        path.substr(path.find_last_of('/') + 1), "Inspect available partitions", [state, path] {
      state->restore_folder = path;
      state->selected.clear();
      state->volumes = RecoveryRestoreVolumes(path);
      if (state->volumes.empty()) {
        Sheet(state->screen, "Backup unavailable",
              "No supported partitions were found. Encrypted backup archives "
              "need a separate archive-password workflow.\n\n" + path);
        state->restore_folder.clear();
        return;
      }
      PartitionRows(state);
    });
  }
  if (folders.empty()) {
    auto *empty = Label(state->list,
        ("No backups found here.\n\n" + root +
         "\n\nChoose another storage location or create a backup first.").c_str(),
        &lv_font_montserrat_32, kMuted);
    lv_obj_set_width(empty, 1240);
    lv_obj_set_pos(empty, 16, 48);
  }
}

void StorageChooser(Tools *state) {
  lv_obj_t *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  MainBackground(overlay);
  auto *heading = Label(overlay, "Choose storage", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 80, 250);
  auto *list = Scroll(overlay, 420, 2050);
  auto volumes = RecoveryVolumes("storage");
  for (size_t i = 0; i < volumes.size(); ++i) {
    const auto v = volumes[i];
    Row(list, static_cast<int>(i) * 190, LV_SYMBOL_DRIVE, v.name, v.path, [state, v] {
      if (!RecoverySetStorage(v.path)) {
        Sheet(state->screen, "Storage unavailable", "Could not mount " + v.path);
      } else Open(state, state->tool);
    });
  }
  auto *close = Button(overlay, "Back", [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_set_size(close, 1260, 140);
  lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -180);
}

void WipeTabs(Tools *state) {
  const bool format = state->tool == Action::kFormatData;
  const bool landscape = Landscape(state->screen);
  auto *wipe = Button(state->screen, "Wipe partitions", [state] { Open(state, Action::kWipe); }, !format);
  lv_obj_set_pos(wipe, 64, landscape ? 330 : 458);
  lv_obj_set_size(wipe, landscape ? 470 : 642, 112);
  auto *data = Button(state->screen, "Format Data", [state] { Open(state, Action::kFormatData); }, format);
  lv_obj_set_pos(data, landscape ? 550 : 734, landscape ? 330 : 458);
  lv_obj_set_size(data, landscape ? 470 : 642, 112);
}

void UpdateFormatConfirmation(Tools *state) {
  const bool confirmed = std::string(lv_textarea_get_text(state->format_input)) == "yes";
  if (confirmed) lv_obj_remove_state(state->format_submit, LV_STATE_DISABLED);
  else lv_obj_add_state(state->format_submit, LV_STATE_DISABLED);
  auto *text = lv_obj_get_child(state->format_submit, 0);
  lv_obj_set_style_text_color(text, confirmed ? kText : kMuted, 0);
}

void BuildFormatData(Tools *state, bool fastboot_mode = false) {
  Header(state->screen, fastboot_mode ? "Fastbootd · Format Data" : "Wipe",
         fastboot_mode
             ? "Erase encrypted internal storage while userspace fastboot remains active."
             : "Erase selected partitions or format internal storage.",
         fastboot_mode ? nullptr : state->callback,
         fastboot_mode ? nullptr : state->context);
  if (!fastboot_mode) WipeTabs(state);
  const bool landscape = Landscape(state->screen);
  auto *title = Label(state->screen, "Erase all internal data", &lv_font_montserrat_48, kRed);
  lv_obj_set_pos(title, 80, landscape ? 480 : 654);
  auto *warning = Label(state->screen,
      "Formatting /data deletes apps, files, photos, videos\n"
      "and backups stored in internal storage.\n\n"
      "This cannot be undone.", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(warning, 80, landscape ? 560 : 744);
  lv_obj_set_width(warning, landscape ? 980 : 1270);
  lv_obj_set_style_text_line_space(warning, 12, 0);
  auto *encryption = Label(state->screen,
      "Resets storage encryption; Android may encrypt it again.\n"
      "Adopted storage, if present, may also be erased.", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(encryption, 80, landscape ? 770 : 1040);
  lv_obj_set_width(encryption, landscape ? 980 : 1270);
  lv_obj_set_style_text_line_space(encryption, 12, 0);
  auto *prompt = Label(state->screen, "Type yes to enable Format Data", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(prompt, 80, landscape ? 900 : 1228);
  state->format_input = TextArea(state->screen);
  lv_obj_set_pos(state->format_input, 80, landscape ? 960 : 1310);
  lv_obj_set_size(state->format_input, landscape ? 940 : 1280, 150);
  lv_textarea_set_one_line(state->format_input, true);
  // Don't truncate to three letters: "yesplease" must not become "yes".
  lv_textarea_set_max_length(state->format_input, 32);
  lv_textarea_set_text(state->format_input, "");
  lv_textarea_set_placeholder_text(state->format_input, "yes");
  lv_obj_set_style_text_font(state->format_input, UiFont(&lv_font_montserrat_48), 0);
  lv_obj_set_style_text_color(state->format_input, kText, 0);
  lv_obj_set_style_text_color(state->format_input, kMuted, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(state->format_input, kMainPanel, 0);
  lv_obj_set_style_bg_opa(state->format_input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(state->format_input, kMainLine, 0);
  lv_obj_set_style_border_color(state->format_input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(state->format_input, 2, 0);
  lv_obj_set_style_radius(state->format_input, 24, 0);
  lv_obj_set_style_pad_all(state->format_input, 32, 0);

  auto *keyboard = lv_keyboard_create(state->screen);
  phone_keyboard::Apply(keyboard);
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard, landscape ? 1100 : 48, landscape ? 330 : 1540);
  lv_obj_set_size(keyboard, landscape ? 2004 : 1344,
                  landscape ? 850 : 780);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(keyboard, state->format_input);

  state->format_submit = Button(state->screen, "Format data now", [state] {
    JobRequest request;
    request.job = Job::kFormatData; request.title = "Format Data"; request.path = "/data";
    request.confirmation = lv_textarea_get_text(state->format_input);
    if (!FormatDataAuthorized(request)) return;
    lv_textarea_set_text(state->format_input, "");
    Run(state, request);
  });
  lv_obj_set_pos(state->format_submit, 80, landscape ? 1140 : 2600);
  lv_obj_set_size(state->format_submit, landscape ? 450 : 1280, 132);
  lv_obj_set_style_bg_color(state->format_submit, kRed, 0);
  lv_obj_set_style_bg_color(state->format_submit, kRedSoft, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(state->format_submit, kMainPanel, LV_STATE_DISABLED);
  UpdateFormatConfirmation(state);
  lv_obj_add_event_cb(state->format_input, [](lv_event_t *event) {
    UpdateFormatConfirmation(static_cast<Tools *>(lv_event_get_user_data(event)));
  }, LV_EVENT_VALUE_CHANGED, state);
  // Neither typing nor the keyboard's Enter/Ready action starts a format.
  auto *cancel = Button(state->screen, "Cancel", [state, fastboot_mode] {
    Open(state, fastboot_mode ? Action::kBack : Action::kWipe);
  });
  lv_obj_set_pos(cancel, landscape ? 570 : 80, landscape ? 1140 : 2770);
  lv_obj_set_size(cancel, landscape ? 450 : 1280, landscape ? 132 : 112);
}

void BuildPartitions(Tools *state) {
  const bool backup = state->tool == Action::kBackup;
  const bool restore = state->tool == Action::kRestore;
  Header(state->screen, backup || restore ? "Backups" : "Wipe",
         backup || restore ? "Create a backup or restore a saved one." :
         "Choose exactly what to erase.", state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->summary = Label(state->screen, "", &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(state->summary, 80, landscape ? 760 : 770);
  state->list = Scroll(state->screen, landscape ? 350 : 870,
                       landscape ? 900 : 1610);
  if (landscape) {
    lv_obj_set_x(state->list, 1100);
    lv_obj_set_width(state->list, 2004);
  }

  if (backup || restore) {
    auto *create = Button(state->screen, "Create backup", [state] { Open(state, Action::kBackup); }, backup);
    lv_obj_set_pos(create, 64, landscape ? 350 : 458);
    lv_obj_set_size(create, landscape ? 470 : 642, 112);
    auto *saved = Button(state->screen, "Restore backups", [state] { Open(state, Action::kRestore); }, restore);
    lv_obj_set_pos(saved, landscape ? 550 : 734, landscape ? 350 : 458);
    lv_obj_set_size(saved, landscape ? 470 : 642, 112);
    lv_obj_set_pos(state->summary, 80, landscape ? 760 : 812);
    lv_obj_set_style_text_font(state->summary, UiFont(&lv_font_montserrat_48), 0);
    state->selection_detail = Label(state->screen, "", &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(state->selection_detail, 80, landscape ? 830 : 884);
    lv_obj_set_width(state->selection_detail, landscape ? 940 : 1280);
    lv_label_set_long_mode(state->selection_detail, LV_LABEL_LONG_DOT);
    if (!landscape) {
      lv_obj_set_pos(state->list, 64, 954);
      lv_obj_set_height(state->list, 1550);
    }
    const std::string storage_action =
        std::string(i18n::Translate("Choose storage")) + "  " LV_SYMBOL_DOWN;
    auto *storage = Button(state->screen, storage_action.c_str(),
                            [state] { StorageChooser(state); });
    lv_obj_set_pos(storage, 64, landscape ? 490 : 608);
    lv_obj_set_size(storage, landscape ? 470 : 630, 112);
    std::string storage_text = RecoveryStorage();
    struct statvfs capacity{};
    if (statvfs(storage_text.c_str(), &capacity) == 0)
      storage_text = i18n::Format(
          "%s / %s free", storage_text.c_str(),
          Size(uint64_t(capacity.f_bavail) * capacity.f_frsize).c_str());
    auto *path = Label(state->screen, storage_text.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(path, 80, landscape ? 630 : 746);
    lv_obj_set_width(path, landscape ? 940 : 1260);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    if (backup) {
      auto *compression = Button(state->screen, state->compression ? "Compression: on" : "Compression: off", [] {});
      lv_obj_set_pos(compression, landscape ? 550 : 714,
                     landscape ? 490 : 608);
      lv_obj_set_size(compression, landscape ? 470 : 662, 112);
      auto *label = lv_obj_get_child(compression, 0);
      OnClick(compression, [state, label] {
        state->compression = !state->compression;
        i18n::BindLabel(label, state->compression ? "Compression: on" : "Compression: off");
        UpdateSelection(state);
      });
    } else {
      auto *folders = Button(state->screen, "Refresh backups", [state] { RestoreFolders(state); });
      lv_obj_set_pos(folders, landscape ? 550 : 714,
                     landscape ? 490 : 608);
      lv_obj_set_size(folders, landscape ? 470 : 662, 112);
    }
  } else {
    WipeTabs(state);
    auto *notice = Label(state->screen,
        "Only selected partitions will be wiped.", &lv_font_montserrat_32, kAmber);
    lv_obj_set_pos(notice, 80, landscape ? 500 : 642);
    lv_obj_set_width(notice, landscape ? 940 : 1250);
    auto *hint = Label(state->screen,
        "To erase all internal data and reset encryption, use Format Data.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(hint, 80, landscape ? 590 : 710);
    lv_obj_set_width(hint, landscape ? 940 : 1270);
    lv_obj_set_pos(state->summary, 80, landscape ? 760 : 818);
    if (!landscape) {
      lv_obj_set_pos(state->list, 64, 900);
      lv_obj_set_height(state->list, 1650);
    }
  }
  auto *review = Button(state->screen, backup ? "Review backup" :
                        restore ? "Review restore" : "Review wipe",
                        [state] { Review(state); }, true);
  constexpr int review_height = 150;
  const int review_gap = landscape ? 20 : 24;
  const int review_y = lv_obj_get_height(state->screen) -
      NavigationHeight(state->screen) - review_gap - review_height;
  lv_obj_set_pos(review, 80, review_y);
  lv_obj_set_size(review, landscape ? 940 : 1280, review_height);
  if (!landscape && (backup || restore)) {
    constexpr int list_top = 954;
    constexpr int list_to_review_gap = 28;
    lv_obj_set_height(state->list,
                      std::max(600, review_y - list_top - list_to_review_gap));
  }
  state->review = review;
  lv_obj_set_style_bg_color(review, kMainPanel, LV_STATE_DISABLED);
  lv_obj_set_style_text_color(lv_obj_get_child(review, 0), kMuted, LV_STATE_DISABLED);
  if (restore) RestoreFolders(state);
  else {
    state->volumes = RecoveryVolumes(backup ? "backup" : "wipe");
    PartitionRows(state);
  }
}

void BuildMounts(Tools *state) {
  Header(state->screen, "Mounts", "Manage the device's recovery volumes.", state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 510,
                       landscape ? 900 : 2260);
  lv_obj_update_layout(state->list);
  const int row_width = std::max(600, static_cast<int>(lv_obj_get_width(state->list)));

  std::vector<Volume> mounted;
  std::vector<Volume> available;
  const auto mount_volumes = RecoveryVolumes("mount");
  const bool has_data = std::any_of(
      mount_volumes.begin(), mount_volumes.end(),
      [](const Volume &volume) { return volume.path == "/data"; });
  for (const auto &volume : mount_volumes) {
    // /storage is a legacy bind alias for data/media. The usable internal
    // storage is already exposed through /sdcard when /data is mounted, so a
    // second independent-looking row reports a misleading state.
    if (has_data && volume.path == "/storage") continue;
    (volume.selected ? mounted : available).push_back(volume);
  }

  int y = 0;
  const auto add_group = [state, row_width, &y](
      const char *heading, const std::vector<Volume> &volumes, bool is_mounted) {
    const std::string title = std::string(i18n::Translate(heading)) + "  " +
        std::to_string(volumes.size());
    auto *section = Label(state->list, title.c_str(), &lv_font_montserrat_28,
                          is_mounted ? kAccent : kMutedStrong);
    lv_obj_set_pos(section, 24, y + 12);
    lv_obj_set_width(section, row_width - 48);
    y += 72;

    for (const auto &volume : volumes) {
      auto *row = Button(state->list, "", [state, volume] {
        JobRequest request;
        request.job = volume.selected ? Job::kUnmount : Job::kMount;
        request.path = volume.path;
        request.title = volume.selected ? "Unmount volume" : "Mount volume";
        Sheet(state->screen, request.title, volume.name + "\n\n" + volume.path +
              "\n\nRecovery's configured mount and read-only rules apply.",
              [state, request] { Run(state, request); });
      });
      lv_obj_set_pos(row, 0, y);
      lv_obj_set_size(row, row_width, 166);
      lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
      lv_obj_set_style_radius(row, 20, 0);
      lv_obj_set_style_bg_color(row, kMainPanel, 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_40, 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_border_color(row, is_mounted ? kAccent : kMainLine, 0);
      lv_obj_set_style_border_opa(row, is_mounted ? LV_OPA_40 : LV_OPA_30, 0);

      auto *rail = lv_obj_create(row);
      Clear(rail);
      lv_obj_remove_flag(rail, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_pos(rail, 0, 20);
      lv_obj_set_size(rail, 6, 126);
      lv_obj_set_style_radius(rail, 3, 0);
      lv_obj_set_style_bg_color(rail, is_mounted ? kAccent : kMainLine, 0);
      lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);

      auto *icon = Label(row, LV_SYMBOL_DRIVE, &lv_font_montserrat_32,
                         is_mounted ? kAccent : kMutedStrong);
      lv_obj_set_pos(icon, 32, 65);

      auto *name = Label(row, volume.name.c_str(), &lv_font_montserrat_32, kText);
      lv_obj_set_pos(name, 112, 29);
      SingleLineLabel(name, row_width - 520, &lv_font_montserrat_32);

      auto *path = Label(row, volume.path.c_str(), &lv_font_montserrat_24,
                         is_mounted ? kMutedStrong : kMuted);
      lv_obj_set_pos(path, 112, 94);
      SingleLineLabel(path, row_width - 520, &lv_font_montserrat_24);

      auto *status = Label(row, is_mounted ? "Mounted" : "Not mounted",
                           &lv_font_montserrat_24,
                           is_mounted ? kAccent : kMutedStrong);
      SingleLineLabel(status, 220, &lv_font_montserrat_24);
      lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_align(status, LV_ALIGN_RIGHT_MID, -154, 0);

      auto *toggle = lv_switch_create(row);
      lv_obj_set_size(toggle, 104, 58);
      lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -24, 0);
      lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_bg_color(toggle, kMainLine, LV_PART_MAIN);
      lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_color(toggle, kAccent,
                               LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB);
      lv_obj_set_style_bg_color(toggle, kText, LV_PART_KNOB);
      lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
      lv_obj_set_style_pad_all(toggle, -8, LV_PART_KNOB);
      if (is_mounted) lv_obj_add_state(toggle, LV_STATE_CHECKED);

      y += 182;
    }
    y += 18;
  };

  add_group("Mounted", mounted, true);
  add_group("Not mounted", available, false);
}

void PreferenceToggle(Tools *state, int y, const char *title, const char *description,
                      Preference preference) {
  auto *row = Row(state->list, y, LV_SYMBOL_SETTINGS, title, description, [] {}, "");
  // A full-width touch target; the switch is visual and does not double-toggle.
  auto *toggle = lv_switch_create(row);
  lv_obj_set_size(toggle, 108, 60);
  lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -24, 0);
  lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(toggle, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(toggle, kAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_bg_color(toggle, kText, LV_PART_KNOB);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(toggle, -8, LV_PART_KNOB);
  if (RecoveryPreference(preference)) lv_obj_add_state(toggle, LV_STATE_CHECKED);
  OnClick(row, [state, toggle, preference] {
    const bool enabled = !RecoveryPreference(preference);
    if (!RecoverySetPreference(preference, enabled)) {
      Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
      return;
    }
    if (enabled) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
  });
}

void PreferenceSection(Tools *state, int y, const char *title) {
  auto *label = Label(state->list, title, &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(label, 32, y);
}

void HapticSlider(Tools *state, int y, const char *title,
                  const char *description, Haptic haptic, int maximum) {
  auto *card = lv_obj_create(state->list);
  Panel(card, 32, kMainPanel);
  lv_obj_set_pos(card, 16, y);
  lv_obj_set_size(card, 1280, 220);
  auto *name = Label(card, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 36, 26);
  auto *copy = Label(card, description, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 36, 78);
  lv_obj_set_width(copy, 950);
  auto duration_text = [](int milliseconds) {
    return milliseconds == 0 ? std::string("Off") :
        std::to_string(milliseconds) + " ms";
  };
  auto *value_plate = lv_obj_create(card);
  Panel(value_plate, 28, kAccentSoft);
  lv_obj_set_pos(value_plate, 1060, 24);
  lv_obj_set_size(value_plate, 176, 68);
  auto *value = Label(value_plate,
                      duration_text(RecoveryHapticDuration(haptic)).c_str(),
                      &lv_font_montserrat_24, kAccent);
  lv_obj_center(value);
  auto *slider = lv_slider_create(card);
  lv_obj_set_pos(slider, 52, 166);
  lv_obj_set_size(slider, 1176, 20);
  lv_slider_set_range(slider, 0, maximum);
  lv_slider_set_value(slider, RecoveryHapticDuration(haptic), LV_ANIM_OFF);
  RangeSlider(slider);
  struct Binding {
    lv_obj_t *value;
    Haptic haptic;
  };
  auto *binding = new Binding{value, haptic};
  lv_obj_add_event_cb(slider, [](lv_event_t *event) {
    auto *binding = static_cast<Binding *>(lv_event_get_user_data(event));
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
      delete binding;
      return;
    }
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
    const int duration = lv_slider_get_value(lv_event_get_target_obj(event));
    RecoverySetHapticDuration(binding->haptic, duration);
    const std::string text = duration == 0 ? "Off" :
        std::to_string(duration) + " ms";
    i18n::BindLabel(binding->value, text.c_str());
    if (code == LV_EVENT_RELEASED) RecoveryVibrate(binding->haptic);
  }, LV_EVENT_ALL, binding);
}

struct AccentPreset {
  const char *name;
  uint32_t rgb;
};

constexpr std::array<AccentPreset, 7> kAccentPresets{{
    {"AERA Cyan", 0x16c8ff}, {"Azure", 0x4c8dff},
    {"Violet", 0xa991ff}, {"Magenta", 0xf05dce},
    {"Lime", 0xa6e35a}, {"Orange", 0xff8a32},
    {"Emerald", 0x42d392},
}};

bool IsPresetAccent(uint32_t rgb) {
  return std::any_of(kAccentPresets.begin(), kAccentPresets.end(),
      [rgb](const AccentPreset &preset) { return preset.rgb == rgb; });
}

const char *AccentName(uint32_t rgb) {
  for (const auto &preset : kAccentPresets)
    if (preset.rgb == rgb) return preset.name;
  return "User selected";
}

lv_obj_t *HueSaturationDisk(lv_obj_t *parent, int size) {
  auto *pixels = new std::vector<lv_color32_t>(
      static_cast<size_t>(size) * static_cast<size_t>(size));
  constexpr double kPi = 3.14159265358979323846;
  const double center = (size - 1) * 0.5;
  const double radius = center - 1.0;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const double dx = x - center;
      const double dy = y - center;
      const double distance = std::sqrt(dx * dx + dy * dy);
      auto &pixel = (*pixels)[static_cast<size_t>(y) * size + x];
      if (distance > radius + 1.0) {
        pixel = lv_color32_make(0, 0, 0, 0);
        continue;
      }
      double degrees = std::atan2(dy, dx) * 180.0 / kPi;
      if (degrees < 0.0) degrees += 360.0;
      const uint8_t saturation = static_cast<uint8_t>(std::clamp(
          distance * 100.0 / radius, 0.0, 100.0));
      const lv_color_t color = lv_color_hsv_to_rgb(
          static_cast<uint16_t>(degrees), saturation, 100);
      const lv_opa_t alpha = distance <= radius ? LV_OPA_COVER
          : static_cast<lv_opa_t>(std::clamp(
                (radius + 1.0 - distance) * 255.0, 0.0, 255.0));
      pixel = lv_color32_make(color.red, color.green, color.blue, alpha);
    }
  }

  auto *canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(canvas, pixels->data(), size, size,
                       LV_COLOR_FORMAT_ARGB8888);
  lv_obj_set_size(canvas, size, size);
  lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, [](lv_event_t *event) {
    delete static_cast<std::vector<lv_color32_t> *>(
        lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, pixels);
  return canvas;
}

struct AccentPickerState {
  Tools *tools = nullptr;
  lv_obj_t *overlay = nullptr;
  lv_obj_t *wheel = nullptr;
  lv_obj_t *marker = nullptr;
  lv_obj_t *preview = nullptr;
  lv_obj_t *hex = nullptr;
  uint32_t rgb = kDefaultAccentRgb;
  int wheel_size = 0;
};

void UpdateAccentPicker(AccentPickerState *picker, double dx, double dy) {
  constexpr double kPi = 3.14159265358979323846;
  const double radius = picker->wheel_size * 0.5 - 3.0;
  double distance = std::sqrt(dx * dx + dy * dy);
  if (distance > radius && distance > 0.0) {
    dx *= radius / distance;
    dy *= radius / distance;
    distance = radius;
  }
  double degrees = std::atan2(dy, dx) * 180.0 / kPi;
  if (degrees < 0.0) degrees += 360.0;
  const uint8_t saturation = static_cast<uint8_t>(std::clamp(
      distance * 100.0 / radius, 0.0, 100.0));
  const lv_color_t color = lv_color_hsv_to_rgb(
      static_cast<uint16_t>(degrees), saturation, 100);
  picker->rgb = (static_cast<uint32_t>(color.red) << 16) |
                (static_cast<uint32_t>(color.green) << 8) |
                static_cast<uint32_t>(color.blue);

  const int marker_size = 46;
  lv_obj_set_pos(picker->marker,
      static_cast<int>(picker->wheel_size * 0.5 + dx) - marker_size / 2,
      static_cast<int>(picker->wheel_size * 0.5 + dy) - marker_size / 2);
  lv_obj_set_style_bg_color(picker->marker, color, 0);
  lv_obj_set_style_bg_color(picker->preview, color, 0);
  lv_obj_set_style_text_color(picker->hex,
      lv_color_luminance(color) < 118 ? Color(0xffffff) : Color(0x101318), 0);
  char value[16];
  std::snprintf(value, sizeof(value), "#%06X", picker->rgb);
  i18n::BindLabel(picker->hex, value);
}

void UpdateAccentPickerFromTouch(AccentPickerState *picker) {
  lv_indev_t *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);
  lv_area_t area{};
  lv_obj_get_coords(picker->wheel, &area);
  UpdateAccentPicker(picker,
      point.x - area.x1 - picker->wheel_size * 0.5,
      point.y - area.y1 - picker->wheel_size * 0.5);
}

void OpenAccentPicker(Tools *tools) {
  constexpr double kPi = 3.14159265358979323846;
  auto *picker = new AccentPickerState;
  picker->tools = tools;
  picker->rgb = RecoveryAccentColor();
  picker->overlay = lv_obj_create(tools->screen);
  lv_obj_set_user_data(picker->overlay, &kModalMarker);
  Clear(picker->overlay);
  lv_obj_set_size(picker->overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(picker->overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(picker->overlay, LV_OPA_70, 0);
  lv_obj_add_event_cb(picker->overlay, [](lv_event_t *event) {
    delete static_cast<AccentPickerState *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, picker);

  const bool landscape = Landscape(tools->screen);
  const int screen_width = lv_obj_get_width(tools->screen);
  const int screen_height = lv_obj_get_height(tools->screen);
  const int sheet_width = landscape ? std::min(2100, screen_width - 160) : 1312;
  const int sheet_height = landscape ? std::min(1120, screen_height - 100) : 1450;
  auto *sheet = lv_obj_create(picker->overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_center(sheet);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);

  auto *heading = Label(sheet, "Choose your accent",
                        &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 56, 48);
  auto *copy = Label(sheet,
      "Drag anywhere on the colour disk. The centre is softer; the edge is vivid.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 56, 116);
  lv_obj_set_width(copy, sheet_width - 112);

  picker->wheel_size = landscape ? 680 : 760;
  auto *wheel_holder = lv_obj_create(sheet);
  Clear(wheel_holder);
  lv_obj_set_size(wheel_holder, picker->wheel_size, picker->wheel_size);
  lv_obj_set_pos(wheel_holder, landscape ? 70 : (sheet_width - picker->wheel_size) / 2,
                 landscape ? 232 : 208);
  lv_obj_set_style_radius(wheel_holder, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(wheel_holder, kMainPanel, 0);
  lv_obj_set_style_bg_opa(wheel_holder, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_color(wheel_holder, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(wheel_holder, 30, 0);
  lv_obj_set_style_shadow_opa(wheel_holder, LV_OPA_30, 0);
  picker->wheel = HueSaturationDisk(wheel_holder, picker->wheel_size);
  lv_obj_set_pos(picker->wheel, 0, 0);

  picker->marker = lv_obj_create(wheel_holder);
  Clear(picker->marker);
  lv_obj_set_size(picker->marker, 46, 46);
  lv_obj_set_style_radius(picker->marker, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(picker->marker, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(picker->marker, 6, 0);
  lv_obj_set_style_border_color(picker->marker, Color(0xffffff), 0);
  lv_obj_set_style_shadow_color(picker->marker, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(picker->marker, 10, 0);
  lv_obj_set_style_shadow_opa(picker->marker, LV_OPA_50, 0);
  lv_obj_remove_flag(picker->marker, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(wheel_holder, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wheel_holder, [](lv_event_t *event) {
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)
      UpdateAccentPickerFromTouch(static_cast<AccentPickerState *>(
          lv_event_get_user_data(event)));
  }, LV_EVENT_ALL, picker);

  const int controls_x = landscape ? 850 : 80;
  const int controls_y = landscape ? 286 : 1012;
  picker->preview = lv_obj_create(sheet);
  Clear(picker->preview);
  lv_obj_set_pos(picker->preview, controls_x, controls_y);
  lv_obj_set_size(picker->preview, landscape ? sheet_width - controls_x - 70
                                             : sheet_width - 160, 126);
  lv_obj_set_style_radius(picker->preview, 38, 0);
  lv_obj_set_style_bg_opa(picker->preview, LV_OPA_COVER, 0);
  picker->hex = Label(picker->preview, "", &lv_font_montserrat_32,
                      Color(0x101318));
  lv_obj_center(picker->hex);

  const int buttons_y = landscape ? 684 : 1182;
  const int button_width = landscape ? (sheet_width - controls_x - 94) / 2
                                     : (sheet_width - 184) / 2;
  auto *cancel = Button(sheet, "Cancel", [picker] {
    lv_obj_delete_async(picker->overlay);
  });
  lv_obj_set_pos(cancel, controls_x, buttons_y);
  lv_obj_set_size(cancel, button_width, 126);
  auto *use = Button(sheet, "Use colour", [picker] {
    const uint32_t rgb = picker->rgb;
    if (!RecoverySetAccentColor(rgb)) {
      Sheet(picker->tools->screen, "Theme unavailable",
            "The selected accent could not be stored.");
      return;
    }
    ApplyAccent(rgb);
    Open(picker->tools, Action::kTheme);
  }, true);
  lv_obj_set_pos(use, controls_x + button_width + 24, buttons_y);
  lv_obj_set_size(use, button_width, 126);

  const lv_color_hsv_t hsv = lv_color_to_hsv(Color(picker->rgb));
  const double angle = hsv.h * kPi / 180.0;
  const double radius = (picker->wheel_size * 0.5 - 3.0) * hsv.s / 100.0;
  UpdateAccentPicker(picker, std::cos(angle) * radius,
                     std::sin(angle) * radius);
  AnimateEnter(sheet, 0, 18);
}

void BuildTheme(Tools *state) {
  Header(state->screen, "Theme Engine",
         "Choose appearance, sizing, keyboard and accent for every AERA page.", state->callback,
         state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 460,
                       landscape ? 810 : 2200);
  if (landscape) {
    lv_obj_set_x(state->list, 884);
    lv_obj_set_width(state->list, 1400);
  }

  auto *preview = lv_obj_create(state->list);
  Panel(preview, 42, kMainPanel);
  lv_obj_set_pos(preview, 16, 0);
  lv_obj_set_size(preview, 1280, 310);
  auto *kicker = Kicker(preview, "LIVE PREVIEW", kAccent);
  lv_obj_set_pos(kicker, 42, 34);
  auto *title = Label(preview, AccentName(RecoveryAccentColor()),
                      &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 42, 92);
  auto *sample = Button(preview, "Primary action", [] {}, true);
  lv_obj_set_pos(sample, 820, 80);
  lv_obj_set_size(sample, 400, 126);
  auto *hint = Label(preview,
      "Buttons, toggles, sliders, progress, navigation and focus states",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 42, 232);

  auto *appearance = Label(state->list, "Appearance",
                        &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(appearance, 32, 380);

  struct ModePreset { const char *name; const char *detail; bool light; };
  const std::array<ModePreset, 2> modes{{
      {"Graphite", "Dark, textured and focused", false},
      {"Light", "Warm white with subtle grain", true},
  }};
  const bool selected_light = RecoveryLightMode();
  for (size_t i = 0; i < modes.size(); ++i) {
    const auto mode = modes[i];
    const bool selected = selected_light == mode.light;
    auto *card = lv_button_create(state->list);
    Clear(card);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 640, 450);
    lv_obj_set_size(card, 624, 170);
    lv_obj_set_style_radius(card, 32, 0);
    lv_obj_set_style_bg_color(card,
        mode.light ? Color(0xf2f0eb) : Color(0x24272c), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, selected ? 4 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_transform_scale(card, 250, LV_STATE_PRESSED);
    auto *name = Label(card, mode.name, &lv_font_montserrat_32,
        mode.light ? Color(0x171a1e) : Color(0xf5f7fa));
    lv_obj_set_pos(name, 32, 30);
    auto *description = Label(card, mode.detail, &lv_font_montserrat_24,
        mode.light ? Color(0x575d64) : Color(0xaeb6c0));
    lv_obj_set_pos(description, 32, 94);
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -32, 38);
    }
    OnClick(card, [state, mode] {
      if (!RecoverySetLightMode(mode.light)) {
        Sheet(state->screen, "Theme unavailable",
              "The selected appearance could not be stored.");
        return;
      }
      ApplySurfaceMode(mode.light);
      ApplyAccent(RecoveryAccentColor());
      Open(state, Action::kTheme);
    });
  }

  auto *size_section = Label(state->list, "Interface size",
                             &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(size_section, 32, 690);
  struct SizePreset {
    const char *name;
    const char *detail;
    InterfaceSize size;
  };
  const std::array<SizePreset, 3> sizes{{
      {"Small", "More content", InterfaceSize::kSmall},
      {"Normal", "AERA default", InterfaceSize::kNormal},
      {"Large", "Easier to read", InterfaceSize::kLarge},
  }};
  const InterfaceSize selected_size = RecoveryInterfaceSize();
  for (size_t i = 0; i < sizes.size(); ++i) {
    const auto preset = sizes[i];
    const bool selected = selected_size == preset.size;
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 426, 760);
    lv_obj_set_size(card, 410, 170);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, preset.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 28, 28);
    auto *detail = Label(card, preset.detail, &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(detail, 28, 96);
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -28, 34);
    }
    OnClick(card, [state, preset] {
      if (!RecoverySetInterfaceSize(preset.size)) {
        Sheet(state->screen, "Size unavailable",
              "The interface size could not be changed.");
        return;
      }
      ApplyInterfaceSize(static_cast<int>(preset.size));
      Open(state, Action::kTheme);
    });
  }

  auto *grid_section = Label(state->list, "Home plugin grid",
                             &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(grid_section, 32, 1000);
  struct GridPreset { const char *name; const char *detail; int columns; };
  const std::array<GridPreset, 2> grids{{
      {"2 x 3", "Wide cards - 6 apps per page", 2},
      {"3 x 3", "Compact cards - 9 apps per page", 3},
  }};
  const int selected_columns = RecoveryHomeGridColumns();
  for (size_t i = 0; i < grids.size(); ++i) {
    const auto grid = grids[i];
    const bool selected = selected_columns == grid.columns;
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 640, 1070);
    lv_obj_set_size(card, 624, 170);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, grid.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 32, 28);
    auto *detail = Label(card, grid.detail, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail, 32, 94);
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -32, 38);
    }
    OnClick(card, [state, grid] {
      if (!RecoverySetHomeGridColumns(grid.columns)) {
        Sheet(state->screen, "Layout unavailable",
              "The Home plugin grid could not be changed.");
        return;
      }
      Open(state, Action::kTheme);
    });
  }

  auto *keyboard_section = Label(state->list, "Keyboard layout",
                                 &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(keyboard_section, 32, 1310);
  struct KeyboardPreset {
    const char *name;
    const char *detail;
    KeyboardLayout layout;
  };
  const std::array<KeyboardPreset, 2> keyboard_layouts{{
      {"QWERTY", "International default", KeyboardLayout::kQwerty},
      {"QWERTZ", "Y and Z exchanged", KeyboardLayout::kQwertz},
  }};
  const KeyboardLayout selected_keyboard = RecoveryKeyboardLayout();
  for (size_t i = 0; i < keyboard_layouts.size(); ++i) {
    const auto preset = keyboard_layouts[i];
    const bool selected = selected_keyboard == preset.layout;
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 640, 1380);
    lv_obj_set_size(card, 624, 170);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, preset.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 32, 28);
    auto *detail = Label(card, preset.detail, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail, 32, 94);
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -32, 38);
    }
    OnClick(card, [state, preset] {
      if (!RecoverySetKeyboardLayout(preset.layout)) {
        Sheet(state->screen, "Keyboard unavailable",
              "The keyboard layout could not be stored.");
        return;
      }
      Open(state, Action::kTheme);
    });
  }

  auto *section = Label(state->list, "Accent palettes",
                        &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(section, 32, 1610);
  const uint32_t selected_rgb = RecoveryAccentColor();
  for (size_t i = 0; i < kAccentPresets.size(); ++i) {
    const auto preset = kAccentPresets[i];
    const int32_t column = static_cast<int32_t>(i % 4);
    const int32_t row = static_cast<int32_t>(i / 4);
    auto *card = lv_button_create(state->list);
    Clear(card);
    lv_obj_set_pos(card, 16 + column * 320, 1680 + row * 220);
    lv_obj_set_size(card, 304, 190);
    lv_obj_set_style_radius(card, 32, 0);
    lv_obj_set_style_bg_color(card, kMainPanel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, kMainSelected, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, selected_rgb == preset.rgb ? 4 : 1, 0);
    lv_obj_set_style_border_color(card,
        selected_rgb == preset.rgb ? Color(preset.rgb) : kMainLine, 0);
    lv_obj_set_style_transform_scale(card, 250, LV_STATE_PRESSED);

    auto *swatch = lv_obj_create(card);
    Clear(swatch);
    lv_obj_set_size(swatch, 72, 72);
    lv_obj_set_pos(swatch, 24, 24);
    lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(swatch, Color(preset.rgb), 0);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
    auto *name = Label(card, preset.name, &lv_font_montserrat_24, kText);
    lv_obj_set_pos(name, 24, 116);
    if (selected_rgb == preset.rgb) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32,
                          Color(preset.rgb));
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -28, 38);
    }
    OnClick(card, [state, preset] {
      if (!RecoverySetAccentColor(preset.rgb)) {
        Sheet(state->screen, "Theme unavailable",
              "The selected accent could not be stored.");
        return;
      }
      ApplyAccent(preset.rgb);
      Open(state, Action::kTheme);
    });
  }

  const bool custom_selected = !IsPresetAccent(selected_rgb);
  auto *custom = lv_button_create(state->list);
  Clear(custom);
  lv_obj_set_pos(custom, 16 + 3 * 320, 1680 + 220);
  lv_obj_set_size(custom, 304, 190);
  lv_obj_set_style_radius(custom, 32, 0);
  lv_obj_set_style_bg_color(custom, kMainPanel, 0);
  lv_obj_set_style_bg_opa(custom, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(custom, kMainSelected, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(custom, custom_selected ? 4 : 1, 0);
  lv_obj_set_style_border_color(custom,
      custom_selected ? Color(selected_rgb) : kMainLine, 0);
  lv_obj_set_style_transform_scale(custom, 250, LV_STATE_PRESSED);
  auto *disk = HueSaturationDisk(custom, 72);
  lv_obj_set_pos(disk, 24, 24);
  auto *custom_name = Label(custom, "User select",
                            &lv_font_montserrat_24, kText);
  lv_obj_set_pos(custom_name, 24, 116);
  if (custom_selected) {
    auto *selected = lv_obj_create(custom);
    Clear(selected);
    lv_obj_set_size(selected, 38, 38);
    lv_obj_align(selected, LV_ALIGN_TOP_RIGHT, -28, 38);
    lv_obj_set_style_radius(selected, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(selected, Color(selected_rgb), 0);
    lv_obj_set_style_bg_opa(selected, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(selected, 3, 0);
    lv_obj_set_style_border_color(selected, kText, 0);
    lv_obj_remove_flag(selected, LV_OBJ_FLAG_CLICKABLE);
  }
  OnClick(custom, [state] { OpenAccentPicker(state); });

  auto *note = Label(state->list,
      "AERA Cyan is the default. Choose User select for any colour. Changes\n"
      "apply immediately and are saved with your recovery preferences.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(note, 32, 2160);
  lv_obj_set_width(note, 1220);

  auto *reset = Button(state->list, "Reset to AERA Cyan", [state] {
    if (!RecoverySetAccentColor(kDefaultAccentRgb)) {
      Sheet(state->screen, "Theme unavailable",
            "The default accent could not be restored.");
      return;
    }
    ApplyAccent(kDefaultAccentRgb);
    Open(state, Action::kTheme);
  });
  lv_obj_set_pos(reset, 16, 2290);
  lv_obj_set_size(reset, 1280, 124);

  auto *dock_section = Label(state->list, "Navigation dock",
                             &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(dock_section, 32, 2500);
  const std::array<std::pair<const char *, DockLayout>, 3> dock_modes{{
      {"Glass", DockLayout::kGlass}, {"Compact", DockLayout::kCompact},
      {"Minimal", DockLayout::kMinimal}}};
  for (size_t i = 0; i < dock_modes.size(); ++i) {
    const auto mode = dock_modes[i];
    const bool selected = RecoveryDockLayout() == mode.second;
    auto *card = Button(state->list, mode.first, [state, mode] {
      RecoverySetDockLayout(mode.second);
      Open(state, Action::kTheme);
    }, selected);
    lv_obj_set_pos(card, 16 + static_cast<int>(i) * 426, 2570);
    lv_obj_set_size(card, 408, 128);
    lv_obj_set_style_radius(card, 34, 0);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_border_opa(card, selected ? LV_OPA_COVER : LV_OPA_40, 0);
  }

  auto dock_slider = [state](int y, const char *name, const char *description,
                             int value, bool blur) {
    auto *card = lv_obj_create(state->list);
    Panel(card, 32, kMainPanel);
    lv_obj_set_pos(card, 16, y);
    lv_obj_set_size(card, 1280, 220);
    auto *title = Label(card, name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(title, 36, 24);
    auto *copy = Label(card, description, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(copy, 36, 76);
    auto *amount = Label(card, (std::to_string(value) + "%").c_str(),
                         &lv_font_montserrat_24, kAccent);
    lv_obj_align(amount, LV_ALIGN_TOP_RIGHT, -38, 34);
    auto *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 52, 166);
    lv_obj_set_size(slider, 1176, 20);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    RangeSlider(slider);
    struct DockBinding { lv_obj_t *amount; bool blur; };
    auto *binding = new DockBinding{amount, blur};
    lv_obj_add_event_cb(slider, [](lv_event_t *event) {
      auto *binding = static_cast<DockBinding *>(lv_event_get_user_data(event));
      if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        delete binding;
        return;
      }
      if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
      const int value = lv_slider_get_value(lv_event_get_target_obj(event));
      i18n::BindLabel(binding->amount,
                        (std::to_string(value) + "%").c_str());
      if (binding->blur) RecoverySetDockBlur(value);
      else RecoverySetDockTransparency(value);
    }, LV_EVENT_ALL, binding);
  };
  dock_slider(2730, "Transparency",
              "0% is solid; 100% leaves only the controls visible",
              RecoveryDockTransparency(), false);
  dock_slider(2970, "Backdrop blur",
              "GPU-friendly live blur behind the dock surface",
              RecoveryDockBlur(), true);

  const bool hide_apps = RecoveryDockHideInApps();
  auto *hide = Button(state->list,
      hide_apps ? "Hide dock inside apps: on" : "Hide dock inside apps: off",
      [state, hide_apps] {
        RecoverySetDockHideInApps(!hide_apps);
        Open(state, Action::kTheme);
      }, hide_apps);
  lv_obj_set_pos(hide, 16, 3210);
  lv_obj_set_size(hide, 1280, 128);
  auto *hide_note = Label(state->list,
      "Installed plugins use the full screen. Edge-swipe or hardware Back still works.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hide_note, 32, 3370);
  lv_obj_set_width(hide_note, 1220);

  auto *save = Button(state->screen, "Save theme", [state] {
    const bool saved = RecoverySavePreferences();
    Sheet(state->screen, saved ? "Theme saved" : "Could not save theme",
          saved ? "Your AERA theme will be restored on the next boot." :
                  "The theme remains active for this session. Unlock settings storage and try again.");
  }, true);
  lv_obj_set_pos(save, landscape ? 884 : 80,
                 landscape ? 1170 : 2740);
  lv_obj_set_size(save, landscape ? 1400 : 1280,
                  landscape ? 100 : 132);
}

void BuildPreferences(Tools *state) {
  Header(state->screen, "Preferences", "Display, files, backups and connection settings.", state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 480,
                       landscape ? 810 : 2210);
  if (landscape) {
    lv_obj_set_x(state->list, 884);
    lv_obj_set_width(state->list, 1400);
  }
  PreferenceSection(state, 0, "Display & time");
  auto *brightness = lv_obj_create(state->list);
  Panel(brightness, 36, kMainPanel);
  lv_obj_set_pos(brightness, 16, 76);
  lv_obj_set_size(brightness, 1280, 246);
  auto *brightness_icon = Label(brightness, LV_SYMBOL_EYE_OPEN,
                                &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(brightness_icon, 34, 34);
  auto *title = Label(brightness, "Brightness", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(title, 100, 28);
  auto *subtitle = Label(brightness, "Display level changes as you drag",
                         &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, 100, 80);
  auto *value_plate = lv_obj_create(brightness);
  Panel(value_plate, 30, kAccentSoft);
  lv_obj_set_pos(value_plate, 1090, 25);
  lv_obj_set_size(value_plate, 146, 72);
  auto *value = Label(value_plate,
                      (std::to_string(RecoveryBrightness()) + "%").c_str(),
                      &lv_font_montserrat_32, kAccent);
  lv_obj_center(value);
  auto *slider = lv_slider_create(brightness);
  lv_obj_set_pos(slider, 52, 176);
  lv_obj_set_size(slider, 1176, 22);
  lv_slider_set_range(slider, 10, 100);
  lv_slider_set_value(slider, std::max(10, RecoveryBrightness()), LV_ANIM_OFF);
  RangeSlider(slider);
  lv_obj_add_event_cb(slider, [](lv_event_t *event) {
    auto *target = lv_event_get_target_obj(event);
    const int percent = lv_slider_get_value(target);
    i18n::BindLabel(static_cast<lv_obj_t *>(lv_event_get_user_data(event)),
                       (std::to_string(percent) + "%").c_str());
    RecoverySetBrightness(percent);
  }, LV_EVENT_VALUE_CHANGED, value);

  PreferenceToggle(state, 340, "24-hour clock", "Off uses 12-hour time with AM / PM", Preference::kClock24);
  auto *zone = Label(state->list, "", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(zone, 116, 562);
  auto refresh_zone = [zone] {
    const int offset = RecoveryUtcOffset(), magnitude = std::abs(offset);
    char text[64];
    snprintf(text, sizeof(text), "UTC %c%02d:%02d", offset < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
    i18n::BindLabel(zone, text);
  };
  refresh_zone();
  auto *zone_hint = Label(state->list, "Fixed offset, 15-minute steps; no automatic DST.", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(zone_hint, 116, 636);
  for (int direction : {-1, 1}) {
    auto *button = Button(state->list, direction < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS,
        [state, direction, refresh_zone] {
      const int next = std::clamp(RecoveryUtcOffset() + 15 * direction, -720, 840);
      if (!RecoverySetUtcOffset(next))
        Sheet(state->screen, "Time offset unavailable", "The time offset could not be changed.");
      refresh_zone();
    });
    lv_obj_set_pos(button, direction < 0 ? 996 : 1156, 534);
    lv_obj_set_size(button, 132, 112);
  }
  PreferenceSection(state, 780, "Files & installation");
  PreferenceToggle(state, 860, "Show hidden files", "Include dot-prefixed files and folders", Preference::kHiddenFiles);
  PreferenceToggle(state, 1050, "Verify ZIP signatures", "Only install packages signed by a trusted recovery key", Preference::kVerifyZip);
  PreferenceSection(state, 1290, "Backup & restore");
  PreferenceToggle(state, 1370, "Compress backups by default", "Smaller archives; backup and restore may take longer", Preference::kCompression);
  if (RecoverySha256Available())
    PreferenceToggle(state, 1560, "SHA-256 backup checksums", "On: SHA-256 / Off: legacy MD5 checksums", Preference::kSha256);
  auto *integrity = Label(state->list,
      "Backup checksums are always generated. Restore verification stays on.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(integrity, 116, 1760);
  lv_obj_set_width(integrity, 1120);
  PreferenceSection(state, 1910, "USB connection");
  auto *mtp = Button(state->list, RecoveryMtpEnabled() ? "USB file transfer: on" :
                       "USB file transfer: off", [] {});
  auto *mtp_label = lv_obj_get_child(mtp, 0);
  OnClick(mtp, [state, mtp_label] {
    if (!RecoverySetMtp(!RecoveryMtpEnabled()))
      Sheet(state->screen, "USB transfer unavailable", "Check that storage is unlocked and mounted.");
    i18n::BindLabel(mtp_label, RecoveryMtpEnabled() ? "USB file transfer: on" : "USB file transfer: off");
  });
  lv_obj_set_pos(mtp, 32, 2010);
  lv_obj_set_size(mtp, 1248, 132);
  auto *hint = Label(state->list,
      "MTP makes accessible storage available to your computer.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 48, 2190);
  lv_obj_set_width(hint, 1190);
  if (RecoveryHapticsAvailable()) {
    PreferenceSection(state, 2310, "Haptics");
    HapticSlider(state, 2390, "Touch feedback",
                 "Buttons, cards and navigation", Haptic::kTouch, 300);
    HapticSlider(state, 2630, "Keyboard feedback",
                 "Keys in PIN, Wi-Fi and text entry", Haptic::kKeyboard, 300);
    HapticSlider(state, 2870, "Operation feedback",
                 "A stronger pulse when recovery work finishes", Haptic::kAction, 500);
    auto *test = Button(state->list, "Test operation vibration", [] {
      RecoveryVibrate(Haptic::kAction);
    });
    lv_obj_set_pos(test, 32, 3110);
    lv_obj_set_size(test, 1248, 124);
  }
  auto *save_hint = Label(state->list,
      "Changes apply now. Save to keep preferences after reboot.\nSettings storage must be available to save.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(save_hint, 48, RecoveryHapticsAvailable() ? 3300 : 2310);
  lv_obj_set_width(save_hint, 1190);
  auto *save = Button(state->screen, "Save preferences", [state] {
    const bool saved = RecoverySavePreferences();
    Sheet(state->screen, saved ? "Preferences saved" : "Could not save preferences",
          saved ? "Your preferences have been saved for the next recovery session." :
                  "Changes still apply to this session. Unlock and mount settings storage, then try again.");
  }, true);
  lv_obj_set_pos(save, landscape ? 884 : 80,
                 landscape ? 1170 : 2740);
  lv_obj_set_size(save, landscape ? 1400 : 1280,
                  landscape ? 100 : 132);
}

void BuildLogs(Tools *state) {
  Header(state->screen, "Recovery log", "Latest output from this recovery session.", state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 490 : 620,
                       landscape ? 760 : 2130);
  auto *text = Label(state->list, ReadLog().c_str(), &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_width(text, 1270);
  auto *refresh = Button(state->screen, "Refresh", [state, text] {
    i18n::BindLabel(text, ReadLog().c_str());
    lv_obj_scroll_to_y(state->list, LV_COORD_MAX, LV_ANIM_OFF);
  });
  lv_obj_set_pos(refresh, 80, landscape ? 340 : 452);
  lv_obj_set_size(refresh, landscape ? 560 : 1280, 120);
}

void BuildLanguage(Tools *state) {
  Header(state->screen, "Language",
         "Mostly machine-translated. Please report mistakes.",
         state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 452,
                       landscape ? 930 : 2290);
  lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_ON);
  lv_obj_set_style_width(state->list, 14, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(state->list, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(state->list, LV_RADIUS_CIRCLE,
                          LV_PART_SCROLLBAR);
  if (landscape) {
    lv_obj_set_x(state->list, 884);
    lv_obj_set_width(state->list, 1400);
  }
  lv_obj_update_layout(state->list);
  const int row_width =
      std::max(600, static_cast<int>(lv_obj_get_width(state->list)) - 28);
  const auto &languages = i18n::AvailableLanguages();
  for (size_t i = 0; i < languages.size(); ++i) {
    const auto language = languages[i];
    const bool selected =
        std::string(i18n::CurrentLanguage()) == language.code;
    auto *row = Button(state->list, "", [] {}, selected);
    lv_obj_set_pos(row, 0, static_cast<int>(i) * 154);
    lv_obj_set_size(row, row_width, 142);
    lv_obj_set_style_radius(row, 22, 0);

    auto *native = Label(row, language.native_name, &lv_font_montserrat_32,
                         kText);
    lv_obj_set_pos(native, 28, 20);
    lv_obj_set_width(native, row_width - 180);
    lv_label_set_long_mode(native, LV_LABEL_LONG_DOT);
    auto *english = Label(row, language.name, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(english, 30, 80);
    lv_obj_set_width(english, row_width - 180);
    if (selected) {
      auto *check = Label(row, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_RIGHT_MID, -38, 0);
    }
    OnClick(row, [state, language] {
      if (!RecoverySetLanguage(language.code) ||
          !i18n::SetLanguage(language.code)) {
        Sheet(state->screen, "Language unavailable",
              "AERA could not apply the selected language.");
        return;
      }
      RecoverySavePreferences();
      Open(state, Action::kLanguage);
    });
  }
}

const char *CredentialName(int type) {
  if (type == 2) return "Pattern";
  if (type == 3) return "PIN";
  if (type == 0) return "No credential";
  return "Password";
}

void BuildUsers(Tools *state) {
  Header(state->screen, "Android Users",
         "Unlock additional Android users only when you need them.",
         state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 452,
                       landscape ? 930 : 2290);
  const auto users = RecoveryAndroidUsers();
  lv_obj_update_layout(state->list);
  const int row_width =
      std::max(600, static_cast<int>(lv_obj_get_width(state->list)));
  for (size_t i = 0; i < users.size(); ++i) {
    const auto user = users[i];
    auto *row = Button(state->list, "", [] {});
    lv_obj_set_pos(row, 0, static_cast<int>(i) * 170);
    lv_obj_set_size(row, row_width, 154);
    lv_obj_set_style_radius(row, 24, 0);
    auto *name = Label(row, user.name.c_str(), &lv_font_montserrat_36, kText);
    lv_obj_set_pos(name, 28, 20);
    lv_obj_set_width(name, row_width - 330);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const std::string detail = i18n::Format(
        "User %d / %s", user.id, CredentialName(user.credential_type));
    auto *credential =
        Label(row, detail.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(credential, 30, 82);
    lv_obj_set_width(credential, row_width - 330);
    auto *status = Label(row, user.decrypted ? "Unlocked" : "Unlock",
                         &lv_font_montserrat_24,
                         user.decrypted ? kGreen : kAccent);
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, -68, 0);
    if (user.decrypted) {
      lv_obj_add_state(row, LV_STATE_DISABLED);
      lv_obj_set_style_opa(row, LV_OPA_80, LV_STATE_DISABLED);
    } else {
      auto *arrow =
          Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kAccent);
      lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -22, 0);
      OnClick(row, [state, user] {
        SetUserDecryptRequest({user});
        Open(state, Action::kDecryptUser);
      });
    }
  }
  if (users.empty()) {
    auto *empty = Label(
        state->list,
        "No Android users were found. Unlock /data first, then try again.",
        &lv_font_montserrat_32, kMuted);
    lv_obj_set_width(empty, row_width - 40);
    lv_obj_set_pos(empty, 20, 80);
  }
}

void BuildMenu(Tools *state) {
  Header(state->screen, "Menu", "Tools for your recovery session.",
         state->callback, state->context);
  // Utility cards scroll. About and Reboot are full-width destinations pinned
  // above Navigation, with About immediately above the final Reboot action.
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 452,
                       landscape ? 438 : 1900);
  struct Item { const char *icon, *title, *detail; Action action; };
  const std::array<Item, 12> items{{
    {LV_SYMBOL_DRIVE, "Mounts", "Mount or unmount recovery volumes", Action::kMounts},
    {LV_SYMBOL_USB, "ADB Sideload", "Receive and install a ZIP package over USB", Action::kSideload},
    {LV_SYMBOL_LIST, "Recovery log", "Read output and troubleshoot operations", Action::kLogs},
    {LV_SYMBOL_WIFI, "Wi-Fi", "Networks, saved credentials and connection test", Action::kWifi},
    {LV_SYMBOL_SHUFFLE, "Network Storage", "Connect and mount SFTP or SMB storage", Action::kNas},
    {LV_SYMBOL_SETTINGS, "Root Manager", "Patch init_boot and manage KernelSU modules", Action::kRootManager},
    {LV_SYMBOL_KEYBOARD, "Android Users", "Unlock additional users on demand", Action::kUsers},
    {LV_SYMBOL_TINT, "Theme Engine", "Global accent colors and interface appearance", Action::kTheme},
    {LV_SYMBOL_SETTINGS, "Preferences", "Display, files, backups, time and USB", Action::kPreferences},
    {"A", "Language", "Choose the recovery interface language", Action::kLanguage},
    {"A", "About AERA", "Project identity, contributors and build information", Action::kAbout},
    {LV_SYMBOL_POWER, "Reboot", "Android, recovery, bootloader or power off", Action::kOpenReboot}}};
  for (size_t i = 0; i < items.size(); ++i) {
    const auto item = items[i];
    const bool reboot = item.action == Action::kOpenReboot;
    const bool about = item.action == Action::kAbout;
    const bool pinned = about || reboot;
    const int columns = landscape ? 3 : 2;
    const int column = static_cast<int>(i % columns);
    const int row = static_cast<int>(i / columns);
    auto *card = lv_button_create(pinned ? state->screen : state->list);
    Panel(card, 34, kMainSheet);
    Interactive(card, kMainSelected);
    // These two deliberate destinations form a full-width stack immediately
    // above the four-button navigation dock.
    const int card_width = landscape ? 992 : 640;
    const int card_step = landscape ? 284 : 306;
    const int card_height = landscape ? 260 : 280;
    const int pinned_y = about ? (landscape ? 798 : 2398)
                               : (landscape ? 1010 : 2648);
    lv_obj_set_pos(card, pinned ? 64 : column * (landscape ? 1016 : 672),
                   pinned ? pinned_y : row * card_step);
    lv_obj_set_size(card, pinned ? (landscape ? 3040 : 1312) : card_width,
                    pinned ? (landscape ? 190 : about ? 218 : 232)
                           : card_height);
    lv_obj_set_style_transform_scale(card, 256, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, pinned ? 0 : 1, 0);
    lv_obj_set_style_border_color(card, reboot ? kRed : kMainLine, 0);
    lv_obj_set_style_border_opa(card,
                                reboot ? LV_OPA_TRANSP : LV_OPA_30, 0);
    OnClick(card, [state, item] { Open(state, item.action); });

    auto *plate = lv_obj_create(card);
    Panel(plate, 22, reboot ? kRedSoft : kAccentSoft);
    lv_obj_set_pos(plate, 28, pinned ? (landscape ? 47 : about ? 55 : 68) : 28);
    lv_obj_set_size(plate, pinned ? 96 : 108, pinned ? 96 : 108);
    auto *icon = Label(plate, item.icon, &lv_font_montserrat_48,
                       reboot ? kRed : kAccent);
    lv_obj_center(icon);
    auto *title = Label(card, item.title, &lv_font_montserrat_36, kText);
    lv_obj_set_pos(title, pinned ? 152 : 164,
                   pinned ? (landscape ? 36 : about ? 42 : 40) : 42);
    lv_obj_set_width(title, pinned ? (landscape ? 2750 : 1040)
                                  : card_width - 252);
    FitLabelToLines(title,
                    pinned ? (landscape ? 2750 : 1040)
                           : card_width - 252,
                    1, {&lv_font_montserrat_36, &lv_font_montserrat_32,
                        &lv_font_montserrat_28, &lv_font_montserrat_24,
                        &lv_font_montserrat_20, &lv_font_montserrat_18,
                        &lv_font_montserrat_16});
    auto *detail = Label(card, item.detail, &lv_font_montserrat_32, kMuted);
    lv_obj_set_pos(detail, pinned ? 152 : 30,
                   pinned ? (landscape ? 112 : about ? 122 : 126)
                          : (landscape ? 150 : 164));
    FitLabelToLines(detail,
                    pinned ? (landscape ? 2750 : 1040)
                           : card_width - 100,
                    2, {&lv_font_montserrat_32, &lv_font_montserrat_28,
                        &lv_font_montserrat_24, &lv_font_montserrat_20,
                        &lv_font_montserrat_18, &lv_font_montserrat_16});
    auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_36,
                        reboot ? kRed : kMutedStrong);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -30, 0);
    AnimateEnter(card, 30 + static_cast<uint32_t>(i) * 28, 12);
  }
}
}  // namespace

void SetUserDecryptRequest(const UserDecryptRequest &request) {
  selected_user_decrypt = request;
}

UserDecryptRequest GetUserDecryptRequest() {
  return selected_user_decrypt;
}

void BuildToolScene(lv_obj_t *screen, Action tool, ActionCallback callback, void *context) {
  auto *state = new Tools;
  state->screen = screen;
  state->tool = tool;
  state->compression = RecoveryPreference(Preference::kCompression);
  state->callback = callback;
  state->context = context;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<Tools *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, state);
  if (tool == Action::kBackup || tool == Action::kWipe || tool == Action::kRestore)
    BuildPartitions(state);
  else if (tool == Action::kFormatData) BuildFormatData(state);
  else if (tool == Action::kMounts) BuildMounts(state);
  else if (tool == Action::kPreferences) BuildPreferences(state);
  else if (tool == Action::kLanguage) BuildLanguage(state);
  else if (tool == Action::kTheme) BuildTheme(state);
  else if (tool == Action::kLogs) BuildLogs(state);
  else if (tool == Action::kUsers) BuildUsers(state);
  else BuildMenu(state);
  Navigation(screen, tool == Action::kBackup || tool == Action::kRestore ? Action::kBackup :
                     tool == Action::kWipe || tool == Action::kFormatData ? Action::kWipe : Action::kSettings,
                     callback, context);
}

void BuildFastbootFormatScene(lv_obj_t *screen, ActionCallback callback,
                              void *context) {
  auto *state = new Tools;
  state->screen = screen;
  state->tool = Action::kFormatData;
  state->callback = callback;
  state->context = context;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<Tools *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, state);
  BuildFormatData(state, true);
}
}  // namespace recovery_ui2
