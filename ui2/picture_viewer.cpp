/* SPDX-License-Identifier: Apache-2.0 */
#include "picture_viewer.hpp"
#include "picture_decode.hpp"
#include "ui_components.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <memory>
#include <thread>

namespace recovery_ui2 {
namespace {
using namespace widgets;
struct Viewer {
  std::shared_ptr<PictureData> data = std::make_shared<PictureData>();
  lv_obj_t *overlay = nullptr, *viewport = nullptr, *content = nullptr;
  lv_obj_t *image = nullptr, *message = nullptr, *detail = nullptr;
  lv_timer_t *timer = nullptr;
  lv_image_dsc_t descriptor{};
  uint32_t scale = 256, fit = 256;
};

void Zoom(Viewer *state, uint32_t scale) {
  if (!state->image) return;
  state->scale = std::clamp(scale, state->fit, 1024u);
  const int width = state->data->width * state->scale / 256;
  const int height = state->data->height * state->scale / 256;
  lv_obj_set_size(state->content, std::max(1312, width), std::max(2180, height));
  lv_image_set_scale(state->image, state->scale);
  lv_obj_center(state->image);
  lv_obj_update_layout(state->viewport);
  lv_obj_scroll_to(state->viewport, std::max(0, (width - 1312) / 2),
                    std::max(0, (height - 2180) / 2), LV_ANIM_OFF);
  const auto text = std::to_string(state->data->width) + " x " +
      std::to_string(state->data->height) + "  /  " +
      std::to_string(state->scale * 100 / 256) + "%  /  Drag to pan";
  lv_label_set_text(state->detail, text.c_str());
}

void Loaded(lv_timer_t *timer) {
  auto *state = static_cast<Viewer *>(lv_timer_get_user_data(timer));
  if (!state->data->ready.load(std::memory_order_acquire)) return;
  lv_timer_pause(timer);
  if (!state->data->pixels) {
    lv_label_set_text(state->message, state->data->error.c_str());
    lv_label_set_text(state->detail, "PNG and baseline JPEG supported");
    return;
  }
  lv_obj_add_flag(state->message, LV_OBJ_FLAG_HIDDEN);
  auto &desc = state->descriptor;
  desc.header.magic = LV_IMAGE_HEADER_MAGIC;
  desc.header.cf = LV_COLOR_FORMAT_ARGB8888;
  desc.header.w = state->data->width;
  desc.header.h = state->data->height;
  desc.header.stride = state->data->width * 4;
  desc.data_size = state->data->width * state->data->height * 4;
  desc.data = state->data->pixels;
  state->image = lv_image_create(state->content);
  lv_image_set_src(state->image, &desc);
  lv_image_set_antialias(state->image, true);
  lv_obj_remove_flag(state->image, LV_OBJ_FLAG_CLICKABLE);
  state->fit = std::max(1u, std::min({1312u * 256 / state->data->width,
                                    2180u * 256 / state->data->height, 256u}));
  Zoom(state, state->fit);
}
} // namespace

void OpenPicture(lv_obj_t *screen, const std::string &path) {
  auto *state = new Viewer;
  auto *overlay = lv_obj_create(screen);
  state->overlay = overlay;
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_user_data(overlay, &kModalMarker);
  MainBackground(overlay);
  AttachStatusBar(overlay, nullptr, nullptr, StatusBarAction::kNone, true);
  auto *name = Label(overlay, path.substr(path.find_last_of('/') + 1).c_str(),
                      &lv_font_montserrat_48, kText);
  lv_obj_set_pos(name, 80, 252);
  lv_obj_set_width(name, 1280);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  state->detail = Label(overlay, "Loading image...", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->detail, 80, 354);
  state->viewport = Scroll(overlay, 470, 2180);
  lv_obj_set_scroll_dir(state->viewport, LV_DIR_ALL);
  state->content = lv_obj_create(state->viewport);
  Clear(state->content);
  lv_obj_set_size(state->content, 1312, 2180);
  lv_obj_remove_flag(state->content, LV_OBJ_FLAG_CLICKABLE);
  state->message = Label(state->content, "Opening preview...", &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(state->message, 1100);
  lv_obj_set_style_text_align(state->message, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(state->message);
  auto *less = Button(overlay, LV_SYMBOL_MINUS, [state] { Zoom(state, state->scale / 2); });
  lv_obj_set_pos(less, 80, 2720); lv_obj_set_size(less, 270, 132);
  auto *fit = Button(overlay, "Fit", [state] { Zoom(state, state->fit); });
  lv_obj_set_pos(fit, 375, 2720); lv_obj_set_size(fit, 690, 132);
  auto *more = Button(overlay, LV_SYMBOL_PLUS, [state] { Zoom(state, state->scale * 2); });
  lv_obj_set_pos(more, 1090, 2720); lv_obj_set_size(more, 270, 132);
  auto *close = Button(overlay, "Back to files", [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_set_pos(close, 80, 2920); lv_obj_set_size(close, 1280, 140);
  // LVGL sends parent DELETE before deleting its children. Explicitly remove
  // the image subtree while its descriptor and pixels are still valid.
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    auto *s = static_cast<Viewer *>(lv_event_get_user_data(event));
    s->data->cancelled = true;
    if (s->timer) lv_timer_delete(s->timer);
    lv_obj_delete(s->content);
    lv_image_cache_drop(&s->descriptor);
    delete s;
  }, LV_EVENT_DELETE, state);
  state->timer = lv_timer_create(Loaded, 40, state);
  // The worker owns only decoded data, never LVGL objects. Closing the viewer
  // cancels JPEG I/O and safely releases PNG data when its bounded decode ends.
  std::thread([data = state->data, path] { DecodePicture(path, *data); }).detach();
}
} // namespace recovery_ui2
