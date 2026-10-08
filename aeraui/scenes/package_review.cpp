/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "package_review.hpp"
#include "ui_components.hpp"
#include "update/payload_inspector.hpp"
#include "update/payload_arb.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdio>

namespace aeraui {
namespace {
using namespace widgets;
struct ArbProgress {
  payload::Info info;
  payload::DeviceArb device;
  std::atomic<bool> done{false};
};
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
lv_obj_t *Row(lv_obj_t *parent, const char *key, const std::string &value) {
  if (value.empty()) return nullptr;
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
  return text;
}
void ArbRow(lv_obj_t *parent, const std::shared_ptr<ArbProgress> &progress, int kind = 0) {
  auto *label = Row(parent, kind == 1 ? "ARB source" : kind == 2 ? "Slot A ARB" :
      kind == 3 ? "Slot B ARB" : "Package ARB", "Loading...");
  struct Watch {
    lv_obj_t *label;
    std::shared_ptr<ArbProgress> progress;
    int kind;
    lv_timer_t *timer = nullptr;
  };
  auto *watch = new Watch{label, progress, kind};
  lv_obj_add_event_cb(label, [](lv_event_t *event) {
    auto *watch = static_cast<Watch *>(lv_event_get_user_data(event));
    if (watch->timer) lv_timer_delete(watch->timer);
    delete watch;
  }, LV_EVENT_DELETE, watch);
  watch->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *watch = static_cast<Watch *>(lv_timer_get_user_data(timer));
    if (!watch->progress->done.load(std::memory_order_acquire)) return;
    const auto &info = watch->progress->info;
    const auto &slot = watch->kind == 2 ? watch->progress->device.slot_a : watch->progress->device.slot_b;
    const auto value = watch->kind >= 2 ? (slot.available ? std::to_string(slot.index) : std::string("Unknown")) :
        watch->kind == 1 ? info.arb_detail :
        info.arb_available ? std::to_string(info.arb_index) : std::string("Unavailable");
    i18n::BindLabel(watch->label, value.c_str());
    watch->timer = nullptr;
    lv_timer_delete(timer);
  }, 100, watch);
}
struct InstallGate {
  std::shared_ptr<ArbProgress> progress;
  bool includes_firmware = false;
  bool acknowledged = false;
  bool warned = false;
  lv_obj_t *overlay = nullptr;
  lv_obj_t *body = nullptr;
  lv_obj_t *details = nullptr;
  lv_obj_t *slider = nullptr;
  payload::ArbDecision Decision() const {
    if (!progress->done.load(std::memory_order_acquire)) return payload::ArbDecision::Unknown;
    const auto &info = progress->info;
    return payload::CompareArb(includes_firmware,
        {info.arb_available, info.arb_index, info.arb_detail}, progress->device);
  }
  bool AllowsInstall() const {
    return progress->done.load(std::memory_order_acquire) &&
        payload::ArbAllowsInstall(Decision(), acknowledged);
  }
};
void FitReview(const InstallGate &gate) {
  auto *sheet = lv_obj_get_parent(gate.body);
  lv_obj_set_height(gate.body, LV_SIZE_CONTENT);
  lv_obj_update_layout(sheet);
  const int content_height = lv_obj_get_height(gate.body);
  // Shared Sheet reserves 670 logical pixels for its heading and swipe footer.
  // Use only the space the metadata needs, capped to the available screen.
  const int height = std::min(content_height + 670,
      static_cast<int>(lv_obj_get_height(gate.overlay)) - 80);
  lv_obj_set_height(sheet, height);
  lv_obj_set_height(gate.body, std::max(80, height - 670));
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -40);
}
void UpdateGate(const std::shared_ptr<InstallGate> &shared);
void ShowArbWarning(const std::shared_ptr<InstallGate> &gate, const char *message) {
  const bool upgrade = gate->Decision() == payload::ArbDecision::Upgrade;
  Sheet(gate->overlay, upgrade ? "Anti-rollback upgrade" : "Installation blocked", "", {}, upgrade ? 1000 : 900,
      false, SheetPresentation::kStandard, "", [gate, message, upgrade](lv_obj_t *area) {
    Layout(area);
    if (upgrade) {
      auto *sheet = lv_obj_get_parent(area);
      lv_obj_update_layout(sheet);
      // Reserve a fixed two-button footer; long warning text stays scrollable.
      lv_obj_set_height(area, std::max(80, static_cast<int>(lv_obj_get_height(sheet)) - 496));
    }
    const auto &info = gate->progress->info;
    const auto &device = gate->progress->device;
    Row(area, "Package ARB", info.arb_available ? std::to_string(info.arb_index) : "Unknown");
    Row(area, "Slot A ARB", device.slot_a.available ? std::to_string(device.slot_a.index) : "Unknown");
    Row(area, "Slot B ARB", device.slot_b.available ? std::to_string(device.slot_b.index) : "Unknown");
    auto *text = Label(area, message, &lv_font_montserrat_32, kText);
    lv_obj_set_width(text, LV_PCT(100));
  }, [gate, upgrade](lv_obj_t *, lv_obj_t *close) {
    auto *sheet = lv_obj_get_parent(close);
    auto style_button = [](lv_obj_t *button) {
      lv_obj_set_size(button, LV_PCT(100), 100);
      lv_obj_set_style_radius(button, 24, 0);
      auto *label = lv_obj_get_child(button, 0);
      lv_obj_set_width(label, LV_PCT(100));
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(label);
    };
    i18n::BindLabel(lv_obj_get_child(close, 0), "Abort");
    style_button(close);
    lv_obj_set_style_bg_color(close, Color(0xB32632), 0);
    lv_obj_set_style_bg_color(close, Color(0x8D1D27), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(close, 0), lv_color_white(), 0);
    if (upgrade) {
      auto *warning = lv_obj_get_parent(sheet);
      auto *confirm = Button(sheet, "I understand - continue", [gate, warning] {
        gate->acknowledged = true;
        lv_obj_delete_async(warning);
        UpdateGate(gate);
      });
      style_button(confirm);
      lv_obj_align(confirm, LV_ALIGN_BOTTOM_MID, 0, -128);
    }
  }, [overlay = gate->overlay] { lv_obj_delete_async(overlay); });
}
void UpdateGate(const std::shared_ptr<InstallGate> &shared) {
  auto &gate = *shared;
  if (!gate.progress->done.load(std::memory_order_acquire)) return;
  using D = payload::ArbDecision;
  const auto decision = gate.Decision();
  if (decision == D::Downgrade)
    lv_obj_add_flag(gate.details, LV_OBJ_FLAG_HIDDEN);
  if (gate.AllowsInstall()) lv_obj_remove_flag(gate.slider, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(gate.slider, LV_OBJ_FLAG_HIDDEN);
  FitReview(gate);
  if (!gate.warned && (decision == D::Upgrade || decision == D::Downgrade)) {
    gate.warned = true;
    ShowArbWarning(shared, decision == D::Downgrade ?
        "Installation blocked: this package has a lower ARB index than firmware in at least one slot. Flashing it can brick this device. Abort this installation." :
        "Warning: this update raises the firmware ARB index compared with at least one slot. It may permanently prevent returning to older firmware. Confirm below before swiping to install.");
  }
}
void Section(lv_obj_t *parent, const char *text) {
  auto *label = Label(parent, text, &lv_font_montserrat_28, kAccent);
  lv_obj_set_style_pad_top(label, 24, 0);
}
void FullInfo(lv_obj_t *screen, const payload::Info &info, const std::string &path,
              const std::shared_ptr<ArbProgress> &arb) {
  Sheet(screen, "Update information", "", {}, 2000, false,
        SheetPresentation::kStandard, "Swipe to confirm", [info, path, arb](lv_obj_t *area) {
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
    ArbRow(area, arb);
    ArbRow(area, arb, true);
    Section(area, "PARTITIONS");
    for (const auto &partition : info.partitions)
      Row(area, partition.name.c_str(), Size(partition.bytes));
  });
}
struct Result {
  payload::Info info;
  std::atomic<bool> done{false};
  std::shared_ptr<ArbProgress> arb = std::make_shared<ArbProgress>();
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
  const auto arb = state->result->arb;
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
  auto gate = std::make_shared<InstallGate>();
  gate->progress = arb;
  gate->includes_firmware = std::any_of(info.partitions.begin(), info.partitions.end(),
      [](const auto &p) { return p.name == "xbl_config"; });
  Sheet(screen, "Review Android update", "", [gate, install] {
    if (gate->AllowsInstall()) install();
  }, 1900, false,
        SheetPresentation::kStandard, "Swipe to install", [info, path, arb, gate](lv_obj_t *area) {
    Layout(area);
    gate->body = area;
    if (info.name_from_filename) Section(area, "PACKAGE NAME");
    auto *version = Label(area, info.target_build.empty() ? "Android update" : info.target_build.c_str(),
                          &lv_font_montserrat_40, kText);
    lv_obj_set_width(version, LV_PCT(100));
    Row(area, "Device", info.target_device);
    Row(area, "Android", info.target_sdk);
    Row(area, "Security patch", info.security_patch);
    Row(area, "Update type", info.incremental ? "Incremental OTA" : "Full OTA");
    Row(area, "Payload size", Size(info.payload_bytes));
    ArbRow(area, arb);
    auto *details = Button(area, "Full information  " LV_SYMBOL_RIGHT,
        [info, path, arb, gate] { FullInfo(gate->overlay, info, path, arb); });
    lv_obj_set_size(details, LV_PCT(100), 96);
    lv_obj_set_style_radius(details, 24, 0);
    lv_obj_set_style_border_width(details, 1, 0);
    lv_obj_set_style_border_color(details, kMainLine, 0);
    gate->details = details;
  }, [gate](lv_obj_t *slider, lv_obj_t *close) {
    gate->slider = slider;
    gate->overlay = lv_obj_get_parent(lv_obj_get_parent(slider));
    FitReview(*gate);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_HIDDEN);
    i18n::BindLabel(lv_obj_get_child(close, 0), "Abort");
    struct Watch { std::shared_ptr<InstallGate> gate; lv_timer_t *timer = nullptr; };
    auto *watch = new Watch{gate};
    lv_obj_add_event_cb(slider, [](lv_event_t *event) {
      auto *watch = static_cast<Watch *>(lv_event_get_user_data(event));
      if (watch->timer) lv_timer_delete(watch->timer);
      delete watch;
    }, LV_EVENT_DELETE, watch);
    watch->timer = lv_timer_create([](lv_timer_t *timer) {
      auto *watch = static_cast<Watch *>(lv_timer_get_user_data(timer));
      if (!watch->gate->progress->done.load(std::memory_order_acquire)) return;
      UpdateGate(watch->gate);
      watch->timer = nullptr;
      lv_timer_delete(timer);
    }, 100, watch);
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
    if (result->info.valid) {
      result->arb->info = payload::InspectZip(path, true);
      result->arb->device = payload::ReadDeviceArb();
      if (result->arb->info.arb_detail.empty())
        result->arb->info.arb_detail = "The package could not be read for the ARB scan";
    }
    result->arb->done.store(true, std::memory_order_release);
  }).detach();
  state->timer = lv_timer_create(Ready, 50, state);
}
}
