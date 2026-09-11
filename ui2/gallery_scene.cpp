/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "picture_decode.hpp"
#include "picture_viewer.hpp"
#include "ui_components.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace recovery_ui2 {
namespace {
using namespace widgets;
struct GalleryEntry { std::string path; std::string name; };

void ScanPictures(const std::string &path, int depth,
                  std::vector<GalleryEntry> &out) {
  if (depth > 5 || out.size() >= 300) return;
  DIR *directory = opendir(path.c_str());
  if (!directory) return;
  while (auto *entry = readdir(directory)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        entry->d_name[0] == '.') continue;
    const std::string child = path + "/" + entry->d_name;
    struct stat info{};
    if (lstat(child.c_str(), &info) || S_ISLNK(info.st_mode)) continue;
    if (S_ISDIR(info.st_mode)) ScanPictures(child, depth + 1, out);
    else if (S_ISREG(info.st_mode) && IsPicture(child))
      out.push_back({child, entry->d_name});
    if (out.size() >= 300) break;
  }
  closedir(directory);
}
}  // namespace

void BuildGalleryScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  Header(screen, "Gallery", "Pictures from internal storage.", callback, context);
  const bool landscape = Landscape(screen);
  std::vector<GalleryEntry> pictures;
  for (const char *root : {"/sdcard/DCIM", "/sdcard/Pictures", "/sdcard/Download",
                           "/sdcard/AERA/screenshots"})
    ScanPictures(root, 0, pictures);
  std::sort(pictures.begin(), pictures.end(), [](const auto &a, const auto &b) {
    return a.path > b.path;
  });
  const std::string count = std::to_string(pictures.size()) +
      (pictures.size() == 1 ? " IMAGE" : " IMAGES");
  auto *badge = Kicker(screen, count.c_str(), pictures.empty() ? kMuted : kAccent);
  lv_obj_set_pos(badge, 80, landscape ? 306 : 426);
  auto *grid = Scroll(screen, landscape ? 350 : 500,
                      landscape ? 900 : 2370);
  int index = 0;
  for (const auto &picture : pictures) {
    const int columns = landscape ? 4 : 2;
    const int column = index % columns;
    const int row = index / columns;
    const int tile_width = landscape ? 748 : 640;
    auto *tile = lv_button_create(grid);
    Panel(tile, 38, kMainPanel); Interactive(tile, kMainSelected);
    lv_obj_set_pos(tile, column * (landscape ? 764 : 664), row * 330);
    lv_obj_set_size(tile, tile_width, 306);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, kMainLine, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_30, 0);
    auto *preview = lv_obj_create(tile);
    Panel(preview, 28, Color(0x242a30));
    lv_obj_set_pos(preview, 18, 18);
    lv_obj_set_size(preview, tile_width - 36, 190);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);
    auto *icon = Label(preview, LV_SYMBOL_IMAGE, &lv_font_montserrat_48, kAccent);
    lv_obj_set_style_transform_scale(icon, 420, 0); lv_obj_center(icon);
    auto *name = Label(tile, picture.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 26, 224);
    lv_obj_set_width(name, tile_width - 70);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    OnClick(tile, [screen, path = picture.path] { OpenPicture(screen, path); });
    AnimateEnter(tile, 10 + index * 4, 8);
    ++index;
  }
  if (pictures.empty()) {
    auto *empty = Label(grid,
        "No PNG or JPEG images were found in DCIM, Pictures, Download, or AERA screenshots.",
        &lv_font_montserrat_32, kMutedStrong);
    lv_obj_set_width(empty, 1050); lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 280);
  }
  Navigation(screen, Action::kBackHome, callback, context, true);
}
}  // namespace recovery_ui2
