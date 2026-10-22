/* SPDX-License-Identifier: Apache-2.0 */
#include "font_browser.hpp"
#include "phone_keyboard.hpp"
#include "ui_components.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace aeraui {
namespace {
using namespace widgets;
using namespace design;
using fontsource::Details;
using fontsource::Face;
using fontsource::Family;
using fonts::SavedFont;

constexpr const char* categories[] = {"", "sans-serif", "serif", "monospace", "display", "handwriting"};
constexpr const char* category_names[] = {"All fonts", "Sans serif", "Serif", "Monospace", "Display", "Handwriting"};

struct FontJob {
  std::atomic<bool> cancel{false};
  std::mutex mutex;
  bool done = false;
  std::string error, directory;
  std::vector<Family> catalog;
  struct Item {
    Details details;
    Face face;
    std::string path, error;
    bool done = false;
  };
  std::vector<Item> items;
  ~FontJob() {
    if (directory.empty()) return;
    if (DIR* dir = opendir(directory.c_str())) {
      while (auto* entry = readdir(dir)) {
        if (entry->d_name[0] != '.') unlinkat(dirfd(dir), entry->d_name, 0);
      }
      closedir(dir);
    }
    rmdir(directory.c_str());
  }
};

bool SaveFont(const std::shared_ptr<FontJob>& job, const std::string& bytes,
              size_t index, std::string& path) {
  if (!fontsource::TtfBytes(bytes)) return false;
  path = job->directory + "/preview-" + std::to_string(index) + ".ttf";
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  size_t written = 0;
  while (written < bytes.size() && !job->cancel.load()) {
    const auto count = write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    written += static_cast<size_t>(count);
  }
  const bool ok = close(fd) == 0 && written == bytes.size() && !job->cancel.load();
  if (!ok) { unlink(path.c_str()); path.clear(); }
  return ok;
}

std::shared_ptr<FontJob> LoadCatalog(fontsource::Fetch fetch) {
  auto job = std::make_shared<FontJob>();
  std::thread([job, fetch] {
    std::string bytes, error;
    std::vector<Family> catalog;
    if (fetch("https://api.fontsource.org/v1/fonts", 8 * 1024 * 1024, job->cancel, bytes, error))
      fontsource::ParseCatalog(bytes, catalog, error);
    std::lock_guard<std::mutex> lock(job->mutex);
    job->catalog = std::move(catalog);
    job->error = std::move(error);
    job->done = true;
  }).detach();
  return job;
}

bool ReadSaved(const SavedFont& font, size_t limit, std::atomic<bool>& cancel, std::string& bytes) {
  const int fd = open(font.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat info{};
  bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
      static_cast<uint64_t>(info.st_size) <= limit;
  bytes.clear();
  if (ok) {
    bytes.resize(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset < bytes.size() && !cancel.load()) {
      const auto count = read(fd, bytes.data() + offset, std::min<size_t>(32768, bytes.size() - offset));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      offset += static_cast<size_t>(count);
    }
    ok = offset == bytes.size() && !cancel.load();
  }
  close(fd);
  return ok;
}

std::shared_ptr<FontJob> LoadFaces(fontsource::Fetch fetch, const std::vector<Family>& families,
                                 const std::string& language, const Details* known = nullptr,
                                 const Face* selected = nullptr, const std::vector<SavedFont>& local = {}) {
  auto job = std::make_shared<FontJob>();
  job->items.resize(families.size());
  char directory[] = "/tmp/aera-font-browser-XXXXXX";
  if (!mkdtemp(directory)) {
    job->done = true; job->error = "Font storage is unavailable or full.";
    for (auto& item : job->items) { item.done = true; item.error = job->error; }
    return job;
  }
  job->directory = directory;
  const Details saved = known ? *known : Details{};
  const Face chosen = selected ? *selected : Face{};
  std::thread([job, fetch, families, language, saved, chosen, local] {
    size_t total = 0;
    for (size_t i = 0; i < families.size() && !job->cancel.load(); ++i) {
      FontJob::Item item;
      std::string bytes;
      const auto local_font = std::find_if(local.begin(), local.end(), [&](const SavedFont& font) {
        return font.id == families[i].id;
      });
      if (local_font != local.end()) {
        item.details.family = families[i];
        item.details.license = local_font->license;
        item.face = {local_font->weight, local_font->style, local_font->subset, local_font->url};
        item.details.faces = {item.face};
        const size_t limit = std::min<size_t>(families.size() == 1 ? 32 * 1024 * 1024 : 8 * 1024 * 1024,
                                            32 * 1024 * 1024 - total);
        if (!ReadSaved(*local_font, limit, job->cancel, bytes) || !SaveFont(job, bytes, i, item.path))
          item.error = "AERA could not load this font.";
        else total += bytes.size();
      } else if (saved.family.id == families[i].id) item.details = saved;
      else if (fetch("https://api.fontsource.org/v1/fonts/" + families[i].id,
                     512 * 1024, job->cancel, bytes, item.error))
        fontsource::ParseDetails(bytes, families[i].id, item.details, item.error);
      if (local_font == local.end() && !item.details.faces.empty()) {
        const auto faces = fontsource::FacesForLanguage(item.details, language);
        if (!chosen.url.empty()) item.face = chosen;
        else if (!faces.empty()) item.face = faces.front();
        if (!item.face.url.empty() && total < 32 * 1024 * 1024) {
          // A page owns at most 32 MB of font files; only the detail view may
          // fetch a single large face up to the importer's existing limit.
          const size_t limit = std::min<size_t>(families.size() == 1 ? 32 * 1024 * 1024 : 8 * 1024 * 1024,
                                                32 * 1024 * 1024 - total);
          if (fetch(item.face.url, limit, job->cancel, bytes, item.error)) {
            if (!SaveFont(job, bytes, i, item.path)) item.error = "AERA could not load this font.";
            else total += bytes.size();
          }
        }
        if (item.path.empty() && item.error.empty()) item.error = "Font unavailable";
      }
      item.done = true;
      std::lock_guard<std::mutex> lock(job->mutex);
      job->items[i] = std::move(item);
    }
    std::lock_guard<std::mutex> lock(job->mutex);
    job->done = true;
  }).detach();
  return job;
}

struct PreviewFace {
  lv_font_t* font = nullptr;
  ~PreviewFace() { if (font) lv_freetype_font_delete(font); }
};
struct Browser {
  lv_obj_t *overlay = nullptr, *content = nullptr, *status = nullptr;
  lv_obj_t *search = nullptr, *filter = nullptr, *previous = nullptr, *next = nullptr, *page_label = nullptr;
  lv_obj_t *heading = nullptr, *query_overlay = nullptr, *use = nullptr, *style_label = nullptr;
  lv_obj_t *browse_tab = nullptr, *saved_tab = nullptr, *download = nullptr;
  lv_timer_t* timer = nullptr;
  fontsource::Fetch fetch;
  std::function<void()> applied;
  std::shared_ptr<FontJob> catalog_job, page_job;
  std::vector<Family> catalog;
  std::vector<Family> local_catalog;
  std::vector<SavedFont> saved;
  std::vector<size_t> filtered;
  struct Row { lv_obj_t* sample = nullptr; lv_obj_t* note = nullptr; bool processed = false; };
  std::vector<Row> rows;
  std::vector<std::unique_ptr<PreviewFace>> previews;
  std::string query, sample_text;
  int category = 0, page = 0, page_size = 6;
  int detail = -1, variant = 0;
  Details details;
  std::vector<Face> variants;
  std::string selected_path;
  std::string saved_id;
  bool local = false, appearance_changed = false;
  std::string queries[2];
  int pages[2]{};
  bool render = false, retry = false;
};

void SetText(lv_obj_t* object, const std::string& text) { lv_label_set_text(object, text.c_str()); }
void Hidden(lv_obj_t* object, bool hidden) {
  if (hidden) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}
std::string FaceName(const Face& face) {
  return std::to_string(face.weight) + " / " +
      i18n::Translate(face.style == "italic" ? "Italic" : "Regular");
}
std::string DetailSample(const std::string& subset) {
  return fontsource::Sample(subset) + "\n\nAa Bb Cc\n0123456789\n\n!? @#$% & () [] {}";
}
lv_obj_t* Icon(lv_obj_t* parent, const char* symbol, Handler action) {
  auto* button = Button(parent, symbol, std::move(action));
  lv_obj_set_size(button, 112, 112);
  return button;
}
void ResetPage(Browser* state) {
  lv_obj_clean(state->content);
  state->rows.clear();
  state->previews.clear();
  if (state->page_job) state->page_job->cancel.store(true);
  state->page_job.reset();
  state->selected_path.clear();
  state->use = nullptr;
  state->download = nullptr;
}

void RefreshSaved(Browser* state) {
  state->saved = RecoverySavedUiFonts();
  state->local_catalog.clear();
  for (const auto& font : state->saved)
    state->local_catalog.push_back({font.id, font.family, font.category, {font.subset}});
}

std::string MatchSaved(Browser* state) {
  if (state->local && state->detail >= 0 && state->detail < static_cast<int>(state->saved.size()))
    return state->saved[state->detail].id;
  if (state->detail < 0 || state->variants.empty()) return {};
  const auto& face = state->variants[state->variant];
  for (const auto& saved : state->saved)
    if (saved.provider_id == state->details.family.id && saved.weight == face.weight &&
        saved.style == face.style && saved.subset == face.subset && saved.url == face.url) return saved.id;
  return {};
}

void UpdateFontActions(Browser* state) {
  state->saved_id = MatchSaved(state);
  if (state->download && !state->local) {
    SetText(lv_obj_get_child(state->download, 0), i18n::Translate(state->saved_id.empty() ? "Download" : "Downloaded"));
    if (state->saved_id.empty()) lv_obj_remove_state(state->download, LV_STATE_DISABLED);
    else lv_obj_add_state(state->download, LV_STATE_DISABLED);
  }
  if (state->use) {
    const bool active = !state->saved_id.empty() && RecoveryUiFont() == state->saved_id;
    SetText(lv_obj_get_child(state->use, 0), i18n::Translate(active ? "Active" : "Use this font"));
    if (active || state->selected_path.empty()) lv_obj_add_state(state->use, LV_STATE_DISABLED);
    else lv_obj_remove_state(state->use, LV_STATE_DISABLED);
  }
}

bool SaveSelected(Browser* state) {
  UpdateFontActions(state);
  if (!state->saved_id.empty()) return true;
  if (state->selected_path.empty() || state->variants.empty()) return false;
  const auto& face = state->variants[state->variant];
  SavedFont metadata;
  metadata.family = state->details.family.name; metadata.provider_id = state->details.family.id;
  metadata.category = state->details.family.category; metadata.license = state->details.license;
  metadata.subset = face.subset; metadata.style = face.style; metadata.weight = face.weight; metadata.url = face.url;
  std::string id, error;
  if (!RecoverySaveUiFont(state->selected_path, metadata, &id, &error)) {
    SetText(state->status, i18n::Translate(error.c_str())); return false;
  }
  RefreshSaved(state);
  UpdateFontActions(state);
  return true;
}

void SwitchTab(Browser* state, bool local) {
  if (state->local == local) return;
  state->queries[state->local] = state->query;
  state->pages[state->local] = state->page;
  state->local = local; state->detail = -1;
  state->query = state->queries[local]; state->page = state->pages[local];
  RefreshSaved(state);
  state->render = true;
  if (!local && state->catalog.empty() && !state->catalog_job) state->retry = true;
}

void Choices(Browser* state, const char* title, const std::vector<std::string>& names,
             int active, std::function<void(int)> choose) {
  auto* overlay = lv_obj_create(state->overlay);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  state->query_overlay = overlay;
  auto* heading = Label(overlay, title, &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 64, 32);
  auto* close = Icon(overlay, LV_SYMBOL_CLOSE, [state, overlay] {
    state->query_overlay = nullptr; lv_obj_delete_async(overlay);
  });
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -48, 12);
  auto* list = lv_obj_create(overlay);
  Clear(list);
  lv_obj_set_pos(list, 48, 162);
  lv_obj_set_size(list, lv_obj_get_width(state->overlay) - 96, lv_obj_get_height(state->overlay) - 194);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 16, 0);
  for (size_t i = 0; i < names.size(); ++i) {
    auto* button = Button(list, names[i].c_str(), [state, overlay, choose, i] {
      state->query_overlay = nullptr; lv_obj_delete_async(overlay);
      choose(static_cast<int>(i));
    }, static_cast<int>(i) == active);
    lv_obj_set_size(button, LV_PCT(100), 136);
  }
}

