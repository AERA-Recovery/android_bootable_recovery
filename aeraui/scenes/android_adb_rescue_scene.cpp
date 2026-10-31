/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "ui_components.hpp"
#include <algorithm>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
namespace aeraui {
namespace {
using namespace design;
using namespace widgets;
lv_obj_t *Card(lv_obj_t *parent, const char *title) {
  auto *card = lv_obj_create(parent); Panel(card, 28, kMainSheet);
  lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(card, 28, 0); lv_obj_set_style_pad_row(card, 20, 0);
  auto *label = Label(card, title, &lv_font_montserrat_32, kText);
  lv_obj_set_width(label, LV_PCT(100)); lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  return card;
}
void Text(lv_obj_t *parent, const char *text, lv_color_t color = kMutedStrong) {
  auto *label = Label(parent, text, &lv_font_montserrat_28, color);
  lv_obj_set_width(label, LV_PCT(100)); lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}
struct Selection { adb_rescue::PublicKey key; lv_obj_t *label = nullptr; };
void ChooseKey(lv_obj_t *screen, const std::shared_ptr<Selection> &selection, std::string path = "") {
  auto *overlay = lv_obj_create(screen); Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_size(overlay, lv_obj_get_width(screen), lv_obj_get_height(screen));
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0); lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  auto *close = Button(overlay, LV_SYMBOL_LEFT, [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_set_pos(close, 40, 40); lv_obj_set_size(close, 112, 112);
  auto *title = Label(overlay, "Choose adbkey.pub", &lv_font_montserrat_40, kText);
  lv_obj_set_pos(title, 180, 52); lv_obj_set_width(title, lv_obj_get_width(screen) - 220);
  auto *list = Scroll(overlay, 184, lv_obj_get_height(screen) - 220);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(list, 16, 0);
  auto row = [&](const std::string &name, const std::string &target, bool directory) {
    auto *button = Button(list, name.c_str(), [=] {
      if (directory) { ChooseKey(screen, selection, target); lv_obj_delete_async(overlay); return; }
      adb_rescue::PublicKey key; std::string error;
      if (!RecoveryReadAdbPublicKey(target, &key, &error)) { Sheet(overlay, "Android ADB Rescue", error); return; }
      selection->key = std::move(key);
      lv_label_set_text(selection->label, (selection->key.label + "\n" + selection->key.fingerprint).c_str());
      lv_obj_delete_async(overlay);
    });
    lv_obj_set_size(button, LV_PCT(100), 112); FitButtonLabel(button);
  };
  if (path.empty()) {
    const auto internal = RecoveryInternalStorage();
    if (!internal.empty() && !RecoveryDataLocked()) row(i18n::Translate("Internal Storage"), internal, true);
    for (const auto &volume : RecoveryVolumes("storage"))
      if (volume.path != internal && volume.path != "/data") row(volume.name, volume.path, true);
  } else {
    Text(list, path.c_str());
    const auto slash = path.find_last_of('/');
    row(i18n::Translate("Up one level"), slash == 0 || path == RecoveryInternalStorage() ? "" : path.substr(0, slash), true);
    struct Entry { std::string name, path; bool directory; }; std::vector<Entry> entries;
    if (DIR *dir = opendir(path.c_str())) {
      while (auto *item = readdir(dir)) {
        const std::string name = item->d_name; if (name == "." || name == "..") continue;
        const std::string child = path + "/" + name; struct stat info{};
        if (lstat(child.c_str(), &info) || S_ISLNK(info.st_mode)) continue;
        const bool directory = S_ISDIR(info.st_mode);
        if (directory || (S_ISREG(info.st_mode) && name.size() > 4 && name.substr(name.size() - 4) == ".pub"))
          entries.push_back({name, child, directory});
        if (entries.size() >= 400) break;
      }
      closedir(dir);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
      return a.directory == b.directory ? a.name < b.name : a.directory > b.directory;
    });
    for (const auto &entry : entries) row(entry.name, entry.path, entry.directory);
  }
}
}
void BuildAndroidAdbRescueScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  Header(screen, "Android ADB Rescue", "", callback, context);
  const int top = Landscape(screen) ? 330 : 410;
  auto *list = Scroll(screen, top, lv_obj_get_height(screen) - top - NavigationHeight(screen) - 28);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(list, 22, 0);
  auto *back = Button(list, (std::string(LV_SYMBOL_LEFT) + "  " + i18n::Translate("Developer settings")).c_str(), [=] { callback(Action::kDeveloperSettings, context); });
  lv_obj_set_size(back, LV_PCT(100), 104);
  const auto status = RecoveryAndroidAdbRescueStatus();
  auto *info = Card(list, "Installed Android");
  Text(info, "These changes affect Android, not recovery.");
  if (!status.message.empty()) Text(info, status.message.c_str(), kAmber);
  if (status.available) {
    Text(info, status.enabled ? "Enabled" : "Disabled", status.enabled ? kAccent : kAmber);
    Text(info, (i18n::Translate("Authorized computers") + std::string(": ") + std::to_string(status.authorized_keys)).c_str());
    Text(info, (status.settings_format + " / " + status.settings_path).c_str());
  }
  auto selection = std::make_shared<Selection>();
  auto *computer = Card(list, "Authorize a computer");
  Text(computer, "Import adbkey.pub, never the private adbkey.");
  Text(computer, "Windows: %USERPROFILE%\\.android\\adbkey.pub");
  Text(computer, "Linux / macOS: ~/.android/adbkey.pub");
  Text(computer, "File missing? Run adb start-server on the computer.");
  selection->label = Label(computer, "Existing computer authorizations are kept.", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_width(selection->label, LV_PCT(100)); lv_label_set_long_mode(selection->label, LV_LABEL_LONG_WRAP);
  auto *choose = Button(computer, "Choose adbkey.pub", [=] { ChooseKey(screen, selection); });
  lv_obj_set_size(choose, LV_PCT(100), 112);
  Text(computer, "Original files and permissions are backed up.");
  auto *apply = Button(list, "Enable Android ADB", [=] {
    Sheet(screen, "Enable Android ADB", "Enable Android ADB and authorize the selected key?", [=] {
      std::string error;
      if (!RecoveryAndroidAdbRescueApply(selection->key.line, false, &error)) { Sheet(screen, "Android ADB Rescue", error); return; }
      Sheet(screen, "Android ADB Rescue", "Ready. Reboot Android, then run adb devices.",
          [=] { callback(Action::kAndroidAdbRescue, context); });
    });
  }, true);
  lv_obj_set_size(apply, LV_PCT(100), 124);
  if (!status.available) lv_obj_add_state(apply, LV_STATE_DISABLED);
  auto *restore = Button(list, "Restore previous ADB settings", [=] {
    Sheet(screen, "Restore previous ADB settings", "Restore the saved Android ADB settings?", [=] {
      std::string error;
      if (!RecoveryAndroidAdbRescueApply("", true, &error)) Sheet(screen, "Android ADB Rescue", error);
      else callback(Action::kAndroidAdbRescue, context);
    });
  });
  lv_obj_set_size(restore, LV_PCT(100), 112);
  if (!status.can_restore) lv_obj_add_state(restore, LV_STATE_DISABLED);
  auto *refresh = Button(list, LV_SYMBOL_REFRESH, [=] { callback(Action::kAndroidAdbRescue, context); });
  lv_obj_set_size(refresh, LV_PCT(100), 104);
  Text(list, "Requires readable Android data and working adbd.");
  Navigation(screen, Action::kSettings, callback, context);
}
}
