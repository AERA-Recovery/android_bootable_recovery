// SPDX-License-Identifier: Apache-2.0
#include "../scene.hpp"
#include "../ui_components.hpp"
#include "protocol.hpp"
#include "launcher.hpp"
#include "../phone_keyboard.hpp"
#include "runtime.hpp"
#include "session.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <thread>
#include <unistd.h>

namespace recovery_ui2 {
namespace {
using namespace widgets;
constexpr int kBrowserViewportWidth = 1392;
constexpr int kBrowserViewportHeight = 2708;
constexpr int kBrowserImageScale =
    (kBrowserViewportWidth * 256 + web::kWidth / 2) / web::kWidth;
struct WebScene;
void BrowserRefreshReady(lv_event_t *event);

struct WebScene {
  lv_obj_t *screen = nullptr, *address = nullptr, *keyboard = nullptr;
  lv_display_t *display = nullptr;
  lv_obj_t *navigation = nullptr;
  lv_obj_t *title = nullptr, *detail = nullptr, *progress = nullptr, *prepare = nullptr;
  lv_obj_t *area = nullptr, *viewport = nullptr, *image = nullptr;
  lv_obj_t *web_progress = nullptr;
  lv_obj_t *controls[5]{};
  lv_image_dsc_t descriptor{};
  web::Session session;
  web::BrowserProcess process;
  bool showing_web = false, web_keyboard = false;
  std::string last_status;
  unsigned last_progress = 101;
  lv_timer_t *timer = nullptr;
  web::Preparation state;
  std::thread worker;
  bool started = false, finished = false, auto_launch = true;
  bool frame_waiting_for_refresh = false;
  int view_left = 24, view_top = 460;
  int view_width = kBrowserViewportWidth;
  int view_height = kBrowserViewportHeight;
  ~WebScene() {
    if (timer) lv_timer_delete(timer);
    if (display)
      lv_display_remove_event_cb_with_user_data(
          display, BrowserRefreshReady, this);
    state.cancel.store(true);
    if (worker.joinable()) worker.join();
    if (image) {
      lv_image_set_src(image, nullptr);
      lv_image_cache_drop(&descriptor);
    }
    if (session.Connected()) session.Send(web::Kind::kClose);
    session.Close();
    process.Stop();
    web::RemoveRuntime(state.directory);
  }
};

void BrowserRefreshReady(lv_event_t *event) {
  auto *s = static_cast<WebScene *>(lv_event_get_user_data(event));
  if (!s->frame_waiting_for_refresh) return;
  s->frame_waiting_for_refresh = false;
  if (s->session.Connected()) s->session.AcknowledgeFrame();
}

void HideKeyboard(WebScene *s) {
  lv_obj_add_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(s->keyboard, nullptr);
  lv_obj_remove_state(s->address, LV_STATE_FOCUSED);
  if (s->navigation && !RecoveryDockHideInApps())
    lv_obj_remove_flag(s->navigation, LV_OBJ_FLAG_HIDDEN);
  s->web_keyboard = false;
}
void ShowKeyboard(WebScene *s, bool web_keyboard, uint32_t purpose = 0) {
  s->web_keyboard = web_keyboard;
  lv_keyboard_set_mode(s->keyboard,
      web_keyboard && (purpose == 2 || purpose == 9)
          ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(s->keyboard, web_keyboard ? nullptr : s->address);
  if (s->navigation) lv_obj_add_flag(s->navigation, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s->keyboard);
}
void Prepare(WebScene *s) {
  HideKeyboard(s);
  if (s->started) return;
  if (!web::RuntimeInstalled()) {
    lv_label_set_text(s->title, "Browser package unavailable");
    lv_label_set_text(s->detail, "This image does not contain the expected browser payload.");
    return;
  }
  s->started = true;
  lv_obj_add_state(s->prepare, LV_STATE_DISABLED);
  lv_obj_remove_flag(s->progress, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s->title, "Preparing WebKit");
  lv_label_set_text(s->detail, "Verifying and expanding the engine in RAM. You can leave this page to cancel.");
  s->worker = std::thread([s] { web::PrepareRuntime(s->state); });
}
}  // namespace

void BuildWebScene(lv_obj_t *screen, ActionCallback callback, void *context,
                   int frame_fd, int control_fd, bool auto_launch) {
  auto *s = new WebScene;
  s->auto_launch = auto_launch;
  s->screen = screen;
  s->display = lv_obj_get_display(screen);
  lv_display_add_event_cb(
      s->display, BrowserRefreshReady, LV_EVENT_REFR_READY, s);
  if (frame_fd >= 0 || control_fd >= 0) s->session.Adopt(frame_fd, control_fd);
  lv_obj_add_event_cb(screen, [](lv_event_t *e) {
    delete static_cast<WebScene *>(lv_event_get_user_data(e));
  }, LV_EVENT_DELETE, s);
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  const bool landscape = lv_obj_get_width(screen) > lv_obj_get_height(screen);
  if (landscape) {
    s->view_top = StatusBarHeight();
    s->view_height = lv_obj_get_height(screen) - s->view_top;
    s->view_width = s->view_height * kBrowserViewportWidth /
                    kBrowserViewportHeight;
    s->view_left = 48;
  } else {
    s->view_height = lv_obj_get_height(screen) - s->view_top;
  }
  s->web_progress = lv_bar_create(screen);
  lv_obj_set_pos(s->web_progress, landscape ? 760 : 24, 316);
  lv_obj_set_size(s->web_progress,
                  landscape ? lv_obj_get_width(screen) - 824 : 1392, 6);
  lv_obj_set_style_bg_color(s->web_progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(s->web_progress, LV_OBJ_FLAG_HIDDEN);
  s->address = lv_textarea_create(screen);
  lv_obj_set_pos(s->address, landscape ? 760 : 24, 188);
  lv_obj_set_size(s->address, landscape ? 2020 : 1168, 112);
  lv_textarea_set_one_line(s->address, true);
  lv_textarea_set_max_length(s->address, 2040);
  lv_textarea_set_placeholder_text(s->address, "Enter website address");
  lv_obj_set_style_text_font(s->address, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(s->address, kText, 0);
  lv_obj_set_style_bg_color(s->address, kMainPanel, 0);
  lv_obj_set_style_bg_opa(s->address, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s->address, kMainLine, 0);
  lv_obj_set_style_border_width(s->address, 1, 0);
  lv_obj_set_style_radius(s->address, 56, 0);
  lv_obj_set_style_pad_all(s->address, 26, 0);
  auto *go = Button(screen, "Go", [s] {
    auto address = web::Address(lv_textarea_get_text(s->address));
    if (address.empty()) {
      HideKeyboard(s);
      Sheet(s->screen, "Website address", "Enter an HTTP or HTTPS address, such as example.org. Local files and executable URLs are not supported.");
      return;
    }
    lv_textarea_set_text(s->address, address.c_str());
    HideKeyboard(s);
    if (s->session.Connected()) {
      s->session.Send(web::Kind::kOpen, 0, 0, 0, address.c_str());
      return;
    }
    Sheet(s->screen, "Browsing unavailable", web::LaunchBlockReason());
  }, true);
  lv_obj_set_pos(go, landscape ? 2800 : 1210, 188);
  lv_obj_set_size(go, landscape ? 304 : 206, 112);
  lv_obj_set_style_radius(go, 56, 0);
  const char *controls[] = {LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT,
                            LV_SYMBOL_REFRESH, LV_SYMBOL_CLOSE, LV_SYMBOL_KEYBOARD};
  for (int i = 0; i < 5; ++i) {
    auto *button = Button(screen, controls[i], [s, i] {
      if (!s->session.Connected()) return;
      if (i == 4) {
        ShowKeyboard(s, true);
      } else {
        const web::Kind kinds[] = {web::Kind::kBack, web::Kind::kForward,
                                   web::Kind::kReload, web::Kind::kStop};
        s->session.Send(kinds[i]);
      }
    });
    s->controls[i] = button;
    lv_obj_set_pos(button, (landscape ? 760 : 24) +
                     i * (landscape ? 469 : 278), 334);
    lv_obj_set_size(button, landscape ? 445 : 254, 106);
    lv_obj_set_style_radius(button, 53, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(button, 0),
                               UiFont(&lv_font_montserrat_48), 0);
    // Do not present navigation as functional before the isolated session exists.
    lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
  }
  auto *area = lv_obj_create(screen);
  s->area = area;
  Panel(area, 36, kMainPanel);
  lv_obj_set_pos(area, s->view_left, s->view_top);
  lv_obj_set_size(area, s->view_width, s->view_height);
  lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);
  auto *icon = Label(area, LV_SYMBOL_GPS, &lv_font_montserrat_48, kAccent);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 160);
  s->title = Label(area, "Web engine installed", &lv_font_montserrat_48, kText);
  lv_obj_set_size(s->title, landscape ? s->view_width - 70 : 1140,
                  LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(s->title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s->title, LV_ALIGN_TOP_MID, 0, landscape ? 230 : 288);
  s->detail = Label(area, web::LaunchBlockReason().c_str(), &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(s->detail, landscape ? s->view_width - 70 : 1090);
  lv_obj_set_style_text_line_space(s->detail, 16, 0);
  lv_obj_set_style_text_align(s->detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s->detail, LV_ALIGN_TOP_MID, 0, landscape ? 360 : 418);
  s->progress = lv_bar_create(area);
  lv_obj_set_size(s->progress, landscape ? s->view_width - 90 : 1030, 12);
  lv_obj_align(s->progress, LV_ALIGN_TOP_MID, 0, landscape ? 620 : 870);
  lv_obj_set_style_bg_color(s->progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(s->progress, LV_OBJ_FLAG_HIDDEN);
  s->prepare = Button(area, "Verify and prepare engine", [s] { Prepare(s); }, true);
  lv_obj_set_size(s->prepare, landscape ? s->view_width - 70 : 1050,
                  landscape ? 118 : 144);
  lv_obj_set_style_opa(s->prepare, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_align(s->prepare, LV_ALIGN_TOP_MID, 0, landscape ? 680 : 960);
  auto *note = Label(area, "Wi-Fi setup is separate.\n\nNo browser runs during boot.\nLeaving this page releases the prepared runtime.\n\nDownloads and file access are not enabled.",
                     &lv_font_montserrat_32, kMuted);
  lv_obj_set_width(note, landscape ? s->view_width - 70 : 1090);
  lv_obj_set_style_text_line_space(note, 14, 0);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(note, LV_ALIGN_TOP_MID, 0, landscape ? 850 : 1240);
  if (!web::RuntimeInstalled()) {
    lv_label_set_text(s->title, "Browser package unavailable");
    lv_obj_add_state(s->prepare, LV_STATE_DISABLED);
  }
  s->viewport = lv_obj_create(screen);
  Clear(s->viewport);
  lv_obj_set_pos(s->viewport, s->view_left, s->view_top);
  lv_obj_set_size(s->viewport, s->view_width, s->view_height);
  lv_obj_add_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
  s->image = lv_image_create(s->viewport);
  lv_image_set_pivot(s->image, 0, 0);
  lv_image_set_scale(s->image, landscape
      ? (s->view_width * 256 + web::kWidth / 2) / web::kWidth
      : kBrowserImageScale);
  lv_obj_remove_flag(s->image, LV_OBJ_FLAG_CLICKABLE);
  s->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  s->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  // WebKit reuses this descriptor while replacing its pixel contents. Keep
  // every frame on the OpenGL renderer, but never reuse a stale GPU texture.
  // USER1 selects the OpenGL renderer's persistent streaming-texture path.
  s->descriptor.header.flags = LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;
  s->descriptor.header.w = web::kWidth; s->descriptor.header.h = web::kHeight;
  s->descriptor.header.stride = web::kWidth * 4;
  s->descriptor.data_size = web::kFrameBytes;
  lv_obj_add_event_cb(s->viewport, [](lv_event_t *e) {
    const auto code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    // Screen DELETE frees the scene before its children are deleted. Ignore
    // child lifecycle events before dereferencing their scene user-data.
    auto *s = static_cast<WebScene *>(lv_event_get_user_data(e));
    if (!s->session.Connected()) return;
    auto *input = lv_indev_active();
    if (!input) return;
    lv_point_t point{}; lv_indev_get_point(input, &point);
    lv_area_t bounds{}; lv_obj_get_coords(s->viewport, &bounds);
    const int x = std::clamp<int>((point.x - bounds.x1) * web::kViewWidth /
        s->view_width, 0, web::kViewWidth - 1);
    const int y = std::clamp<int>((point.y - bounds.y1) * web::kViewHeight /
        s->view_height, 0, web::kViewHeight - 1);
    const auto kind = code == LV_EVENT_PRESSED ? web::Kind::kTouchDown :
        code == LV_EVENT_PRESSING ? web::Kind::kTouchMove : web::Kind::kTouchUp;
    s->session.Send(kind, x, y);
  }, LV_EVENT_ALL, s);
  s->navigation = Navigation(screen, Action::kSettings, callback, context, true);
  s->keyboard = lv_keyboard_create(screen);
  phone_keyboard::Apply(s->keyboard);
  lv_obj_set_size(s->keyboard, landscape ? 2408 : 1440,
                  landscape ? 720 : 760);
  if (landscape) lv_obj_align(s->keyboard, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  else lv_obj_align(s->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(s->keyboard, UiFont(&lv_font_montserrat_48),
                             LV_PART_ITEMS);
  lv_obj_set_style_bg_color(s->keyboard, kMainBottom, 0);
  lv_obj_set_style_bg_opa(s->keyboard, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(s->keyboard, 18, 0);
  lv_obj_set_style_pad_row(s->keyboard, 12, 0);
  lv_obj_set_style_pad_column(s->keyboard, 8, 0);
  lv_obj_set_style_bg_color(s->keyboard, kMainPanel, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(s->keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(s->keyboard, kMainSelected,
                            LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(s->keyboard, kText, LV_PART_ITEMS);
  lv_obj_set_style_radius(s->keyboard, 22, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s->keyboard, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(s->keyboard, kMainLine, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s->keyboard, 1, 0);
  lv_obj_set_style_border_side(s->keyboard, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(s->keyboard, kMainLine, 0);
  lv_obj_add_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s->address, [](lv_event_t *e) {
    auto *s = static_cast<WebScene *>(lv_event_get_user_data(e));
    ShowKeyboard(s, false);
  }, LV_EVENT_CLICKED, s);
  lv_obj_add_event_cb(s->keyboard, [](lv_event_t *e) {
    auto *s = static_cast<WebScene *>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL)
      HideKeyboard(s);
    else if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED && s->web_keyboard && s->session.Connected()) {
      const auto selected = lv_buttonmatrix_get_selected_button(s->keyboard);
      const char *key = lv_buttonmatrix_get_button_text(s->keyboard, selected);
      if (!key) return;
      if (!strcmp(key, LV_SYMBOL_BACKSPACE)) s->session.Send(web::Kind::kKey, 0, 0, 8);
      else if (!strcmp(key, LV_SYMBOL_NEW_LINE)) s->session.Send(web::Kind::kKey, 0, 0, 13);
      else if (strlen(key) == 1 && static_cast<unsigned char>(key[0]) >= 32)
        s->session.Send(web::Kind::kKey, 0, 0, static_cast<unsigned char>(key[0]));
      // Mode selectors are keyboard UI, never injected as strings into a page.
    }
  }, LV_EVENT_ALL, s);
  s->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *s = static_cast<WebScene *>(lv_timer_get_user_data(timer));
    if (s->session.Connected() || s->showing_web) {
      const bool new_frame = s->session.Poll();
      uint32_t keyboard_purpose = 0;
      const auto keyboard_request = s->session.TakeKeyboardRequest(&keyboard_purpose);
      if (keyboard_request == web::KeyboardRequest::kShow)
        ShowKeyboard(s, true, keyboard_purpose);
      else if (keyboard_request == web::KeyboardRequest::kHide && s->web_keyboard)
        HideKeyboard(s);
      if (new_frame) {
        s->descriptor.data = s->session.Pixels();
        lv_image_cache_drop(&s->descriptor);
        lv_image_set_src(s->image, &s->descriptor);
        s->frame_waiting_for_refresh = true;
        lv_obj_invalidate(s->image);
        lv_obj_add_flag(s->area, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
        s->showing_web = true;
      }
      for (int i = 0; i < 5; ++i) {
        const bool enabled = s->session.Connected() &&
            (i == 0 ? s->session.CanBack() : i == 1 ? s->session.CanForward() : true);
        if (enabled) lv_obj_remove_state(s->controls[i], LV_STATE_DISABLED);
        else lv_obj_add_state(s->controls[i], LV_STATE_DISABLED);
      }
      if (s->last_status != s->session.Status()) {
        s->last_status = s->session.Status();
        lv_label_set_text(s->detail, s->last_status.c_str());
        // The editable field is the only address display. Track redirects,
        // history navigation and same-document URL changes unless the user is
        // actively replacing its contents.
        const auto current_address = web::Address(s->last_status);
        if (!current_address.empty() &&
            !lv_obj_has_state(s->address, LV_STATE_FOCUSED))
          lv_textarea_set_text(s->address, current_address.c_str());
      }
      if (s->session.Progress() != s->last_progress) {
        s->last_progress = s->session.Progress();
        lv_bar_set_value(s->web_progress, s->last_progress, LV_ANIM_ON);
        if (s->last_progress < 100 && s->session.Connected())
          lv_obj_remove_flag(s->web_progress, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s->web_progress, LV_OBJ_FLAG_HIDDEN);
      }
      if (!s->session.Connected()) {
        HideKeyboard(s);
        lv_obj_add_flag(s->web_progress, LV_OBJ_FLAG_HIDDEN);
        s->showing_web = false;
        lv_obj_add_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s->area, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s->title, "Browser stopped");
      }
    }
    if (!s->started || s->finished) return;
    lv_bar_set_value(s->progress, s->state.progress.load(), LV_ANIM_ON);
    if (!s->state.done.load(std::memory_order_acquire)) return;
    s->finished = true;
    if (s->worker.joinable()) s->worker.join();
    if (!s->state.verified) {
      lv_label_set_text(s->title, "Preparation failed");
      lv_label_set_text(s->detail, s->state.error.c_str());
      return;
    }
    if (!s->auto_launch) {
      lv_label_set_text(s->title, "Runtime verified");
      lv_label_set_text(s->detail, web::LaunchBlockReason().c_str());
      return;
    }
    int frame = -1, control = -1;
    std::string error;
    if (!s->process.Start(s->state.directory, frame, control, error) ||
        !s->session.Adopt(frame, control)) {
      s->process.Stop();
      lv_label_set_text(s->title, "Browser start failed");
      lv_label_set_text(s->detail, error.empty() ? s->session.Status().c_str() : error.c_str());
      return;
    }
    lv_label_set_text(s->title, "Starting private browser");
    lv_label_set_text(s->detail, "Loading the built-in start page...");
  }, 8, s);

  // Opening Browser is the user's explicit request to use the engine. Begin
  // verification immediately instead of depending on a second tap in the
  // large content panel; manual mode remains available to the host UI tests.
  if (s->auto_launch) Prepare(s);
}
}  // namespace recovery_ui2