void Search(Browser* state, bool sample = false) {
  auto* overlay = lv_obj_create(state->overlay);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  state->query_overlay = overlay;
  const int width = lv_obj_get_width(state->overlay);
  const int height = lv_obj_get_height(state->overlay);
  auto* heading = Label(overlay, sample ? "Preview text" : "Search fonts", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 64, 36);
  SingleLineLabel(heading, width - 128, &lv_font_montserrat_48);
  auto* input = TextArea(overlay);
  lv_obj_set_pos(input, 64, 150);
  lv_obj_set_size(input, width - 128, 160);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_max_length(input, sample ? 120 : 80);
  lv_textarea_set_text(input, (sample ? state->sample_text : state->query).c_str());
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_40), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_bg_color(input, kMainPanel, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(input, 28, 0);
  const int button_width = (width - 152) / 2;
  auto* cancel = Button(overlay, "Cancel", [state, overlay] {
    state->query_overlay = nullptr; lv_obj_delete_async(overlay);
  });
  lv_obj_set_pos(cancel, 64, 350); lv_obj_set_size(cancel, button_width, 120);
  auto* accept = Button(overlay, sample ? "Apply" : "Search", [state, overlay, input, sample] {
    if (sample) {
      state->sample_text = lv_textarea_get_text(input);
      if (!state->rows.empty() && state->rows.front().sample)
        SetText(state->rows.front().sample, state->sample_text.empty()
            ? DetailSample(state->variants.empty() ? "latin" : state->variants[state->variant].subset)
            : state->sample_text);
    } else {
      state->query = lv_textarea_get_text(input);
      state->page = 0; state->render = true;
    }
    state->query_overlay = nullptr; lv_obj_delete_async(overlay);
  }, true);
  lv_obj_set_pos(accept, 88 + button_width, 350); lv_obj_set_size(accept, button_width, 120);
  auto* keyboard = lv_keyboard_create(overlay);
  phone_keyboard::Apply(keyboard);
  const int keyboard_height = std::min(900, height - 540);
  lv_obj_set_size(keyboard, width - 96, keyboard_height);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -32);
  lv_keyboard_set_textarea(keyboard, input);
  lv_obj_add_event_cb(keyboard, [](lv_event_t* event) {
    lv_obj_send_event(static_cast<lv_obj_t*>(lv_event_get_user_data(event)), LV_EVENT_CLICKED, nullptr);
  }, LV_EVENT_READY, accept);
  lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
}

