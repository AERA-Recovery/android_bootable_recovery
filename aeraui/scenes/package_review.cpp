/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "package_review.hpp"
#include "ui_components.hpp"
#include "update/payload_inspector.hpp"
#include <atomic>
#include <memory>
#include <thread>
#include <cstdio>

namespace aeraui {
namespace {
using namespace widgets;
std::string Size(uint64_t bytes) {
  char value[48];
  if (bytes >= (1ULL << 30)) snprintf(value, sizeof(value), "%.2f GiB", bytes / double(1ULL << 30));
  else snprintf(value, sizeof(value), "%.1f MiB", bytes / double(1ULL << 20));
  return value;
}
void Layout(lv_obj_t *area) {
  lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(area, 18, 0);
  lv_obj_set_style_pad_right(area, 16, 0);
  auto *sheet = lv_obj_get_parent(area);
  lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
  lv_obj_set_style_blur_backdrop(sheet, false, 0);
}
void Row(lv_obj_t *parent, const char *key, const std::string &value) {
  if (value.empty()) return;
  auto *row = lv_obj_create(parent);
  Clear(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(row, 68, 0);
  lv_obj_set_style_pad_ver(row, 14, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  auto *name = Label(row, key, &lv_font_montserrat_28, kMuted);
  lv_obj_set_width(name, LV_PCT(32));
  auto *text = Label(row, value.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_width(text, LV_PCT(65));
  lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
}
void Section(lv_obj_t *parent, const char *text) {
  auto *label = Label(parent, text, &lv_font_montserrat_28, kAccent);
  lv_obj_set_style_pad_top(label, 24, 0);
}
void FullInfo(lv_obj_t *screen, const payload::Info &info, const std::string &path) {
  Sheet(screen, "Update information", "", {}, 2000, false,
        SheetPresentation::kStandard, "Swipe to confirm", [info, path](lv_obj_t *area) {
    Layout(area);
    Section(area, "PACKAGE");
    Row(area, info.name_from_filename ? "Package name" : "Version", info.target_build);
    Row(area, "Product", info.target_device);
    Row(area, "Device codename", info.device_codename);
    Row(area, "Build ID", info.build_id);
    Row(area, "System fingerprint", info.system_fingerprint);
    Row(area, "Android", info.target_sdk);
    Row(area, "Security patch", info.security_patch);
    Row(area, "File", path);
    Section(area, "PAYLOAD");
    Row(area, "Update type", info.incremental ? "Incremental" : "Full");
    Row(area, "Partition coverage", info.partial ? "Partial update" : "Full update");
    Row(area, "Payload size", Size(info.payload_bytes));
    Row(area, "Expanded images", Size(info.expanded_bytes));
    Row(area, "Format revision", std::to_string(info.format_version) + "." + std::to_string(info.minor_version));
    Row(area, "Dynamic partitions", info.dynamic_partitions ? "Yes" : "No");
    Row(area, "Snapshots", info.snapshots ? "Requested by package" : "Not requested");
    Row(area, "Virtual A/B compression", info.virtual_ab_compression ? "Requested by package" : "Not requested");
    Row(area, "Operations", std::to_string(info.operations));
    Row(area, "Operation types", info.operation_types);
    Section(area, "PARTITIONS");
    for (const auto &partition : info.partitions)
      Row(area, partition.name.c_str(), Size(partition.bytes));
  });
}
struct Result {
  payload::Info info;
  std::atomic<bool> done{false};
};
struct Review {
  lv_obj_t *screen;
  lv_obj_t *overlay;
  lv_timer_t *timer = nullptr;
  std::string path;
  ActionCallback callback;
  void *context;
  std::shared_ptr<Result> result = std::make_shared<Result>();
};
void Ready(lv_timer_t *timer) {
  auto *state = static_cast<Review *>(lv_timer_get_user_data(timer));
  if (!state->result->done.load(std::memory_order_acquire)) return;
  const auto info = state->result->info;
  auto *screen = state->screen;
  const auto path = state->path;
  const auto callback = state->callback;
  auto *context = state->context;
  lv_obj_delete(state->overlay);
  if (!info.error.empty()) {
    widgets::Sheet(screen, "Cannot inspect package", info.error);
    return;
  }
  const std::string detail = path + "\n\n" + payload::Summary(info);
  JobRequest request;
  request.job = Job::kInstall;
  request.path = path;
  request.title = "Install ZIP";
  Handler install = [request, callback, context] {
        SetJobRequest(request);
        callback(Action::kRunOperation, context);
      };
  if (!info.is_payload) {
    Sheet(screen, "Install this package?", detail, install);
    return;
  }
  Sheet(screen, "Review Android update", "", install, 1500, false,
        SheetPresentation::kStandard, "Swipe to install", [info, path, screen](lv_obj_t *area) {
    Layout(area);
    if (info.name_from_filename) Section(area, "PACKAGE NAME");
    auto *version = Label(area, info.target_build.empty() ? "Android update" : info.target_build.c_str(),
                          &lv_font_montserrat_40, kText);
    lv_obj_set_width(version, LV_PCT(100));
    Row(area, "Device", info.target_device);
    Row(area, "Android", info.target_sdk);
    Row(area, "Security patch", info.security_patch);
    Row(area, "Update type", info.incremental ? "Incremental OTA" : "Full OTA");
    Row(area, "Payload size", Size(info.payload_bytes));
    auto *details = Button(area, "Full information  " LV_SYMBOL_RIGHT,
        [info, path, screen] { FullInfo(screen, info, path); });
    lv_obj_set_size(details, LV_PCT(100), 96);
    lv_obj_set_style_radius(details, 24, 0);
    lv_obj_set_style_border_width(details, 1, 0);
    lv_obj_set_style_border_color(details, kMainLine, 0);
  });
}
}
void ReviewPackage(lv_obj_t *screen, const std::string &path,
                   ActionCallback callback, void *context) {
  using namespace widgets;
  auto *overlay = lv_obj_create(screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
  auto *label = Label(overlay, "Reading package information...",
                      &lv_font_montserrat_32, kText);
  lv_obj_center(label);
  auto *cancel = Button(overlay, "Cancel", [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -100);
  auto *state = new Review{screen, overlay, nullptr, path, callback, context};
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    auto *state = static_cast<Review *>(lv_event_get_user_data(event));
    if (state->timer) lv_timer_delete(state->timer);
    delete state;
  }, LV_EVENT_DELETE, state);
  // Worker owns only its result; closing the page never leaves a UI pointer
  // accessible to the background reader and never waits for storage I/O.
  std::thread([result = state->result, path] {
    result->info = payload::InspectZip(path);
    result->done.store(true, std::memory_order_release);
  }).detach();
  state->timer = lv_timer_create(Ready, 50, state);
}
}
