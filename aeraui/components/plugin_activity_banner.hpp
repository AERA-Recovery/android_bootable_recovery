/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "ui_components.hpp"
#include "package_review.hpp"
#include "plugin_api/activity.hpp"
#include "plugin_api/runtime.hpp"

namespace aeraui::widgets {
class PluginActivityBanner {
 public:
  void Clear() {
    Close();
    if (badge_) lv_obj_delete(badge_);
    badge_ = label_ = nullptr;
  }
  bool Expanded() const { return sheet_ != nullptr; }
  void Close() {
    if (sheet_) lv_obj_delete(sheet_);
    sheet_ = list_ = nullptr; rows_.clear(); keys_.clear();
  }
  void Refresh(bool visible, ActionCallback callback, void *context, int offset = 0) {
    callback_ = callback; context_ = context;
    const auto activities = plugin_api::Activities();
    if (activities.empty()) { Clear(); return; }
    if (!visible) Close();
    if (!badge_) {
      badge_ = Button(lv_layer_top(), "", [this] { Open(); });
      lv_obj_set_size(badge_, 224, 68);
      lv_obj_set_style_bg_opa(badge_, LV_OPA_90, 0);
      label_ = lv_obj_get_child(badge_, 0);
    }
    lv_obj_align(badge_, LV_ALIGN_TOP_RIGHT, -32, StatusBarHeight() + 10 + offset);
    lv_obj_set_flag(badge_, LV_OBJ_FLAG_HIDDEN, !visible || Expanded());
    const size_t active = std::count_if(activities.begin(), activities.end(),
        [](const auto &activity) { return activity.Active(); });
    const auto current = std::find_if(activities.begin(), activities.end(),
        [](const auto &activity) { return activity.Active(); });
    const auto &first = current == activities.end() ? activities.front() : *current;
    std::string text = LV_SYMBOL_DOWNLOAD;
    if (active > 1) text += "  " + std::to_string(active);
    else if (first.progress <= 10000) text += "  " + std::to_string(first.progress / 100) + "%";
    else text += "  ...";
    SetText(label_, text);
    if (!sheet_) return;
    std::vector<std::string> keys;
    for (const auto &activity : activities) keys.push_back(activity.plugin + ":" + std::to_string(activity.id) +
        ((activity.flags & plugin_api::kActivityComplete) && !activity.result_file.empty() ? ":ready" : ""));
    if (keys != keys_) BuildRows(activities, keys);
    for (size_t i = 0; i < rows_.size(); ++i) {
      auto &row = rows_[i]; const auto &activity = activities[i];
      SetText(row.title, activity.name + " / " + activity.title);
      SetText(row.detail, activity.detail);
      lv_bar_set_value(row.bar, activity.progress <= 10000 ? activity.progress : 0, LV_ANIM_OFF);
      const auto color = activity.flags & plugin_api::kActivityFailed ? design::kRed : design::kAccent;
      if (!lv_color_eq(lv_obj_get_style_bg_color(row.bar, LV_PART_INDICATOR), color))
        lv_obj_set_style_bg_color(row.bar, color, LV_PART_INDICATOR);
      const bool disabled = activity.Active() ? !(activity.flags & plugin_api::kActivityCancellable) : activity.result_file.empty();
      if (lv_obj_has_state(row.cancel, LV_STATE_DISABLED) != disabled)
        lv_obj_set_state(row.cancel, LV_STATE_DISABLED, disabled);
      SetText(lv_obj_get_child(row.cancel, 0), activity.cancelling ? LV_SYMBOL_STOP : LV_SYMBOL_CLOSE);
      row.cancelling = activity.cancelling;
    }
  }
  void Open() {
    if (sheet_) return;
    sheet_ = lv_obj_create(lv_layer_top());
    ClearObject(sheet_);
    lv_obj_set_pos(sheet_, 0, StatusBarHeight());
    lv_obj_set_size(sheet_, LV_PCT(100), lv_obj_get_height(lv_layer_top()) - StatusBarHeight());
    lv_obj_set_style_bg_color(sheet_, design::kMainCanvas, 0);
    lv_obj_set_style_bg_opa(sheet_, LV_OPA_COVER, 0);
    lv_obj_update_layout(sheet_);
    const int width = lv_obj_get_width(sheet_), top = 36;
    auto *title = Label(sheet_, "Background tasks", &lv_font_montserrat_40, design::kText);
    lv_obj_set_pos(title, 60, top); SingleLineLabel(title, width - 260, &lv_font_montserrat_40);
    auto *close = Button(sheet_, LV_SYMBOL_CLOSE, [this] { Close(); });
    lv_obj_set_pos(close, width - 160, top - 10); lv_obj_set_size(close, 100, 90);
    list_ = lv_obj_create(sheet_); ClearObject(list_);
    lv_obj_set_pos(list_, 40, top + 130);
    lv_obj_set_size(list_, width - 80, std::max(160, lv_obj_get_height(sheet_) - top - 180));
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    Refresh(true, callback_, context_);
  }
 private:
  struct Row { lv_obj_t *title, *detail, *bar, *cancel; bool cancelling = false; };
  static void SetText(lv_obj_t *label, const std::string &text) {
    if (text != lv_label_get_text(label)) lv_label_set_text(label, text.c_str());
  }
  static void ClearObject(lv_obj_t *object) {
    widgets::Clear(object); lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  }
  void BuildRows(const std::vector<plugin_api::Activity> &activities,
                 const std::vector<std::string> &keys) {
    lv_obj_clean(list_); rows_.clear(); keys_ = keys;
    lv_obj_update_layout(list_);
    const int width = lv_obj_get_width(list_) - 40;
    int y = 0;
    for (size_t i = 0; i < activities.size(); ++i) {
      const auto id = activities[i].plugin;
      const bool ready = (activities[i].flags & plugin_api::kActivityComplete) && !activities[i].result_file.empty();
      auto *item = lv_obj_create(list_); design::Panel(item, 24, design::kMainSheet);
      lv_obj_set_pos(item, 20, y); lv_obj_set_size(item, width, ready ? 310 : 210);
      y += ready ? 330 : 230;
      auto *title = Label(item, "", &lv_font_montserrat_32, design::kText);
      lv_obj_set_pos(title, 24, 24); SingleLineLabel(title, width - 390, &lv_font_montserrat_32);
      auto *detail = Label(item, "", &lv_font_montserrat_28, design::kMutedStrong);
      lv_obj_set_pos(detail, 24, 88); SingleLineLabel(detail, width - 390, &lv_font_montserrat_28);
      auto *bar = lv_bar_create(item); lv_obj_set_pos(bar, 24, 162);
      lv_obj_set_size(bar, width - 48, 10); lv_bar_set_range(bar, 0, 10000);
      lv_obj_set_style_bg_color(bar, design::kMainLine, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
      auto *open = Button(item, LV_SYMBOL_RIGHT "  Open", [this, id] {
        const auto selected = id;
        const auto callback = callback_; auto *context = context_;
        Close(); SetSelectedPluginId(selected);
        if (callback) callback(Action::kPluginApp, context);
      });
      lv_obj_set_pos(open, width - 340, 40); lv_obj_set_size(open, 196, 96);
      auto *cancel = Button(item, LV_SYMBOL_CLOSE, [this, id] {
        bool force = false;
        for (const auto &activity : plugin_api::Activities()) {
          if (activity.plugin != id) continue;
          if (!activity.Active()) { plugin_api::RemoveActivity(id); return; }
          force = activity.cancelling;
        }
        Sheet(sheet_, force ? "Force stop plugin?" : "Cancel task?",
            force ? "The plugin will be terminated. Files or operations may be incomplete."
                  : "Cancel this task? Work already completed will not be undone.",
            [id, force] { if (force) StopGenericPluginRuntime(id); else CancelGenericPluginActivity(id); });
      });
      lv_obj_set_pos(cancel, width - 126, 40); lv_obj_set_size(cancel, 96, 96);
      if (ready) {
        auto *install = Button(item, "Install ZIP", [this, path = activities[i].result_file] {
          const auto selected = path;
          const auto callback = callback_; auto *context = context_;
          if (!callback || plugin_api::PluginStorageBusy() || !plugin_api::IsReviewablePackage(selected)) return;
          Close();
          ReviewPackage(lv_screen_active(), selected, callback, context);
        });
        lv_obj_set_pos(install, 24, 204); lv_obj_set_size(install, 300, 80);
      }
      rows_.push_back({title, detail, bar, cancel, false});
    }
  }
  lv_obj_t *badge_ = nullptr, *label_ = nullptr, *sheet_ = nullptr, *list_ = nullptr;
  std::vector<Row> rows_; std::vector<std::string> keys_;
  ActionCallback callback_ = nullptr; void *context_ = nullptr;
};
}  // namespace aeraui::widgets