void Render(Browser* state) {
  state->render = false;
  ResetPage(state);
  const int width = lv_obj_get_width(state->content);
  const bool detail = state->detail >= 0;
  const int content_top = detail ? 232 : 528;
  lv_obj_set_y(state->content, content_top);
  lv_obj_set_height(state->content, lv_obj_get_height(state->overlay) - content_top - (detail ? 48 : 208));
  lv_obj_update_layout(state->content);
  const int content_height = lv_obj_get_height(state->content);
  Hidden(state->search, detail);
  Hidden(state->filter, detail);
  Hidden(state->previous, detail);
  Hidden(state->next, detail);
  Hidden(state->page_label, detail);
  Hidden(state->browse_tab, detail);
  Hidden(state->saved_tab, detail);
  lv_obj_set_style_bg_color(state->browse_tab, state->local ? kMainPanel : kAccentSoft, 0);
  lv_obj_set_style_bg_color(state->saved_tab, state->local ? kAccentSoft : kMainPanel, 0);
  const auto& catalog = state->local ? state->local_catalog : state->catalog;
  if (detail) {
    const auto family = catalog[state->detail];
    SetText(state->heading, family.name);
    auto* face = Button(state->content,
        state->variants.empty() ? "Loading" : FaceName(state->variants[state->variant]).c_str(), [state] {
      if (state->variants.size() < 2) return;
      std::vector<std::string> names;
      for (const auto& item : state->variants) names.push_back(FaceName(item));
      Choices(state, "Font style", names, state->variant, [state](int selected) {
        state->variant = selected; state->render = true;
      });
    });
    if (state->local) lv_obj_add_state(face, LV_STATE_DISABLED);
    lv_obj_set_pos(face, 0, 0); lv_obj_set_size(face, width - 144, 120);
    state->style_label = lv_obj_get_child(face, 0);
    auto* edit = Icon(state->content, LV_SYMBOL_EDIT, [state] { Search(state, true); });
    lv_obj_set_pos(edit, width - 112, 4);
    auto* sample_area = lv_obj_create(state->content);
    Clear(sample_area);
    lv_obj_set_pos(sample_area, 32, 200);
    lv_obj_set_size(sample_area, width - 64, std::max(160, content_height - 450));
    lv_obj_add_flag(sample_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sample_area, LV_DIR_VER);
    auto* sample = lv_label_create(sample_area);
    lv_obj_set_width(sample, LV_PCT(100));
    lv_label_set_long_mode(sample, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(sample, kText, 0);
    lv_obj_set_style_text_font(sample, UiFont(&lv_font_montserrat_48), 0);
    lv_label_set_text(sample, "");
    auto* note = Label(state->content, "Loading preview", &lv_font_montserrat_28, kMuted);
    lv_obj_set_pos(note, 32, content_height - 200);
    SingleLineLabel(note, width - 64, &lv_font_montserrat_28);
    state->rows.push_back({sample, note, false});
    state->use = Button(state->content, "Use this font", [state] {
      if (state->selected_path.empty()) return;
      if (!SaveSelected(state)) return;
      std::string error;
      if (!RecoveryUseSavedUiFont(state->saved_id, &error)) {
        SetText(state->status, i18n::Translate(error.c_str())); return;
      }
      lv_obj_report_style_change(nullptr);
      auto callback = state->applied;
      lv_obj_delete_async(state->overlay);
      if (callback) callback();
    }, true);
    const int action_width = (width - 24) / 2;
    lv_obj_set_size(state->use, action_width, 120);
    lv_obj_align(state->use, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_state(state->use, LV_STATE_DISABLED);
    state->download = Button(state->content, state->local ? LV_SYMBOL_TRASH : "Download", [state] {
      if (!state->local) { SaveSelected(state); return; }
      const auto id = MatchSaved(state);
      const bool active = RecoveryUiFont() == id;
      Sheet(lv_obj_get_screen(state->overlay), "Delete font",
          active ? "This font is active. Switch to the default font and delete it?" :
                   "Delete this font from My fonts?", [state, id, active] {
        std::string error;
        if (!RecoveryDeleteSavedUiFont(id, active, &error)) {
          SetText(state->status, i18n::Translate(error.c_str())); return;
        }
        if (active) { state->appearance_changed = true; lv_obj_report_style_change(nullptr); }
        RefreshSaved(state); state->detail = -1; state->render = true;
      });
    });
    lv_obj_set_size(state->download, action_width, 120);
    lv_obj_align(state->download, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_state(state->download, LV_STATE_DISABLED);
    const bool known = state->details.family.id == family.id && !state->variants.empty();
    const std::vector<SavedFont> local = state->local
        ? std::vector<SavedFont>{state->saved[state->detail]} : std::vector<SavedFont>{};
    state->page_job = LoadFaces(state->fetch, {family}, RecoveryLanguage(),
        known ? &state->details : nullptr, known ? &state->variants[state->variant] : nullptr, local);
    SetText(state->status, state->local ? i18n::Translate("My fonts") : "Fontsource");
    return;
  }
  SetText(state->heading, i18n::Translate(state->local ? "My fonts" : "Browse fonts"));
  auto* search_label = lv_obj_get_child(state->search, 0);
  SetText(search_label, state->query.empty() ? i18n::Translate("Search fonts") : state->query);
  FitButtonLabel(state->search);
  SetText(lv_obj_get_child(state->filter, 0), i18n::Translate(category_names[state->category]));
  FitButtonLabel(state->filter);
  state->filtered = fontsource::Filter(catalog, state->query, categories[state->category]);
  state->page_size = std::clamp(content_height / 260, 2, 8);
  const int pages = std::max(1, (static_cast<int>(state->filtered.size()) + state->page_size - 1) / state->page_size);
  state->page = std::clamp(state->page, 0, pages - 1);
  SetText(state->page_label, std::to_string(state->page + 1) + " / " + std::to_string(pages));
  if (state->page == 0) lv_obj_add_state(state->previous, LV_STATE_DISABLED);
  else lv_obj_remove_state(state->previous, LV_STATE_DISABLED);
  if (state->page + 1 >= pages) lv_obj_add_state(state->next, LV_STATE_DISABLED);
  else lv_obj_remove_state(state->next, LV_STATE_DISABLED);
  SetText(state->status, state->filtered.empty() ? i18n::Translate("No results") :
      std::to_string(state->filtered.size()) + " / " + (state->local ? i18n::Translate("My fonts") : "Fontsource"));
  if (state->local && state->saved.empty()) {
    auto* empty = Label(state->content, RecoveryDataLocked() ? "Unlock storage first" :
        "No saved fonts yet.", &lv_font_montserrat_36, kMuted);
    lv_obj_set_width(empty, width - 64); lv_obj_set_pos(empty, 32, 40);
  } else if (!state->local && state->catalog.empty()) {
    const char* message = state->catalog_job ? "Loading fonts" :
        "Connect to Wi-Fi or mobile data to browse fonts.";
    SetText(state->status, i18n::Translate(message));
  }
  std::vector<Family> visible;
  const int start = state->page * state->page_size;
  for (int i = start; i < std::min(start + state->page_size, static_cast<int>(state->filtered.size())); ++i) {
    const int index = static_cast<int>(state->filtered[i]);
    const auto& family = catalog[index];
    visible.push_back(family);
    auto* row = lv_button_create(state->content);
    Clear(row);
    lv_obj_set_style_bg_color(row, kMainPanel, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, kMainSelected, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 32, 0);
    OnClick(row, [state, index] {
      state->detail = index; state->variant = 0; state->details = {}; state->variants.clear();
      state->sample_text.clear(); state->render = true;
    });
    lv_obj_set_pos(row, 0, (i - start) * 260);
    lv_obj_set_size(row, width, 240);
    auto* name = Label(row, family.name.c_str(), &lv_font_montserrat_36, kText);
    lv_obj_set_pos(name, 32, 24); SingleLineLabel(name, width - 128, &lv_font_montserrat_36);
    auto* arrow = Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kMuted);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -32, 0);
    auto* sample = lv_label_create(row);
    lv_obj_set_pos(sample, 32, 96); lv_obj_set_size(sample, width - 128, 70);
    lv_label_set_long_mode(sample, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(sample, kText, 0);
    lv_obj_set_style_text_font(sample, UiFont(&lv_font_montserrat_48), 0);
    lv_label_set_text(sample, "");
    auto* note = Label(row, "Loading preview", &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(note, 32, 184); SingleLineLabel(note, width - 128, &lv_font_montserrat_24);
    state->rows.push_back({sample, note, false});
  }
  if (!visible.empty()) state->page_job = LoadFaces(state->fetch, visible, RecoveryLanguage(),
      nullptr, nullptr, state->local ? state->saved : std::vector<SavedFont>{});
}

void Tick(Browser* state) {
  if (state->retry) {
    state->retry = false;
    if (state->local) { RefreshSaved(state); state->render = true; }
    else if (RecoveryNetworkConnected()) {
      if (state->catalog_job) state->catalog_job->cancel.store(true);
      state->catalog_job = LoadCatalog(state->fetch);
      SetText(state->status, i18n::Translate("Loading fonts"));
    } else {
      SetText(state->status, i18n::Translate("Connect to Wi-Fi or mobile data to browse fonts."));
    }
  }
  if (state->catalog_job) {
    bool done;
    { std::lock_guard<std::mutex> lock(state->catalog_job->mutex); done = state->catalog_job->done; }
    if (done) {
      if (!state->catalog_job->catalog.empty()) state->catalog = std::move(state->catalog_job->catalog);
      const auto error = state->catalog_job->error;
      state->catalog_job.reset();
      if (!state->local) state->render = true;
      if (!state->local && state->catalog.empty()) {
        ResetPage(state);
        SetText(state->status, i18n::Translate(error.empty() ? "Download failed" : error.c_str()));
        state->render = false;
      }
    }
  }
  if (state->render) Render(state);
  if (!state->page_job) return;
  std::vector<FontJob::Item> items;
  std::string job_error;
  { std::lock_guard<std::mutex> lock(state->page_job->mutex);
    items = state->page_job->items; job_error = state->page_job->error; }
  if (!job_error.empty()) SetText(state->status, i18n::Translate(job_error.c_str()));
  for (size_t i = 0; i < std::min(items.size(), state->rows.size()); ++i) {
    auto& row = state->rows[i];
    if (row.processed || !items[i].done) continue;
    row.processed = true;
    const auto& item = items[i];
    if (item.path.empty()) {
      SetText(row.note, i18n::Translate(item.error.empty() ? "Preview unavailable" : item.error.c_str()));
      continue;
    }
    auto preview = std::make_unique<PreviewFace>();
    preview->font = lv_freetype_font_create(item.path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
        state->detail >= 0 ? 96 : 48, LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!preview->font) { SetText(row.note, i18n::Translate("Preview unavailable")); continue; }
    // Preview faces are owned by this page, never inserted into the permanent
    // UI-font cache. Delete their labels before releasing the face and file.
    preview->font->fallback = UiFont(&lv_font_montserrat_48);
    lv_obj_set_style_text_font(row.sample, preview->font, 0);
    SetText(row.sample, state->detail >= 0
        ? (state->sample_text.empty() ? DetailSample(item.face.subset) : state->sample_text)
        : fontsource::Sample(item.face.subset));
    std::string saved_id;
    for (const auto& font : state->saved) {
      const bool same = state->local ? font.id == item.details.family.id :
          font.provider_id == item.details.family.id && font.url == item.face.url;
      if (same) { saved_id = font.id; break; }
    }
    const std::string badge = saved_id.empty() ? "" :
        " / " + std::string(i18n::Translate(RecoveryUiFont() == saved_id ? "Active" : "Downloaded"));
    SetText(row.note, item.face.subset + " / " + FaceName(item.face) + badge +
        (item.details.license.empty() ? "" : " / " + item.details.license));
    state->previews.push_back(std::move(preview));
    if (state->detail >= 0) {
      state->details = item.details;
      state->variants = fontsource::FacesForLanguage(state->details, RecoveryLanguage());
      state->selected_path = item.path;
      SetText(state->style_label, FaceName(item.face));
      if (state->download) lv_obj_remove_state(state->download, LV_STATE_DISABLED);
      UpdateFontActions(state);
    }
  }
}
}  // namespace

void OpenFontBrowser(lv_obj_t* screen, std::function<void()> applied, fontsource::Fetch fetch) {
  auto* state = new Browser;
  state->fetch = std::move(fetch); state->applied = std::move(applied);
  state->local = !RecoveryNetworkConnected();
  RefreshSaved(state);
  const int width = lv_obj_get_width(screen), height = lv_obj_get_height(screen);
  const int top = StatusBarHeight();
  state->overlay = lv_obj_create(screen);
  Clear(state->overlay);
  lv_obj_set_user_data(state->overlay, &kPersistentModalMarker);
  lv_obj_set_pos(state->overlay, 0, top);
  lv_obj_set_size(state->overlay, width, height - top);
  lv_obj_set_style_bg_color(state->overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(state->overlay, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(state->overlay, [](lv_event_t* event) {
    auto* state = static_cast<Browser*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_CANCEL) {
      if (state->query_overlay) {
        lv_obj_delete_async(state->query_overlay); state->query_overlay = nullptr;
      } else if (state->detail >= 0) { state->detail = -1; state->render = true; }
      else {
        auto callback = state->appearance_changed ? state->applied : std::function<void()>{};
        lv_obj_delete_async(state->overlay);
        if (callback) callback();
      }
    } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
      if (state->timer) lv_timer_delete(state->timer);
      if (state->catalog_job) state->catalog_job->cancel.store(true);
      if (state->page_job) state->page_job->cancel.store(true);
      // LV_EVENT_DELETE precedes child deletion: remove preview labels first.
      lv_obj_clean(state->content);
      state->previews.clear();
      delete state;
    }
  }, LV_EVENT_ALL, state);
  auto* back = Icon(state->overlay, LV_SYMBOL_LEFT, [state] {
    lv_obj_send_event(state->overlay, LV_EVENT_CANCEL, nullptr);
  });
  lv_obj_set_pos(back, 48, 16);
  state->heading = Label(state->overlay, "Browse fonts", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(state->heading, 192, 44);
  SingleLineLabel(state->heading, width - 420, &lv_font_montserrat_48);
  auto* refresh = Icon(state->overlay, LV_SYMBOL_REFRESH, [state] {
    if (state->detail >= 0) state->render = true;
    else state->retry = true;
  });
  lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -48, 16);
  state->status = Label(state->overlay, "Loading fonts", &lv_font_montserrat_28, kMuted);
  lv_obj_set_pos(state->status, 64, 158);
  SingleLineLabel(state->status, width - 128, &lv_font_montserrat_28);
  const int tab_width = (width - 120) / 2;
  state->browse_tab = Button(state->overlay, "Browse fonts", [state] { SwitchTab(state, false); });
  lv_obj_set_pos(state->browse_tab, 48, 232); lv_obj_set_size(state->browse_tab, tab_width, 112);
  state->saved_tab = Button(state->overlay, "My fonts", [state] { SwitchTab(state, true); });
  lv_obj_set_pos(state->saved_tab, 72 + tab_width, 232); lv_obj_set_size(state->saved_tab, tab_width, 112);
  state->search = Button(state->overlay, "Search fonts", [state] { Search(state); });
  lv_obj_set_pos(state->search, 48, 368); lv_obj_set_size(state->search, (width - 120) * 2 / 3, 120);
  state->filter = Button(state->overlay, "All fonts", [state] {
    std::vector<std::string> names;
    for (const auto* name : category_names) names.push_back(i18n::Translate(name));
    Choices(state, "Category", names, state->category, [state](int category) {
      state->category = category; state->page = 0; state->render = true;
    });
  });
  lv_obj_set_pos(state->filter, 72 + (width - 120) * 2 / 3, 368);
  lv_obj_set_size(state->filter, (width - 120) / 3, 120);
  state->content = lv_obj_create(state->overlay);
  Clear(state->content);
  lv_obj_set_pos(state->content, 48, 528);
  lv_obj_set_size(state->content, width - 96, height - top - 736);
  state->previous = Icon(state->overlay, LV_SYMBOL_LEFT, [state] { --state->page; state->render = true; });
  lv_obj_align(state->previous, LV_ALIGN_BOTTOM_LEFT, 48, -32);
  state->next = Icon(state->overlay, LV_SYMBOL_RIGHT, [state] { ++state->page; state->render = true; });
  lv_obj_align(state->next, LV_ALIGN_BOTTOM_RIGHT, -48, -32);
  state->page_label = Label(state->overlay, "", &lv_font_montserrat_32, kMuted);
  lv_obj_align(state->page_label, LV_ALIGN_BOTTOM_MID, 0, -64);
  state->retry = !state->local;
  state->render = true;
  state->timer = lv_timer_create([](lv_timer_t* timer) {
    Tick(static_cast<Browser*>(lv_timer_get_user_data(timer)));
  }, 100, state);
}
}  // namespace aeraui
