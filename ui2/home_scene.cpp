/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "ui_components.hpp"
#include "picture_decode.hpp"
#include "picture_viewer.hpp"
#include <cerrno>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

namespace recovery_ui2 {
namespace {
using namespace widgets;
struct Entry { std::string name, path; bool directory; uint64_t bytes; };
struct Files {
  lv_obj_t *screen, *list, *path_label, *summary, *storage_label;
  ActionCallback callback;
  void *context;
  std::vector<Entry> entries;
  size_t visible = 100;
};
Files *gFiles = nullptr;
std::string gDirectory;
bool gPackagesOnly = false;

bool Zip(const std::string &name) {
  if (name.size() < 4) return false;
  std::string suffix = name.substr(name.size() - 4);
  for (char &c : suffix) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return suffix == ".zip";
}
bool Image(const std::string &name) {
  if (name.size() < 4) return false;
  std::string suffix = name.substr(name.size() - 4);
  for (char &c : suffix)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return suffix == ".img";
}
bool Package(const std::string &name) {
  return Zip(name) || Image(name) || plugins::IsPackageFile(name);
}

std::string Lower(std::string value) {
  for (char &character : value)
    character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
  return value;
}

std::string SuggestedImageTarget(const std::string &name) {
  const std::string lower = Lower(name);
  if (lower.find("init_boot") != std::string::npos) return "/init_boot";
  if (lower.find("vendor_boot") != std::string::npos) return "/vendor_boot";
  if (lower.find("abl") != std::string::npos) return "/abl";
  if (lower.find("dtbo") != std::string::npos) return "/dtbo";
  if (lower.find("recovery") != std::string::npos ||
      lower.find("twrp") != std::string::npos ||
      lower.find("orangefox") != std::string::npos ||
      lower.find("aera") != std::string::npos) return "/recovery";
  if (lower.find("boot") != std::string::npos) return "/boot";
  return {};
}

const char *ImageTargetDescription(const std::string &path) {
  if (path == "/init_boot") return "Early Android ramdisk and root patches";
  if (path == "/boot") return "Kernel and Android boot ramdisk";
  if (path == "/vendor_boot") return "Vendor ramdisk and device boot components";
  if (path == "/dtbo") return "Device-tree overlays used by the kernel";
  if (path == "/recovery") return "AERA or another compatible recovery image";
  if (path == "/abl") return "Android bootloader image — device-specific and high risk";
  return "Raw flashable partition";
}

std::string Parent(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  const auto slash = path.find_last_of('/');
  return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}
void Populate(Files *state);

void OpenImageTargetPicker(Files *state, const Entry &entry) {
  const auto targets = RecoveryImageVolumes();
  if (targets.empty()) {
    Sheet(state->screen, "No flashable partitions",
          "This device tree did not expose any partitions that safely accept raw images.");
    return;
  }

  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);

