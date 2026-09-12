/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "ui_components.hpp"
#include "browser/runtime.hpp"
#include "media/launcher.hpp"
#include "media/protocol.hpp"
#include "media/session.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace recovery_ui2 {
namespace {
using namespace widgets;
struct MediaFile { std::string path; std::string name; bool video = false; };
bool MediaSuffix(const std::string &name, bool &video) {
  auto dot = name.find_last_of('.'); if (dot == std::string::npos) return false;
  std::string suffix = name.substr(dot);
  for (char &c : suffix) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  video = suffix == ".mp4" || suffix == ".mkv" || suffix == ".webm" || suffix == ".mov";
  return video || suffix == ".mp3" || suffix == ".flac" || suffix == ".wav" ||
      suffix == ".m4a" || suffix == ".aac" || suffix == ".ogg" || suffix == ".opus";
}
void ScanMedia(const std::string &path, int depth, std::vector<MediaFile> &out) {
  if (depth > 5 || out.size() >= 400) return;
  DIR *directory = opendir(path.c_str()); if (!directory) return;
  while (auto *entry = readdir(directory)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || entry->d_name[0] == '.') continue;
    const std::string child = path + "/" + entry->d_name;
    struct stat info{}; if (lstat(child.c_str(), &info) || S_ISLNK(info.st_mode)) continue;
    if (S_ISDIR(info.st_mode)) ScanMedia(child, depth + 1, out);
    else if (S_ISREG(info.st_mode)) {
      bool video = false; if (MediaSuffix(entry->d_name, video)) out.push_back({child, entry->d_name, video});
    }
    if (out.size() >= 400) break;
  }
  closedir(directory);
}
std::string Time(int64_t ms) {
  if (ms < 0) ms = 0; const int seconds = static_cast<int>(ms / 1000);
  char value[32]; snprintf(value, sizeof(value), "%d:%02d", seconds / 60, seconds % 60);
  return value;
}
struct MediaScene {
  lv_obj_t *screen = nullptr, *video = nullptr, *empty = nullptr;
  lv_obj_t *title = nullptr, *time = nullptr, *play = nullptr, *progress = nullptr;
  lv_image_dsc_t descriptor{};
  media::Session session; media::Process process; web::Preparation preparation;
  std::thread worker; lv_timer_t *timer = nullptr;
  bool launched = false, has_frame = false;
  std::string selected;
  ~MediaScene() {
    if (timer) lv_timer_delete(timer); preparation.cancel.store(true);
    if (worker.joinable()) worker.join();
    if (video) { lv_image_set_src(video, nullptr); lv_image_cache_drop(&descriptor); }
    if (session.Connected()) session.Send(media::Kind::kClose);
    session.Close(); process.Stop(); web::RemoveRuntime(preparation.directory);
  }
};
void Open(MediaScene *scene, const std::string &path) {
  scene->selected = path;
  if (!scene->session.Connected()) {
    lv_label_set_text(scene->title, "Media engine is still preparing"); return;
  }
  scene->session.Send(media::Kind::kOpen, path.c_str());
  lv_label_set_text(scene->title, path.substr(path.find_last_of('/') + 1).c_str());
}
}  // namespace

void BuildMediaScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *scene = new MediaScene; scene->screen = screen;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<MediaScene *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, scene);
  Header(screen, "AERA Media", "Music and video from internal storage.", callback, context);
  const bool landscape = Landscape(screen);
  auto *stage = lv_obj_create(screen); Panel(stage, 42, Color(0x15171a));
  lv_obj_set_pos(stage, 64, landscape ? 340 : 440);
  lv_obj_set_size(stage, landscape ? 1450 : 1312,
                  landscape ? 650 : 850);
  lv_obj_set_style_clip_corner(stage, true, 0);
  scene->empty = Label(stage, LV_SYMBOL_AUDIO "\nChoose something to play",
                       &lv_font_montserrat_48, kMutedStrong);
  lv_obj_set_style_text_align(scene->empty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(scene->empty, 34, 0); lv_obj_center(scene->empty);
  scene->video = lv_image_create(stage); lv_obj_center(scene->video);
  lv_obj_add_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
  scene->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  scene->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  scene->descriptor.header.flags = LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;
  scene->descriptor.header.w = media::kWidth; scene->descriptor.header.h = media::kHeight;
  scene->descriptor.header.stride = media::kWidth * 4;
  scene->descriptor.data_size = media::kFrameBytes;
  scene->title = Label(screen, "Preparing media engine", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(scene->title, 80, landscape ? 1010 : 1325);
  SingleLineLabel(scene->title, landscape ? 1120 : 1040,
                  &lv_font_montserrat_32);
  scene->time = Label(screen, "0:00 / 0:00", &lv_font_montserrat_24, kMuted);
  if (landscape) lv_obj_set_pos(scene->time, 1240, 1018);
  else lv_obj_align(scene->time, LV_ALIGN_TOP_RIGHT, -80, 1332);
  scene->progress = lv_bar_create(screen);
  lv_obj_set_pos(scene->progress, 80, landscape ? 1070 : 1390);
  lv_obj_set_size(scene->progress, landscape ? 1418 : 1280, 10);
  lv_bar_set_range(scene->progress, 0, 1000);
  lv_obj_set_style_bg_color(scene->progress, kAccent, LV_PART_INDICATOR);
  auto add_control = [&](const char *text, int x, auto action) {
    auto *button = Button(screen, text, action);
    lv_obj_set_pos(button, x, landscape ? 1110 : 1435);
    lv_obj_set_size(button, landscape ? 430 : 380,
                    landscape ? 108 : 112);
    lv_obj_set_style_radius(button, 56, 0); return button;
  };
  add_control("-15 sec", 80, [scene] { scene->session.Send(media::Kind::kSeekRelative, nullptr, -15); });
  scene->play = add_control(LV_SYMBOL_PLAY "  Play / Pause", landscape ? 574 : 530, [scene] {
    if (!scene->session.Connected()) return;
    if (scene->session.Duration() <= 0 && !scene->selected.empty())
      scene->session.Send(media::Kind::kOpen, scene->selected.c_str());
    else
      scene->session.Send(scene->session.Playing() ? media::Kind::kPause : media::Kind::kPlay);
  });
  add_control("+15 sec", landscape ? 1068 : 980, [scene] { scene->session.Send(media::Kind::kSeekRelative, nullptr, 15); });
  auto *library_title = Label(screen, "LIBRARY", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(library_title, 3, 0);
  lv_obj_set_pos(library_title, landscape ? 1580 : 80,
                 landscape ? 306 : 1610);
  auto *library = Scroll(screen, landscape ? 350 : 1660,
                         landscape ? 900 : 1210);
  if (landscape) {
    lv_obj_set_x(library, 1560);
    lv_obj_set_width(library, 1544);
  }
  std::vector<MediaFile> files;
  for (const char *root : {"/sdcard/Music", "/sdcard/Movies", "/sdcard/Download",
                           "/sdcard/DCIM", "/sdcard/AERA"}) ScanMedia(root, 0, files);
  std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
  if (!files.empty()) scene->selected = files.front().path;
  int y = 0;
  for (const auto &file : files) {
    Row(library, y, file.video ? LV_SYMBOL_VIDEO : LV_SYMBOL_AUDIO, file.name,
        file.video ? "Video" : "Audio", [scene, path = file.path] { Open(scene, path); }, LV_SYMBOL_PLAY);
    y += 174;
  }
  if (files.empty()) {
    auto *empty = Label(library, "No supported music or videos found.", &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 160);
  }
  Navigation(screen, Action::kBackHome, callback, context, true);
  scene->worker = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, "media", "app-runtime", "media");
  });
  scene->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *scene = static_cast<MediaScene *>(lv_timer_get_user_data(timer));
    if (!scene->preparation.done.load(std::memory_order_acquire)) return;
    if (!scene->preparation.verified) {
      lv_label_set_text(scene->title, scene->preparation.error.c_str()); return;
    }
    if (!scene->launched) {
      scene->launched = true; int frame = -1, control = -1; std::string error;
      if (!scene->process.Start(scene->preparation.directory, frame, control, error) ||
          !scene->session.Adopt(frame, control)) {
        lv_label_set_text(scene->title, error.empty() ? scene->session.Status().c_str() : error.c_str());
        return;
      }
      lv_label_set_text(scene->title, "Choose something to play");
    }
    if (!scene->session.Connected()) return;
    scene->session.AcknowledgeFrame();
    if (scene->session.Poll()) {
      scene->descriptor.data = scene->session.Pixels();
      lv_image_cache_drop(&scene->descriptor); lv_image_set_src(scene->video, &scene->descriptor);
      lv_obj_invalidate(scene->video); lv_obj_remove_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(scene->empty, LV_OBJ_FLAG_HIDDEN); scene->has_frame = true;
    }
    const int64_t duration = scene->session.Duration();
    const int value = duration > 0 ? std::clamp<int64_t>(scene->session.Position() * 1000 / duration, 0, 1000) : 0;
    lv_bar_set_value(scene->progress, value, LV_ANIM_OFF);
    const std::string time = Time(scene->session.Position()) + " / " + Time(duration);
    lv_label_set_text(scene->time, time.c_str());
    if (!scene->session.Status().empty())
      lv_label_set_text(scene->title, scene->session.Status().c_str());
  }, 12, scene);
}
}  // namespace recovery_ui2
