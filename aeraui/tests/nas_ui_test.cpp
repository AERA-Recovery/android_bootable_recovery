// SPDX-License-Identifier: Apache-2.0
#include "scene.hpp"
#include "ui_components.hpp"
#include "nas/smb_browser.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <png.h>

using namespace aeraui;
static NasStatus g_status;
static std::atomic<bool> g_slow{false};
static std::atomic<int> g_active{0};
static std::atomic<int> g_cancelled{0};
static int g_actions = 0;

namespace aeraui::wallpaper {
bool Attach(lv_obj_t *) { return false; }
}

namespace aeraui {
NasStatus RecoveryNasStatus() { return g_status; }
bool RecoverySetNasConfig(const NasConfig &config, std::string *) {
  g_status.config = config;
  return true;
}
void AttachStatusBar(lv_obj_t *, void (*)(Action, void *), void *, StatusBarAction, bool) {}
int32_t StatusBarHeight() { return 165; }
DockLayout RecoveryDockLayout() { return DockLayout::kGlass; }
bool RecoveryTintedIconBackgrounds() { return false; }
int RecoveryDockTransparency() { return 60; }
int RecoveryDockBlur() { return 24; }
bool RecoveryDockHideInApps() { return false; }
KeyboardLayout RecoveryKeyboardLayout() { return KeyboardLayout::kQwerty; }
std::string RecoveryUiFont() { return {}; }
void RecoveryVibrate(Haptic) {}
}

namespace aeraui::smb {
Result List(const NasConfig &, const std::string &share, const std::string &path,
            const std::atomic<bool> &cancel) {
  ++g_active;
  for (int i = 0; i < (g_slow ? 100 : 3); ++i) {
    if (cancel.load()) { ++g_cancelled; --g_active; return {}; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Result result;
  if (share.empty()) result.directories = {"Public"};
  else if (share == "Public" && path.empty()) result.directories = {
      "Android", "A very long folder name with spaces that must remain readable in the large interface size"};
  else if (share == "Public" && path == "Android") result.directories = {"AERA"};
  --g_active;
  return result;
}
}

static void Tick(int count = 12) {
  for (int i = 0; i < count; ++i) {
    lv_tick_inc(16);
    lv_timer_handler();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

static lv_obj_t *Find(lv_obj_t *root, const char *text) {
  if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text))
    return lv_obj_get_parent(root);
  for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
    if (auto *found = Find(lv_obj_get_child(root, i), text)) return found;
  return nullptr;
}

static void Click(lv_obj_t *screen, const char *text, bool translate = true) {
  auto *button = Find(screen, translate ? i18n::Translate(text) : text);
  assert(button);
  assert(!lv_obj_has_state(button, LV_STATE_DISABLED));
  lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
  Tick();
}

static lv_obj_t *FindType(lv_obj_t *root, const lv_obj_class_t *type) {
  if (lv_obj_check_type(root, type)) return root;
  for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
    if (auto *found = FindType(lv_obj_get_child(root, i), type)) return found;
  return nullptr;
}

static void CheckButton(lv_obj_t *button) {
  assert(button);
  lv_obj_update_layout(button);
  auto *label = lv_obj_get_child(button, 0);
  lv_area_t outer{}, inner{};
  lv_obj_get_coords(button, &outer);
  lv_obj_get_coords(label, &inner);
  assert(inner.x1 >= outer.x1 && inner.x2 <= outer.x2);
  assert(inner.y1 >= outer.y1 && inner.y2 <= outer.y2);
}

int main(int argc, char **) {
  const bool preview = argc > 1;
  lv_init();
  i18n::Initialize("en");
  auto *display = lv_display_create(1440, 3168);
  std::vector<uint8_t> frame(3168 * 3168 * 4);
  lv_display_set_buffers(display, frame.data(), nullptr, frame.size(), LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display, [](lv_display_t *value, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(value);
  });
  auto *home = lv_screen_active();
  for (const char *language : {"en", "de_DE"}) {
    if (preview && !strcmp(language, "en")) continue;
    i18n::SetLanguage(language);
    for (bool landscape : {false, true}) {
      lv_display_set_resolution(display, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      for (int size = 0; size < 4; ++size) {
        if (preview && size != 3) continue;
        printf("NAS UI: %s, %s, size %d\n", language, landscape ? "landscape" : "portrait", size);
        fflush(stdout);
        design::ApplyInterfaceSize(size);
        g_status = {};
        g_status.supported = true;
        g_status.config.type = "smb";
        g_status.config.host = "test-server";
        auto *screen = lv_obj_create(nullptr);
        BuildNasScene(screen, [](Action, void *) { ++g_actions; }, nullptr);
        lv_screen_load(screen);
        Tick();
        auto save = [&](const char *name) {
          if (!preview) return;
          png_image image{};
          image.version = PNG_IMAGE_VERSION;
          image.width = lv_display_get_horizontal_resolution(display);
          image.height = lv_display_get_vertical_resolution(display);
          image.format = PNG_FORMAT_BGRA;
          const std::string file = std::string("/tmp/nas-") + name +
              (landscape ? "-landscape.png" : "-portrait.png");
          assert(png_image_write_to_file(&image, file.c_str(), 0, frame.data(), 0, nullptr));
        };
        save("settings");
        assert(!Find(screen, i18n::Translate("Browse shares")));
        auto *advanced = Find(screen, i18n::Translate("Advanced"));
        auto *folders = Find(screen, i18n::Translate("Browse folders"));
        assert(advanced && folders);
        assert(lv_obj_get_width(advanced) == lv_obj_get_width(folders));
        assert(lv_obj_get_y(advanced) > lv_obj_get_y(folders));
        assert(Find(screen, i18n::Translate("Write cache")));
        Click(screen, "Browse folders");
        assert(lv_obj_has_state(Find(screen, i18n::Translate("Use this folder")), LV_STATE_DISABLED));
        Click(screen, "Public", false);
        save("picker");
        CheckButton(Find(screen, i18n::Translate("Use this folder")));
        Click(screen, "Android", false);
        Click(screen, "AERA", false);
        assert(Find(screen, i18n::Translate("No folders found")));
        Click(screen, "Use this folder");
        assert(g_status.config.share == "Public" && g_status.config.path == "Android/AERA");
        assert(g_actions == 0);
        Click(screen, "Browse folders");
        Click(screen, "Public", false);
        Click(screen, "Android", false);
        Click(screen, "AERA", false);
        assert(widgets::DismissModal(screen));
        Tick();
        assert(Find(screen, "AERA"));
        Click(screen, LV_SYMBOL_CLOSE, false);
        Click(screen, "Browse folders");
        Click(screen, "SMB share name");
        auto *input = FindType(screen, &lv_textarea_class);
        assert(input);
        lv_textarea_set_text(input, "Hidden");
        lv_obj_send_event(input, LV_EVENT_READY, nullptr);
        Tick();
        assert(Find(screen, "//test-server/Hidden"));
        Click(screen, "Use this folder");
        assert(g_status.config.share == "Hidden" && g_status.config.path.empty());
        Click(screen, "Advanced");
        assert(Find(screen, i18n::Translate("Remote folder or path")));
        assert(Find(screen, i18n::Translate("Write cache")));
        g_slow = true;
        Click(screen, "Browse folders");
        lv_screen_load(home);
        lv_obj_delete(screen);
        Tick();
        g_slow = false;
        while (g_active.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }
  assert(g_cancelled.load() == (preview ? 2 : 16));
  lv_deinit();
  puts("NAS picker: English/German, four sizes, both orientations, navigation, hidden shares and close cancellation passed");
}