  const bool landscape = Landscape(state->screen);
  const int sheet_width = landscape
      ? std::min(2200, static_cast<int>(lv_obj_get_width(state->screen)) - 128)
      : 1312;
  const int sheet_height = landscape
      ? std::min(1280, static_cast<int>(lv_obj_get_height(state->screen)) - 80)
      : 2200;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, landscape ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID,
               0, landscape ? 0 : -40);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_20, 0);

  auto *grabber = lv_obj_create(sheet);
  Clear(grabber);
  lv_obj_set_size(grabber, 112, 8);
  lv_obj_align(grabber, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_radius(grabber, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(grabber, kMutedStrong, 0);
  lv_obj_set_style_bg_opa(grabber, LV_OPA_30, 0);

  auto *tag = Kicker(sheet, "SELECT TARGET PARTITION", kAccent);
  lv_obj_set_pos(tag, 48, 58);
  auto *title = Label(sheet, "Flash image", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 48, 118);
  const std::string subtitle_text =
      "Choose exactly where this raw image will be written. Active slot: " +
      RecoverySlot();
  auto *subtitle = Label(sheet, subtitle_text.c_str(),
                         &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(subtitle, 48, 188);

  auto *file = lv_obj_create(sheet);
  Panel(file, 26, kMainPanel);
  lv_obj_set_pos(file, 48, 264);
  lv_obj_set_size(file, sheet_width - 96, 210);
  auto *file_icon = Label(file, LV_SYMBOL_FILE, &lv_font_montserrat_48, kViolet);
  lv_obj_align(file_icon, LV_ALIGN_LEFT_MID, 32, 0);
  auto *file_name = Label(file, entry.name.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(file_name, 112, 40);
  SingleLineLabel(file_name, sheet_width - 280, &lv_font_montserrat_32);
  const std::string file_detail = Size(entry.bytes) + "  /  " + entry.path;
  auto *file_path = Label(file, file_detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(file_path, 112, 108);
  SingleLineLabel(file_path, sheet_width - 280, &lv_font_montserrat_24);

  const std::string active_slot = RecoverySlot();
  auto both_slots = std::make_shared<bool>(false);
  auto *slot_mode = lv_obj_create(sheet);
  Panel(slot_mode, 28, kInset);
  lv_obj_set_pos(slot_mode, 48, 504);
  lv_obj_set_size(slot_mode, sheet_width - 96, 136);
  const int mode_width = (sheet_width - 120) / 2;
  auto *current_slot = lv_button_create(slot_mode);
  Panel(current_slot, 22, kAccent);
  lv_obj_set_pos(current_slot, 12, 12);
  lv_obj_set_size(current_slot, mode_width, 112);
  auto *current_label = Label(current_slot,
      ("Current slot " + active_slot).c_str(), &lv_font_montserrat_24, kOnAccent);
  lv_obj_center(current_label);
  auto *both_slot_button = lv_button_create(slot_mode);
  Panel(both_slot_button, 22, kInset);
  lv_obj_set_pos(both_slot_button, mode_width + 12, 12);
  lv_obj_set_size(both_slot_button, mode_width, 112);
  auto *both_label = Label(both_slot_button, "Both slots A + B",
                           &lv_font_montserrat_24, kMutedStrong);
  lv_obj_center(both_label);
  OnClick(current_slot, [both_slots, current_slot, current_label,
                         both_slot_button, both_label] {
    *both_slots = false;
    lv_obj_set_style_bg_color(current_slot, kAccent, 0);
    lv_obj_set_style_bg_opa(current_slot, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(current_label, kOnAccent, 0);
    lv_obj_set_style_bg_color(both_slot_button, kInset, 0);
    lv_obj_set_style_text_color(both_label, kMutedStrong, 0);
  });
  OnClick(both_slot_button, [both_slots, current_slot, current_label,
                             both_slot_button, both_label] {
    *both_slots = true;
    lv_obj_set_style_bg_color(current_slot, kInset, 0);
    lv_obj_set_style_text_color(current_label, kMutedStrong, 0);
    lv_obj_set_style_bg_color(both_slot_button, kAccent, 0);
    lv_obj_set_style_bg_opa(both_slot_button, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(both_label, kOnAccent, 0);
  });

  auto *list = lv_obj_create(sheet);
  Clear(list);
  lv_obj_set_pos(list, 48, 680);
  lv_obj_set_size(list, sheet_width - 96, sheet_height - 920);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 5, LV_PART_SCROLLBAR);

  auto select_target = [state, overlay, entry, both_slots](const Volume &target) {
      lv_obj_delete_async(overlay);
      JobRequest request;
      request.job = Job::kFlashImage;
      request.title = "Flash Image";
      request.path = entry.path;
      request.partitions = {target.path};
      request.both_slots = *both_slots;
      const std::string slot_destination = *both_slots
          ? "Both slots A + B"
          : "Current slot " + RecoverySlot();
      const std::string warning =
          "Image\n" + entry.name + "\n\nTarget\n" + target.name +
          "  /  " + target.path + "\n\nDestination\n" + slot_destination +
          "\n\nThis writes directly to the selected partition. An incorrect image or target can prevent the device from booting.";
      Sheet(state->screen, "Flash to " + target.name + "?", warning,
            [state, request] {
        SetJobRequest(request);
        state->callback(Action::kRunOperation, state->context);
      });
  };

  const int target_width = sheet_width - 96;
  auto add_target = [select_target, target_width](
      lv_obj_t *parent, int y, const Volume &target, bool recommended) {
    auto *row = lv_button_create(parent);
    Panel(row, 28, kMainPanel);
    Interactive(row, kMainSelected);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, target_width, 184);
    lv_obj_set_style_border_width(row, 0, 0);
    OnClick(row, [select_target, target] { select_target(target); });

    auto *icon = IconPlate(row, LV_SYMBOL_UPLOAD, kAccent,
                           recommended ? kAccentSoft : kMainSelected, 76);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 28, 0);
    auto *name = Label(row, target.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 132, 28);
    SingleLineLabel(name, target_width - 430, &lv_font_montserrat_32);
    auto *description = Label(row, ImageTargetDescription(target.path),
                              &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(description, 132, 92);
    SingleLineLabel(description, target_width - 300,
                    &lv_font_montserrat_24);
    if (recommended) {
      auto *tag = Kicker(row, "RECOMMENDED", kAccent);
      lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, -62, 28);
    }
    auto *arrow = Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kMutedStrong);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -28, 0);
  };

  const std::string suggestion = SuggestedImageTarget(entry.name);
  const auto recommended = std::find_if(
      targets.begin(), targets.end(), [&](const Volume &target) {
        return target.path == suggestion;
      });
  if (recommended != targets.end()) {
    auto *recommended_title = Label(list, "DETECTED FROM FILENAME",
                                    &lv_font_montserrat_18, kAccent);
    lv_obj_set_pos(recommended_title, 8, 8);
    lv_obj_set_style_text_letter_space(recommended_title, 3, 0);
    add_target(list, 58, *recommended, true);

    std::vector<Volume> advanced_targets;
    for (const auto &target : targets)
      if (target.path != recommended->path) advanced_targets.push_back(target);

    auto *advanced = lv_obj_create(list);
    Clear(advanced);
    lv_obj_set_pos(advanced, 0, 420);
    lv_obj_set_size(advanced, target_width,
                    static_cast<int>(advanced_targets.size()) * 196);
    for (size_t index = 0; index < advanced_targets.size(); ++index)
      add_target(advanced, static_cast<int>(index) * 196,
                 advanced_targets[index], false);
    lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);

    auto *toggle = lv_button_create(list);
    Panel(toggle, 28, kInset);
    Interactive(toggle, kMainSelected);
    lv_obj_set_pos(toggle, 0, 266);
    lv_obj_set_size(toggle, target_width, 120);
    const std::string show_text = "Show " +
        std::to_string(advanced_targets.size()) + " advanced partitions";
    auto *toggle_label = Label(toggle, show_text.c_str(),
                               &lv_font_montserrat_24, kMutedStrong);
    lv_obj_align(toggle_label, LV_ALIGN_LEFT_MID, 32, 0);
    auto *toggle_icon = Label(toggle, LV_SYMBOL_DOWN,
                              &lv_font_montserrat_24, kMutedStrong);
    lv_obj_align(toggle_icon, LV_ALIGN_RIGHT_MID, -32, 0);
    OnClick(toggle, [advanced, toggle_label, toggle_icon, show_text] {
      const bool hidden = lv_obj_has_flag(advanced, LV_OBJ_FLAG_HIDDEN);
      if (hidden) {
        lv_obj_remove_flag(advanced, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(toggle_label, "Hide advanced partitions");
        lv_label_set_text(toggle_icon, LV_SYMBOL_UP);
      } else {
        lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(toggle_label, show_text.c_str());
        lv_label_set_text(toggle_icon, LV_SYMBOL_DOWN);
        lv_obj_scroll_to_y(lv_obj_get_parent(advanced), 0, LV_ANIM_ON);
      }
    });
  } else {
    auto *available_title = Label(list, "AVAILABLE PARTITIONS",
                                  &lv_font_montserrat_18, kMuted);
    lv_obj_set_pos(available_title, 8, 8);
    lv_obj_set_style_text_letter_space(available_title, 3, 0);
    for (size_t index = 0; index < targets.size(); ++index)
      add_target(list, 58 + static_cast<int>(index) * 196,
                 targets[index], false);
  }

  auto *cancel = Button(sheet, "Cancel", [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_size(cancel, sheet_width - 96, 132);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -40);
  AnimateEnter(sheet, 0, 30);
}

void OpenFile(Files *state, const Entry &entry) {
  if (entry.directory) {
    gDirectory = entry.path;
    state->visible = 100;
    Populate(state);
    return;
  }
  if (IsPicture(entry.name)) {
    OpenPicture(state->screen, entry.path);
  } else if (plugins::IsPackageFile(entry.name)) {
    plugins::Plugin plugin;
    std::string error;
    if (!plugins::InspectLocalPackage(entry.path, plugin, error)) {
      Sheet(state->screen, "Cannot open plugin",
            error.empty() ? "This .aerap package is invalid." : error);
      return;
    }
    auto install = [state, entry, plugin](bool allow_unofficial) {
      plugins::Request request;
      request.job = plugins::Job::kInstallLocalStorage;
      request.id = plugin.id;
      request.path = entry.path;
      request.allow_unofficial = allow_unofficial;
      SetPluginRequest(request);
      state->callback(Action::kInstallLocalPlugin, state->context);
    };
    const std::string details = plugin.name + "\nVersion " + plugin.version +
        "\n\n" + plugin.description + "\n\nPackage: " +
        Size(entry.bytes) + "\nID: " + plugin.id;
    if (plugin.trust == plugins::Trust::kOfficial) {
      Sheet(state->screen, "Install official AERA app?",
            "OFFICIAL / SIGNATURE VERIFIED\n\n" + details,
            [install] { install(false); });
    } else {
      Sheet(state->screen, "Unofficial app warning",
            "This package is not signed by AERA. Its code will run with "
            "recovery privileges and may read, change, or erase device data.\n\n" +
            details + "\n\nOnly continue if you trust where this file came from.",
            [state, details, install] {
        Sheet(state->screen, "Install unofficial app?",
              "UNVERIFIED PUBLISHER\n\n" + details +
              "\n\nAERA cannot verify the developer or guarantee this package is safe.",
              [install] { install(true); });
      });
    }
  } else if (Zip(entry.name)) {
    JobRequest request;
    request.job = Job::kInstall;
    request.title = "Install ZIP";
    request.path = entry.path;
    Sheet(state->screen, "Install this package?",
          entry.name + "\n\n" + Size(entry.bytes) + "\n" + entry.path +
          "\n\nActive slot: " + RecoverySlot() +
          "\n\nThe package's installer can modify your system and data."
          "\nReview the file above before continuing.", [state, request] {
      SetJobRequest(request);
      state->callback(Action::kRunOperation, state->context);
    });
  } else if (Image(entry.name)) {
    OpenImageTargetPicker(state, entry);
  } else {
    Sheet(state->screen, entry.name,
          entry.path + "\n\nSize: " + Size(entry.bytes) +
          "\n\nOpen pictures, install ZIP packages, flash .img files, or install .aerap plugins. "
          "This file type has no preview.");
  }
}

void RenderEntries(Files *state) {
  lv_obj_clean(state->list);
  int y = 0;
  if (gDirectory != "/") {
    Row(state->list, y, LV_SYMBOL_UP, "Parent folder", Parent(gDirectory), [state] {
      gDirectory = Parent(gDirectory);
      state->visible = 100;
      Populate(state);
    });
    y += 180;
  }
  const size_t count = std::min(state->entries.size(), state->visible);
  for (size_t i = 0; i < count; ++i) {
    const auto entry = state->entries[i];
    Row(state->list, y, entry.directory ? LV_SYMBOL_DIRECTORY :
        IsPicture(entry.name) ? LV_SYMBOL_IMAGE :
        Image(entry.name) ? LV_SYMBOL_UPLOAD : LV_SYMBOL_FILE,
        entry.name, entry.directory ? "Folder" :
          Size(entry.bytes) + (plugins::IsPackageFile(entry.name) ?
                              "  /  AERA plugin package" :
                              Zip(entry.name) ? "  /  ZIP package" :
                              Image(entry.name) ? "  /  Flashable image" :
                              IsPicture(entry.name) ? "  /  Image preview" : "  /  File"),
        [state, entry] { OpenFile(state, entry); });
    y += 180;
  }
  if (count < state->entries.size()) {
    Row(state->list, y, LV_SYMBOL_PLUS, "Show more files", "Next 100 entries", [state] {
      state->visible += 100;
      RenderEntries(state);
    });
  }
}

void Populate(Files *state) {
  state->entries.clear();
  lv_label_set_text(state->path_label, gDirectory.c_str());
  DIR *directory = opendir(gDirectory.c_str());
  const int error = errno;
  if (directory) {
    while (auto *item = readdir(directory)) {
      if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
      if (item->d_name[0] == '.' && !RecoveryPreference(Preference::kHiddenFiles)) continue;
      const std::string path = (gDirectory == "/" ? "" : gDirectory) + "/" + item->d_name;
      struct stat info{};
      if (stat(path.c_str(), &info) != 0) continue;
      const bool folder = S_ISDIR(info.st_mode);
      if (!folder && !S_ISREG(info.st_mode)) continue;
      if (!folder && gPackagesOnly && !Package(item->d_name)) continue;
      state->entries.push_back({item->d_name, path, folder, static_cast<uint64_t>(info.st_size)});
    }
    closedir(directory);
  }
  std::sort(state->entries.begin(), state->entries.end(), [](const Entry &a, const Entry &b) {
    return a.directory != b.directory ? a.directory : strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  struct statvfs storage{};
  std::string capacity;
  if (statvfs(gDirectory.c_str(), &storage) == 0)
    capacity = Size(static_cast<uint64_t>(storage.f_bavail) * storage.f_frsize) + " available";
  if (RecoveryDataLocked()) capacity = "Internal storage is locked";
  lv_label_set_text(state->storage_label, capacity.c_str());
  std::string summary = directory ? std::to_string(state->entries.size()) + " items" :
      std::string("Cannot open folder: ") + strerror(error);
  if (directory && state->entries.empty()) summary = "No files in this view";
  lv_label_set_text(state->summary, summary.c_str());
  RenderEntries(state);
  lv_obj_scroll_to_y(state->list, 0, LV_ANIM_OFF);
}
}  // namespace

bool NavigateFileBack() {
  if (!gFiles || gDirectory == "/") return false;
  gDirectory = Parent(gDirectory);
  gFiles->visible = 100;
  Populate(gFiles);
  return true;
}

void BuildFilesScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  if (gDirectory.empty()) gDirectory = RecoveryStorage();
  if (gDirectory.empty()) gDirectory = "/sdcard";
  auto *state = new Files{screen, nullptr, nullptr, nullptr, nullptr, callback, context, {}, 100};
  gFiles = state;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    auto *s = static_cast<Files *>(lv_event_get_user_data(event));
    if (gFiles == s) gFiles = nullptr;
    delete s;
  }, LV_EVENT_DELETE, state);
  Header(screen, "Files", "Browse storage, preview pictures or install packages.", callback, context);
  const bool landscape = Landscape(screen);
  state->storage_label = Label(screen, "", &lv_font_montserrat_24, kAccent);
  lv_obj_set_pos(state->storage_label, 80, landscape ? 306 : 420);

  auto *storage = Button(screen, "Storage  " LV_SYMBOL_DOWN, [state] {
    auto volumes = RecoveryVolumes("storage");
    if (volumes.empty()) {
      Sheet(state->screen, "Storage", "No storage volumes are available.");
      return;
    }
    size_t next = 0;
    for (size_t i = 0; i < volumes.size(); ++i)
      if (volumes[i].path == gDirectory) next = (i + 1) % volumes.size();
    gDirectory = volumes[next].path;
    Populate(state);
  });
  lv_obj_set_pos(storage, 64, landscape ? 350 : 496);
  lv_obj_set_size(storage, 410, 120);
  auto *root = Button(screen, "Root", [state] { gDirectory = "/"; Populate(state); });
  lv_obj_set_pos(root, 492, landscape ? 350 : 496);
  lv_obj_set_size(root, 220, 120);
  auto *filter = Button(screen, gPackagesOnly ? "Installable only" : "All files", [state] {
    gPackagesOnly = !gPackagesOnly;
    state->callback(Action::kInstall, state->context);
  });
  lv_obj_set_pos(filter, 730, landscape ? 350 : 496);
  lv_obj_set_size(filter, 420, 120);
  auto *refresh = Button(screen, LV_SYMBOL_REFRESH, [state] { Populate(state); });
  lv_obj_set_pos(refresh, 1170, landscape ? 350 : 496);
  lv_obj_set_size(refresh, 206, 120);

  state->path_label = Label(screen, "", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->path_label, landscape ? 1460 : 80,
                 landscape ? 358 : 684);
  SingleLineLabel(state->path_label, landscape ? 1620 : 1270,
                  &lv_font_montserrat_32);
  state->summary = Label(screen, "", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->summary, landscape ? 1460 : 80,
                 landscape ? 424 : 750);
  // The file surface reaches the physical bottom. Navigation is a foreground
  // overlay, while bottom padding keeps the final entry scrollable above it.
  state->list = Scroll(screen, landscape ? 500 : 826,
                       landscape ? 940 : 2342);
  lv_obj_set_style_pad_bottom(state->list, landscape ? 190 : 242, 0);
  Navigation(screen, Action::kNone, callback, context);
  Populate(state);
}
}  // namespace recovery_ui2
