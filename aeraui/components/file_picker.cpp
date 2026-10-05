/* SPDX-License-Identifier: Apache-2.0 */
#include "file_picker.hpp"

#include "file_manager.hpp"
#include "picture_decode.hpp"
#include "phone_keyboard.hpp"
#include "ui_components.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <aeraui/backend.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <set>
#include <strings.h>
#include <sys/stat.h>
#include <thread>

namespace aeraui::file_picker {
namespace {
using namespace design;
using namespace widgets;

constexpr int kRowHeight = 142;
constexpr int kThumbnailSize = 76;
constexpr size_t kPage = 100;

// A folder listing made on a worker thread; the picker polls `ready`.
struct Listing {
  std::atomic<bool> ready{false};
  std::string directory;
  std::vector<Entry> entries;
  std::string error;
};

struct Thumbnail {
  std::string path;
  int y = 0;
  lv_obj_t *preview = nullptr;
  lv_obj_t *placeholder = nullptr;
  lv_obj_t *image = nullptr;
  std::shared_ptr<PictureData> data;
  lv_image_dsc_t descriptor{};
  int state = 0;  // 0 waiting, 1 decoding, 2 shown, 3 failed
};

struct Picker {
  lv_obj_t *screen = nullptr;
  lv_obj_t *overlay = nullptr;
  lv_obj_t *where = nullptr;
  lv_obj_t *summary = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *confirm = nullptr;
  lv_obj_t *name_button = nullptr;
  lv_timer_t *timer = nullptr;
  Request request;
  Chosen chosen;
  Cancelled cancelled;
  bool finished = false;
  // The folder shown; empty for the list of roots of a confined picker.
  std::string directory;
  std::shared_ptr<Listing> listing;
  std::vector<Entry> entries;
  size_t visible = kPage;
  std::set<std::string> selected;
  std::string name;
  std::vector<std::unique_ptr<Thumbnail>> thumbnails;
  int decoding = -1;
};

// Open pickers by overlay, for Dismiss() and Forget().
std::map<lv_obj_t *, Picker *> gPickers;

void Render(Picker *picker);

void DropThumbnails(Picker *picker) {
  for (auto &thumbnail : picker->thumbnails) {
    if (thumbnail->data) thumbnail->data->cancelled = true;
    if (thumbnail->image) {
      lv_obj_delete(thumbnail->image);
      thumbnail->image = nullptr;
      lv_image_cache_drop(&thumbnail->descriptor);
    }
  }
  picker->thumbnails.clear();
  picker->decoding = -1;
}

bool ThumbnailVisible(const Picker *picker, const Thumbnail *thumbnail) {
  const int scroll = lv_obj_get_scroll_y(picker->list);
  const int viewport = lv_obj_get_height(picker->list);
  return thumbnail->y + kRowHeight >= scroll - kRowHeight &&
         thumbnail->y <= scroll + viewport + kRowHeight;
}

void ShowThumbnail(Thumbnail *thumbnail) {
  if (!thumbnail->data || !thumbnail->data->pixels) {
    thumbnail->state = 3;
    thumbnail->data.reset();
    return;
  }
  auto &descriptor = thumbnail->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = thumbnail->data->width;
  descriptor.header.h = thumbnail->data->height;
  descriptor.header.stride = thumbnail->data->width * 4;
  descriptor.data_size = thumbnail->data->width * thumbnail->data->height * 4;
  descriptor.data = thumbnail->data->pixels;
  thumbnail->image = lv_image_create(thumbnail->preview);
  lv_image_set_src(thumbnail->image, &descriptor);
  lv_image_set_antialias(thumbnail->image, true);
  const uint32_t scale = std::max(
      (kThumbnailSize * 256U + thumbnail->data->width - 1) /
          thumbnail->data->width,
      (kThumbnailSize * 256U + thumbnail->data->height - 1) /
          thumbnail->data->height);
  lv_image_set_scale(thumbnail->image, scale);
  lv_obj_center(thumbnail->image);
  lv_obj_remove_flag(thumbnail->image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(thumbnail->placeholder, LV_OBJ_FLAG_HIDDEN);
  thumbnail->state = 2;
}

// Decodes one visible thumbnail at a time, as Telegram's picker did.
void TickThumbnails(Picker *picker) {
  if (picker->decoding >= 0) {
    auto *thumbnail = picker->thumbnails[picker->decoding].get();
    if (!thumbnail->data->ready.load(std::memory_order_acquire)) return;
    ShowThumbnail(thumbnail);
    picker->decoding = -1;
  }
  for (size_t index = 0; index < picker->thumbnails.size(); ++index) {
    auto *thumbnail = picker->thumbnails[index].get();
    if (thumbnail->state != 0 || !ThumbnailVisible(picker, thumbnail))
      continue;
    thumbnail->state = 1;
    thumbnail->data = std::make_shared<PictureData>();
    picker->decoding = static_cast<int>(index);
    std::thread([data = thumbnail->data, path = thumbnail->path] {
      DecodePictureThumbnail(path, kThumbnailSize, kThumbnailSize, *data);
    }).detach();
    return;
  }
}

void Tick(lv_timer_t *timer) {
  auto *picker = static_cast<Picker *>(lv_timer_get_user_data(timer));
  if (picker->listing &&
      picker->listing->ready.load(std::memory_order_acquire)) {
    auto listing = std::move(picker->listing);
    if (listing->directory == picker->directory) {
      picker->entries = std::move(listing->entries);
      picker->visible = kPage;
      const std::string summary = !listing->error.empty()
          ? i18n::Format("Cannot open folder: %s", listing->error.c_str())
          : picker->entries.empty()
              ? std::string("No files in this view")
              : i18n::Format("%zu items", picker->entries.size());
      i18n::BindLabel(picker->summary, summary.c_str());
      Render(picker);
      lv_obj_scroll_to_y(picker->list, 0, LV_ANIM_OFF);
    }
  }
  TickThumbnails(picker);
}

std::vector<std::pair<std::string, std::string>> Volumes(
    const Request &request) {
  if (!request.roots.empty()) return request.roots;
  std::vector<std::pair<std::string, std::string>> volumes;
  for (const auto &volume : RecoveryVolumes("storage"))
    volumes.emplace_back(volume.name, volume.path);
  return volumes;
}

void Navigate(Picker *picker, const std::string &directory) {
  picker->directory = directory;
  picker->entries.clear();
  picker->selected.clear();
  DropThumbnails(picker);
  i18n::BindLabel(picker->where, directory.empty() ? "Available storage"
                                                   : directory.c_str());
  if (directory.empty()) {
    for (const auto &root : picker->request.roots)
      picker->entries.push_back({root.first, root.second, true, 0});
    i18n::BindLabel(picker->summary, picker->entries.empty()
                                         ? "No readable storage is mounted"
                                         : "Storage locations");
    picker->listing.reset();
    Render(picker);
    return;
  }
  i18n::BindLabel(picker->summary, "Loading");
  Render(picker);
  auto listing = std::make_shared<Listing>();
  listing->directory = directory;
  picker->listing = listing;
  std::thread([listing, request = picker->request,
               hidden = RecoveryPreference(Preference::kHiddenFiles)] {
    listing->entries = List(request, listing->directory, hidden,
                            listing->error);
    listing->ready.store(true, std::memory_order_release);
  }).detach();
}

void Finish(Picker *picker, std::vector<std::string> paths) {
  if (picker->finished) return;
  picker->finished = true;
  auto chosen = std::move(picker->chosen);
  lv_obj_delete_async(picker->overlay);
  if (chosen) chosen(std::move(paths));
}

void UpdateBar(Picker *picker) {
  if (!picker->confirm) return;
  bool enabled = true;
  switch (picker->request.mode) {
    case Mode::kFiles: {
      const std::string label = picker->selected.empty()
          ? std::string("Choose")
          : i18n::Format("Choose (%zu)", picker->selected.size());
      i18n::BindLabel(lv_obj_get_child(picker->confirm, 0), label.c_str());
      enabled = !picker->selected.empty();
      break;
    }
    case Mode::kFolder:
      enabled = !picker->directory.empty();
      break;
    case Mode::kSave:
      enabled = !picker->directory.empty() && !picker->name.empty();
      if (picker->name_button)
        i18n::BindLabel(lv_obj_get_child(picker->name_button, 0),
                        picker->name.empty() ? "File name"
                                             : picker->name.c_str());
      break;
    case Mode::kFile:
      break;
  }
  if (enabled) lv_obj_remove_state(picker->confirm, LV_STATE_DISABLED);
  else lv_obj_add_state(picker->confirm, LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(picker->confirm, enabled ? LV_OPA_COVER : LV_OPA_40,
                          0);
}

void Choose(Picker *picker, const Entry &entry) {
  if (entry.directory) {
    Navigate(picker, entry.path);
    return;
  }
  switch (picker->request.mode) {
    case Mode::kFile:
      Finish(picker, {entry.path});
      break;
    case Mode::kFiles:
      if (picker->selected.erase(entry.path) == 0) {
        if (picker->request.max_count &&
            picker->selected.size() >= picker->request.max_count) {
          Sheet(picker->screen, "Choose files",
                i18n::Format("You can choose up to %zu files.",
                             picker->request.max_count));
          return;
        }
        picker->selected.insert(entry.path);
      }
      Render(picker);
      break;
    case Mode::kSave:
      picker->name = entry.name;
      UpdateBar(picker);
      break;
    case Mode::kFolder:
      break;
  }
}

void UtilityRow(Picker *picker, int y, const char *symbol, const char *title,
                const std::string &detail, Handler action) {
  const int width = lv_obj_get_width(picker->list);
  auto *row = Button(picker->list, "", std::move(action));
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_size(row, width, kRowHeight);
  lv_obj_set_style_radius(row, 26, 0);
  lv_obj_set_style_bg_color(row, kMainPanel, 0);
  auto *icon = IconPlate(row, symbol, kAccent, kMainPanel, kThumbnailSize);
  lv_obj_set_pos(icon, 22, 32);
  auto *name = Label(row, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 126, 24);
  SingleLineLabel(name, width - 280, &lv_font_montserrat_32);
  auto *copy = Label(row, detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 126, 82);
  SingleLineLabel(copy, width - 280, &lv_font_montserrat_24);
}

void EntryRow(Picker *picker, int y, const Entry &entry) {
  const int width = lv_obj_get_width(picker->list);
  const bool selected = picker->selected.count(entry.path) != 0;
  auto *row = Button(picker->list, "", [picker, entry] {
    Choose(picker, entry);
  });
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_size(row, width, kRowHeight);
  lv_obj_set_style_radius(row, selected ? 26 : 0, 0);
  lv_obj_set_style_bg_color(row, selected ? kAccentSoft : kMainCanvas, 0);
  lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  if (!entry.directory && IsPicture(entry.path)) {
    auto thumbnail = std::make_unique<Thumbnail>();
    thumbnail->path = entry.path;
    thumbnail->y = y;
    thumbnail->preview = lv_obj_create(row);
    Clear(thumbnail->preview);
    lv_obj_set_pos(thumbnail->preview, 22, 32);
    lv_obj_set_size(thumbnail->preview, kThumbnailSize, kThumbnailSize);
    lv_obj_set_style_radius(thumbnail->preview, 18, 0);
    lv_obj_set_style_clip_corner(thumbnail->preview, true, 0);
    lv_obj_set_style_bg_color(thumbnail->preview, kMainPanel, 0);
    lv_obj_set_style_bg_opa(thumbnail->preview, LV_OPA_COVER, 0);
    lv_obj_remove_flag(thumbnail->preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(thumbnail->preview, LV_OBJ_FLAG_CLICKABLE);
    thumbnail->placeholder = Label(thumbnail->preview, LV_SYMBOL_IMAGE,
                                   &lv_font_montserrat_28, kAccent);
    lv_obj_center(thumbnail->placeholder);
    picker->thumbnails.push_back(std::move(thumbnail));
  } else {
    auto *icon = IconPlate(row, entry.directory ? LV_SYMBOL_DIRECTORY
                                                : LV_SYMBOL_FILE,
                           entry.directory ? kCyan : kAccent, kMainPanel,
                           kThumbnailSize);
    lv_obj_set_pos(icon, 22, 32);
  }
  auto *name = Label(row, entry.name.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 126, 24);
  SingleLineLabel(name, width - 280, &lv_font_montserrat_32);
  const std::string detail = picker->directory.empty() ? entry.path
      : entry.directory ? std::string("Folder") : Size(entry.size);
  auto *copy = Label(row, detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 126, 82);
  SingleLineLabel(copy, width - 280, &lv_font_montserrat_24);
  const char *end = entry.directory ? LV_SYMBOL_RIGHT
      : selected ? LV_SYMBOL_OK
      : picker->request.mode == Mode::kFiles ? "" : LV_SYMBOL_PLUS;
  auto *mark = Label(row, end, &lv_font_montserrat_32,
                     entry.directory ? kMutedStrong : kCyan);
  lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -34, 0);
}

void Render(Picker *picker) {
  const int32_t scroll = lv_obj_get_scroll_y(picker->list);
  DropThumbnails(picker);
  lv_obj_clean(picker->list);
  lv_obj_update_layout(picker->list);
  int y = 0;
  if (!picker->directory.empty() &&
      (picker->directory != "/" || !picker->request.roots.empty())) {
    const std::string parent = Parent(picker->request, picker->directory);
    UtilityRow(picker, y, LV_SYMBOL_UP, "Parent folder",
               parent.empty() ? std::string("Storage locations") : parent,
               [picker, parent] { Navigate(picker, parent); });
    y += kRowHeight;
  }
  const size_t count = std::min(picker->entries.size(), picker->visible);
  for (size_t index = 0; index < count; ++index) {
    EntryRow(picker, y, picker->entries[index]);
    y += kRowHeight;
  }
  if (count < picker->entries.size())
    UtilityRow(picker, y, LV_SYMBOL_PLUS, "Show more files",
               "Next 100 entries", [picker] {
      picker->visible += kPage;
      Render(picker);
    });
  lv_obj_scroll_to_y(picker->list, scroll, LV_ANIM_OFF);
  UpdateBar(picker);
}

// Asks for the file name in kSave, like Files' name dialog.
void AskName(Picker *picker) {
  auto *overlay = lv_obj_create(picker->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
  const bool landscape = Landscape(picker->screen);
  const int screen_width = lv_obj_get_width(picker->screen);
  const int width = landscape ? std::min(1900, screen_width - 128)
                              : std::min(1312, screen_width - 80);
  const int height = landscape ? 1250 : 1450;
  const int margin = 48;
  const int button_height = 126;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 44, kMainSheet);
  lv_obj_set_size(sheet, width, height);
  lv_obj_align(sheet, landscape ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0,
               landscape ? 0 : -40);
  lv_obj_set_style_pad_all(sheet, 0, 0);
  auto *heading = Label(sheet, "File name", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, margin, 42);
  auto *input = TextArea(sheet);
  lv_obj_set_pos(input, margin, 120);
  lv_obj_set_size(input, width - 2 * margin, 126);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_max_length(input, NAME_MAX);
  lv_textarea_set_text(input, picker->name.c_str());
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_bg_color(input, kMainPanel, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(input, 2, 0);
  lv_obj_set_style_radius(input, 24, 0);
  lv_obj_set_style_pad_all(input, 28, 0);
  auto *error = Label(sheet, "", &lv_font_montserrat_24, kRed);
  lv_obj_set_pos(error, margin + 8, 264);
  lv_obj_set_width(error, width - 2 * margin - 16);
  auto *keyboard = lv_keyboard_create(sheet);
  phone_keyboard::Apply(keyboard);
  const int button_y = height - margin - button_height;
  const int keyboard_y = 320;
  lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, margin, keyboard_y);
  lv_obj_set_size(keyboard, width - 2 * margin,
                  button_y - keyboard_y - 48);
  lv_keyboard_set_textarea(keyboard, input);
  const int button_width = (width - 2 * margin - 20) / 2;
  auto *cancel = Button(sheet, "Cancel", [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_pos(cancel, margin, button_y);
  lv_obj_set_size(cancel, button_width, button_height);
  auto *accept = Button(sheet, "OK", [picker, input, error, overlay] {
    const std::string name = lv_textarea_get_text(input);
    std::string message;
    if (!file_manager::ValidName(name, message)) {
      i18n::BindLabel(error, message.c_str());
      return;
    }
    picker->name = name;
    UpdateBar(picker);
    lv_obj_delete_async(overlay);
  }, true);
  lv_obj_set_pos(accept, margin + button_width + 20, button_y);
  lv_obj_set_size(accept, button_width, button_height);
  lv_obj_add_event_cb(keyboard, [](lv_event_t *event) {
    lv_obj_send_event(static_cast<lv_obj_t *>(lv_event_get_user_data(event)),
                      LV_EVENT_CLICKED, nullptr);
  }, LV_EVENT_READY, accept);
  lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
}

void Confirm(Picker *picker) {
  switch (picker->request.mode) {
    case Mode::kFiles:
      if (!picker->selected.empty())
        Finish(picker, {picker->selected.begin(), picker->selected.end()});
      break;
    case Mode::kFolder:
      if (!picker->directory.empty()) Finish(picker, {picker->directory});
      break;
    case Mode::kSave: {
      if (picker->directory.empty() || picker->name.empty()) return;
      const std::string path = Join(picker->directory, picker->name);
      if (picker->request.max_path && path.size() >= picker->request.max_path) {
        Sheet(picker->screen, "File name", "This name is too long.");
        return;
      }
      struct stat info{};
      if (stat(path.c_str(), &info) == 0) {
        if (S_ISDIR(info.st_mode)) {
          Sheet(picker->screen, "File name",
                "A folder with this name already exists.");
          return;
        }
        Sheet(picker->screen, "Replace file?",
              i18n::Format("%s already exists in this folder.",
                           picker->name.c_str()),
              [picker, path] { Finish(picker, {path}); }, 0, true);
        return;
      }
      Finish(picker, {path});
      break;
    }
    case Mode::kFile:
      break;
  }
}

void Delete(lv_event_t *event) {
  auto *picker = static_cast<Picker *>(lv_event_get_user_data(event));
  gPickers.erase(picker->overlay);
  if (picker->timer) lv_timer_delete(picker->timer);
  DropThumbnails(picker);
  auto cancelled = std::move(picker->cancelled);
  const bool finished = picker->finished;
  delete picker;
  if (!finished && cancelled) cancelled();
}

Picker *Of(lv_obj_t *overlay) {
  const auto found = gPickers.find(overlay);
  return found == gPickers.end() ? nullptr : found->second;
}
bool Listable(const std::string &path) {
  DIR *directory = opendir(path.c_str());
  if (directory == nullptr) return false;
  closedir(directory);
  return true;
}
}  // namespace

lv_obj_t *Show(lv_obj_t *screen, Request request, Chosen chosen,
               Cancelled cancelled) {
  auto *picker = new Picker;
  picker->screen = screen;
  picker->request = std::move(request);
  picker->chosen = std::move(chosen);
  picker->cancelled = std::move(cancelled);
  picker->name = picker->request.suggested_name;
  const Request &req = picker->request;

  auto *overlay = picker->overlay = lv_obj_create(screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_add_event_cb(overlay, Delete, LV_EVENT_DELETE, picker);
  gPickers[overlay] = picker;
  const int status_height = StatusBarHeight();
  const int width = lv_obj_get_width(screen);
  const int height = lv_obj_get_height(screen) - status_height;
  lv_obj_set_pos(overlay, 0, status_height);
  lv_obj_set_size(overlay, width, height);
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

  auto *close = Button(overlay, LV_SYMBOL_LEFT, [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_pos(close, 24, 20);
  lv_obj_set_size(close, 120, 112);
  const char *fallback = req.mode == Mode::kFolder ? "Choose a folder"
      : req.mode == Mode::kSave ? "Save as"
      : req.mode == Mode::kFiles ? "Choose files" : "Choose a file";
  auto *title = Label(overlay, req.title.empty() ? fallback : req.title.c_str(),
                      &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 174, 22);
  SingleLineLabel(title, width - 198, &lv_font_montserrat_48);
  picker->where = Label(overlay, "", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(picker->where, 174, 84);
  SingleLineLabel(picker->where, width - 198, &lv_font_montserrat_24);

  // Files' Storage and Root buttons: Storage steps through the volumes (or
  // the caller's roots), Root opens / when not confined.
  auto *storage = Button(overlay, "Storage  " LV_SYMBOL_DOWN, [picker] {
    const auto volumes = Volumes(picker->request);
    if (volumes.empty()) {
      Sheet(picker->screen, "Storage", "No storage volumes are available.");
      return;
    }
    size_t next = 0;
    for (size_t index = 0; index < volumes.size(); ++index)
      if (volumes[index].second == picker->directory)
        next = (index + 1) % volumes.size();
    Navigate(picker, volumes[next].second);
  });
  lv_obj_set_pos(storage, 24, 152);
  lv_obj_set_size(storage, 300, 112);
  if (req.roots.empty()) {
    auto *root = Button(overlay, "Root", [picker] { Navigate(picker, "/"); });
    lv_obj_set_pos(root, 348, 152);
    lv_obj_set_size(root, 180, 112);
  }
  picker->summary = Label(overlay, "", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(picker->summary, req.roots.empty() ? 552 : 348, 194);
  SingleLineLabel(picker->summary, width - (req.roots.empty() ? 576 : 372),
                  &lv_font_montserrat_24);

  const bool bar = req.mode != Mode::kFile;
  const int bar_height = bar ? 168 : 0;
  picker->list = lv_obj_create(overlay);
  Clear(picker->list);
  lv_obj_set_pos(picker->list, 24, 288);
  lv_obj_set_size(picker->list, width - 48, height - 312 - bar_height);
  lv_obj_add_flag(picker->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(picker->list, LV_DIR_VER);
  lv_obj_set_style_bg_opa(picker->list, LV_OPA_TRANSP, 0);

  if (bar) {
    const int y = height - bar_height + 16;
    const char *action = req.mode == Mode::kSave ? "Save"
        : req.mode == Mode::kFolder ? "Use this folder" : "Choose";
    int confirm_x = 24;
    if (req.mode == Mode::kSave) {
      picker->name_button = Button(overlay, "", [picker] { AskName(picker); });
      lv_obj_set_pos(picker->name_button, 24, y);
      lv_obj_set_size(picker->name_button, width - 48 - 380 - 24, 126);
      confirm_x = width - 24 - 380;
    }
    picker->confirm = Button(overlay, action, [picker] { Confirm(picker); },
                             true);
    lv_obj_set_pos(picker->confirm, confirm_x, y);
    lv_obj_set_size(picker->confirm,
                    req.mode == Mode::kSave ? 380 : width - 48, 126);
  }

  std::string start = req.start;
  while (start.size() > 1 && start.back() == '/') start.pop_back();
  struct stat info{};
  if (start.empty() || !Allowed(req, start) || stat(start.c_str(), &info) != 0 ||
      !S_ISDIR(info.st_mode)) {
    start = req.roots.empty() ? RecoveryStorage() : std::string{};
    if (req.roots.empty() && start.empty()) start = "/sdcard";
  }
  // The current storage can be a folder that cannot be listed, such as
  // /data/media before Android has made user 0's folder in it; start in
  // internal storage instead of on an error.
  if (req.roots.empty() && !Listable(start)) {
    for (const char *fallback : {"/data/media/0", "/sdcard"}) {
      if (Listable(fallback)) {
        start = fallback;
        break;
      }
    }
  }
  picker->timer = lv_timer_create(Tick, 35, picker);
  Navigate(picker, start);
  return overlay;
}

void Dismiss(lv_obj_t *picker) {
  if (Of(picker)) lv_obj_delete_async(picker);
}

void Forget(lv_obj_t *overlay) {
  if (auto *picker = Of(overlay)) {
    picker->chosen = nullptr;
    picker->cancelled = nullptr;
  }
}

}  // namespace aeraui::file_picker
