/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "phone_keyboard.hpp"
#include "ui_components.hpp"
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace recovery_ui2 {
namespace {
using namespace widgets;
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
  return "Erase " + path;
}

void UpdateSelection(Tools *state) {
  uint64_t bytes = 0;
  for (const auto &v : state->volumes)
    if (state->selected.count(v.path)) bytes += v.bytes;
  std::string summary = std::to_string(state->selected.size()) + " selected";
  if (state->tool == Action::kBackup) summary = "Estimated backup: " + Size(bytes);
  lv_label_set_text(state->summary, summary.c_str());
  if (state->review) {
    if (state->selected.empty()) lv_obj_add_state(state->review, LV_STATE_DISABLED);
    else lv_obj_remove_state(state->review, LV_STATE_DISABLED);
  }
  if (state->selection_detail) {
    std::string detail = std::to_string(state->selected.size()) +
        (state->selected.size() == 1 ? " partition selected" : " partitions selected");
    if (state->tool == Action::kBackup)
      detail += state->compression ? " / Before compression; archive size varies" : " / Archive overhead not included";
    else if (!state->restore_folder.empty())
      detail = state->restore_folder.substr(state->restore_folder.find_last_of('/') + 1);
    lv_label_set_text(state->selection_detail, detail.c_str());
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
      lv_obj_set_style_text_font(check, &lv_font_montserrat_32, LV_PART_MAIN);
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
  lv_label_set_text(state->summary, (std::to_string(folders.size()) + " saved backups").c_str());
  if (state->selection_detail) lv_label_set_text(state->selection_detail, "Choose a backup, then select partitions to restore");
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
  state->format_input = lv_textarea_create(state->screen);
  lv_obj_set_pos(state->format_input, 80, landscape ? 960 : 1310);
  lv_obj_set_size(state->format_input, landscape ? 940 : 1280, 150);
  lv_textarea_set_one_line(state->format_input, true);
  // Don't truncate to three letters: "yesplease" must not become "yes".
  lv_textarea_set_max_length(state->format_input, 32);
  lv_textarea_set_text(state->format_input, "");
  lv_textarea_set_placeholder_text(state->format_input, "yes");
  lv_obj_set_style_text_font(state->format_input, &lv_font_montserrat_48, 0);
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
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, 0);
  lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_32, LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, kText, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, kMainPanel, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, kMainSelected, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_radius(keyboard, 18, LV_PART_ITEMS);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_pad_all(keyboard, 16, 0);
  lv_obj_set_style_pad_row(keyboard, 18, 0);
  lv_obj_set_style_pad_column(keyboard, 10, 0);

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
    lv_obj_set_style_text_font(state->summary, &lv_font_montserrat_48, 0);
    state->selection_detail = Label(state->screen, "", &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(state->selection_detail, 80, landscape ? 830 : 884);
    lv_obj_set_width(state->selection_detail, landscape ? 940 : 1280);
    lv_label_set_long_mode(state->selection_detail, LV_LABEL_LONG_DOT);
    if (!landscape) {
      lv_obj_set_pos(state->list, 64, 954);
      lv_obj_set_height(state->list, 1550);
    }
    auto *storage = Button(state->screen, "Choose storage  " LV_SYMBOL_DOWN,
                            [state] { StorageChooser(state); });
    lv_obj_set_pos(storage, 64, landscape ? 490 : 608);
    lv_obj_set_size(storage, landscape ? 470 : 630, 112);
    std::string storage_text = RecoveryStorage();
    struct statvfs capacity{};
    if (statvfs(storage_text.c_str(), &capacity) == 0)
      storage_text += " / " + Size(uint64_t(capacity.f_bavail) * capacity.f_frsize) + " free";
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
        lv_label_set_text(label, state->compression ? "Compression: on" : "Compression: off");
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
  auto volumes = RecoveryVolumes("mount");
  for (size_t i = 0; i < volumes.size(); ++i) {
    const auto v = volumes[i];
    Row(state->list, static_cast<int>(i) * 190, LV_SYMBOL_DRIVE, v.name,
        v.path + (v.selected ? "  /  Mounted" : "  /  Unmounted"), [state, v] {
      JobRequest request;
      request.job = v.selected ? Job::kUnmount : Job::kMount;
      request.path = v.path;
      request.title = v.selected ? "Unmount volume" : "Mount volume";
      Sheet(state->screen, request.title, v.name + "\n\n" + v.path +
            "\n\nRecovery's configured mount and read-only rules apply.",
            [state, request] { Run(state, request); });
    }, v.selected ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
  }
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
    lv_label_set_text(binding->value, text.c_str());
    if (code == LV_EVENT_RELEASED) RecoveryVibrate(binding->haptic);
  }, LV_EVENT_ALL, binding);
}

struct AccentPreset {
  const char *name;
  uint32_t rgb;
};

constexpr std::array<AccentPreset, 8> kAccentPresets{{
    {"AERA Cyan", 0x16c8ff}, {"Azure", 0x4c8dff},
    {"Violet", 0xa991ff}, {"Magenta", 0xf05dce},
    {"Rose", 0xff5c7a}, {"Orange", 0xff8a32},
    {"Emerald", 0x42d392}, {"Lime", 0xa6e35a},
}};

const char *AccentName(uint32_t rgb) {
  for (const auto &preset : kAccentPresets)
    if (preset.rgb == rgb) return preset.name;
  return "Custom";
}

void BuildTheme(Tools *state) {
  Header(state->screen, "Theme Engine",
         "Choose the surface and accent for every AERA page.", state->callback,
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

  auto *section = Label(state->list, "Accent palettes",
                        &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(section, 32, 690);
  const uint32_t selected_rgb = RecoveryAccentColor();
  for (size_t i = 0; i < kAccentPresets.size(); ++i) {
    const auto preset = kAccentPresets[i];
    const int32_t column = static_cast<int32_t>(i % 4);
    const int32_t row = static_cast<int32_t>(i / 4);
    auto *card = lv_button_create(state->list);
    Clear(card);
    lv_obj_set_pos(card, 16 + column * 320, 760 + row * 220);
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

  auto *note = Label(state->list,
      "AERA Cyan is the default. Palette changes apply immediately and are\n"
      "saved with the rest of your recovery preferences.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(note, 32, 1240);
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
  lv_obj_set_pos(reset, 16, 1370);
  lv_obj_set_size(reset, 1280, 124);

  auto *dock_section = Label(state->list, "Navigation dock",
                             &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(dock_section, 32, 1580);
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
    lv_obj_set_pos(card, 16 + static_cast<int>(i) * 426, 1650);
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
      lv_label_set_text(binding->amount,
                        (std::to_string(value) + "%").c_str());
      if (binding->blur) RecoverySetDockBlur(value);
      else RecoverySetDockTransparency(value);
    }, LV_EVENT_ALL, binding);
  };
  dock_slider(1810, "Transparency",
              "0% is solid; 100% leaves only the controls visible",
              RecoveryDockTransparency(), false);
  dock_slider(2050, "Backdrop blur",
              "GPU-friendly live blur behind the dock surface",
              RecoveryDockBlur(), true);

  const bool hide_apps = RecoveryDockHideInApps();
  auto *hide = Button(state->list,
      hide_apps ? "Hide dock inside apps: on" : "Hide dock inside apps: off",
      [state, hide_apps] {
        RecoverySetDockHideInApps(!hide_apps);
        Open(state, Action::kTheme);
      }, hide_apps);
  lv_obj_set_pos(hide, 16, 2290);
  lv_obj_set_size(hide, 1280, 128);
  auto *hide_note = Label(state->list,
      "Installed plugins use the full screen. Edge-swipe or hardware Back still works.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hide_note, 32, 2450);
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
    lv_label_set_text(static_cast<lv_obj_t *>(lv_event_get_user_data(event)),
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
    lv_label_set_text(zone, text);
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
    lv_label_set_text(mtp_label, RecoveryMtpEnabled() ? "USB file transfer: on" : "USB file transfer: off");
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
    lv_label_set_text(text, ReadLog().c_str());
    lv_obj_scroll_to_y(state->list, LV_COORD_MAX, LV_ANIM_OFF);
  });
  lv_obj_set_pos(refresh, 80, landscape ? 340 : 452);
  lv_obj_set_size(refresh, landscape ? 560 : 1280, 120);
}

void BuildMenu(Tools *state) {
  Header(state->screen, "Menu", "Tools for your recovery session.", state->callback, state->context);
  // Only the six utility cards scroll. Reboot is pinned above Navigation so
  // it always reads as the final action on the page.
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 452,
                       landscape ? 610 : 2130);
  struct Item { const char *icon, *title, *detail; Action action; };
  const std::array<Item, 8> items{{
    {LV_SYMBOL_DRIVE, "Mounts", "Mount or unmount recovery volumes", Action::kMounts},
    {LV_SYMBOL_LIST, "Recovery log", "Read output and troubleshoot operations", Action::kLogs},
    {LV_SYMBOL_WIFI, "Wi-Fi", "Networks, saved credentials and connection test", Action::kWifi},
    {LV_SYMBOL_SHUFFLE, "Network Storage", "Connect and mount SFTP or SMB storage", Action::kNas},
    {LV_SYMBOL_SETTINGS, "Root Manager", "Patch init_boot and manage KernelSU modules", Action::kRootManager},
    {LV_SYMBOL_TINT, "Theme Engine", "Global accent colors and interface appearance", Action::kTheme},
    {LV_SYMBOL_SETTINGS, "Preferences", "Display, files, backups, time and USB", Action::kPreferences},
    {LV_SYMBOL_POWER, "Reboot", "Android, recovery, bootloader or power off", Action::kOpenReboot}}};
  for (size_t i = 0; i < items.size(); ++i) {
    const auto item = items[i];
    const bool reboot = item.action == Action::kOpenReboot;
    const int columns = landscape ? 3 : 2;
    const int column = static_cast<int>(i % columns);
    const int row = static_cast<int>(i / columns);
    auto *card = lv_button_create(reboot ? state->screen : state->list);
    Panel(card, 34, kMainSheet);
    Interactive(card, kMainSelected);
    // Reboot is a deliberate final destination, pinned immediately above the
    // four-button navigation dock instead of merely following the grid.
    const int card_width = landscape ? 992 : 640;
    lv_obj_set_pos(card, reboot ? 64 : column * (landscape ? 1016 : 672),
                   reboot ? (landscape ? 1030 : 2684) : row * 268);
    lv_obj_set_size(card, reboot ? (landscape ? 3040 : 1312) : card_width,
                    reboot ? (landscape ? 160 : 210) : 242);
    lv_obj_set_style_transform_scale(card, 256, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, reboot ? 0 : 1, 0);
    lv_obj_set_style_border_color(card, reboot ? kRed : kMainLine, 0);
    lv_obj_set_style_border_opa(card,
                                reboot ? LV_OPA_TRANSP : LV_OPA_30, 0);
    OnClick(card, [state, item] { Open(state, item.action); });

    auto *plate = lv_obj_create(card);
    Panel(plate, 22, reboot ? kRedSoft : kAccentSoft);
    lv_obj_set_pos(plate, 28, 28);
    lv_obj_set_size(plate, 84, 84);
    auto *icon = Label(plate, item.icon, &lv_font_montserrat_32,
                       reboot ? kRed : kAccent);
    lv_obj_center(icon);
    auto *title = Label(card, item.title, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(title, 136, 42);
    lv_obj_set_width(title, reboot ? (landscape ? 2750 : 1040)
                                  : card_width - 230);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    auto *detail = Label(card, item.detail, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail, reboot ? 136 : 30, reboot ? 116 : 140);
    lv_obj_set_width(detail, reboot ? (landscape ? 2750 : 1040)
                                   : card_width - 100);
    auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_32,
                        reboot ? kRed : kMutedStrong);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -30, 0);
    AnimateEnter(card, 30 + static_cast<uint32_t>(i) * 28, 12);
  }
}
}  // namespace

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
  else if (tool == Action::kTheme) BuildTheme(state);
  else if (tool == Action::kLogs) BuildLogs(state);
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
