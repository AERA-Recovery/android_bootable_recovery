/* SPDX-License-Identifier: Apache-2.0 */
#include "wallpaper.hpp"

#include <algorithm>
#include <memory>

#include "picture_decode.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace aeraui::wallpaper {
namespace {

std::shared_ptr<PictureData> gPicture;
std::string gPath;

struct Instance {
  std::shared_ptr<PictureData> picture;
  lv_image_dsc_t descriptor{};
};

void DeleteInstance(lv_event_t *event) {
  auto *instance = static_cast<Instance *>(lv_event_get_user_data(event));
  if (!instance) return;
  lv_image_cache_drop(&instance->descriptor);
  delete instance;
}

}  // namespace

bool Load(const std::string &path, std::string *error) {
  if (path.empty()) {
    Clear();
    return true;
  }
  if (gPicture && gPicture->pixels && gPath == path) return true;

  const uint32_t horizontal = std::max(
      1, static_cast<int>(lv_display_get_horizontal_resolution(nullptr)));
  const uint32_t vertical = std::max(
      1, static_cast<int>(lv_display_get_vertical_resolution(nullptr)));
  const uint32_t limit = std::max(horizontal, vertical);
  auto picture = std::make_shared<PictureData>();
  DecodePictureThumbnail(path, limit, limit, *picture);
  if (!picture->pixels) {
    if (error) *error = picture->error.empty()
        ? "AERA could not decode this image." : picture->error;
    return false;
  }

  gPicture = std::move(picture);
  gPath = path;
  return true;
}

void Clear() {
  gPicture.reset();
  gPath.clear();
}

bool Active() { return gPicture && gPicture->pixels; }

const std::string &Path() { return gPath; }

bool Attach(lv_obj_t *target) {
  if (!target || !Active()) return false;
  auto *instance = new Instance;
  instance->picture = gPicture;
  auto &descriptor = instance->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = instance->picture->width;
  descriptor.header.h = instance->picture->height;
  descriptor.header.stride = instance->picture->width * 4;
  descriptor.data_size = instance->picture->width *
      instance->picture->height * 4;
  descriptor.data = instance->picture->pixels;

  auto *image = lv_image_create(target);
  lv_obj_set_pos(image, 0, 0);
  lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
  lv_image_set_src(image, &descriptor);
  lv_image_set_inner_align(image, LV_IMAGE_ALIGN_COVER);
  lv_image_set_antialias(image, true);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(image, DeleteInstance, LV_EVENT_DELETE, instance);
  lv_obj_move_background(image);
  return true;
}

}  // namespace aeraui::wallpaper
