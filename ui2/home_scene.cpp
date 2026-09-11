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
bool Package(const std::string &name) {
  return Zip(name) || plugins::IsPackageFile(name);
}
std::string Parent(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  const auto slash = path.find_last_of('/');
  return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}
void Populate(Files *state);

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
  } else {
    Sheet(state->screen, entry.name,
          entry.path + "\n\nSize: " + Size(entry.bytes) +
          "\n\nOpen images, install ZIP packages, or install .aerap plugins. "
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
        IsPicture(entry.name) ? LV_SYMBOL_IMAGE : LV_SYMBOL_FILE,
        entry.name, entry.directory ? "Folder" :
          Size(entry.bytes) + (plugins::IsPackageFile(entry.name) ?
                              "  /  AERA plugin package" :
                              Zip(entry.name) ? "  /  ZIP package" :
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
  auto *filter = Button(screen, gPackagesOnly ? "Packages only" : "All files", [state] {
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
  lv_obj_set_width(state->path_label, landscape ? 1620 : 1270);
  lv_label_set_long_mode(state->path_label, LV_LABEL_LONG_DOT);
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
