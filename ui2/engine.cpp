/*
 * Copyright (C) 2026 Recovery UI2 contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "recovery_ui2/engine.hpp"
#include "recovery_ui2/display_transform.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

#include <android/log.h>
#include <lvgl.h>
#include <minuitwrp/minui.h>
#include <pixelflinger/pixelflinger.h>
#include "design.hpp"
#include "gpu_renderer.hpp"
#include "picture_viewer.hpp"
#include "recorder/service.hpp"
#include "scene.hpp"
#include "ui_components.hpp"
#include "draw/opengles/lv_draw_opengles.h"

extern "C" int recovery_ui2_install_package(const char *path);
extern "C" int recovery_ui2_decrypt_data(const char *credential, int user_id);

namespace recovery_ui2 {
namespace {

constexpr char kLogTag[] = "RecoveryUI2";
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kTargetRefreshHz = 120;
// Rendering and DRM presentation consume several milliseconds on dodge. Wake
// after 2 ms so the complete pipeline still fits a 120 Hz frame deadline.
constexpr uint32_t kFrameIntervalMs = 2;
constexpr uint32_t kBufferAlignment = 64;

uint32_t MonotonicMilliseconds() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint32_t>(now.tv_sec * 1000ULL + now.tv_nsec / 1000000ULL);
}

} // namespace

class Engine::Impl final {
public:
  ~Impl() { Shutdown(); }

  bool Initialize(bool fastboot_mode, bool adaptive_resolution,
                  int32_t logical_height) {
    if (initialized_)
      return true;

    fastboot_mode_ = fastboot_mode;
    physical_width_ = gr_fb_width();
    physical_height_ = gr_fb_height();
    transform_ = adaptive_resolution
        ? DisplayTransform::Adaptive(physical_width_, physical_height_,
                                     logical_height)
        : DisplayTransform::Native(physical_width_, physical_height_);
    width_ = transform_.logical_width;
    height_ = transform_.logical_height;
    if (!transform_.IsValid()) {
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "minui is not initialized (invalid display %dx%d)",
                          physical_width_, physical_height_);
      return false;
    }

    direct_scanout_ = ConfigureDirectScanout();
    const size_t pixel_count = static_cast<size_t>(width_) * height_;
    if (pixel_count > SIZE_MAX / kBytesPerPixel)
      return false;
    const size_t logical_buffer_size = pixel_count * kBytesPerPixel;
    if (!direct_scanout_ || transform_.IsScaled()) {
      buffer_size_ = logical_buffer_size;
      if (posix_memalign(&draw_buffer_, kBufferAlignment, buffer_size_) != 0) {
        draw_buffer_ = nullptr;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "unable to allocate %zu-byte fallback render buffer",
                            buffer_size_);
        return false;
      }
      std::memset(draw_buffer_, 0, buffer_size_);
    }

    gpu_accelerated_ =
        direct_scanout_ &&
        gpu_renderer_.Initialize(physical_width_, physical_height_);
    lv_draw_opengles_set_enabled(gpu_accelerated_);
    lv_init();
    lv_tick_set_cb(MonotonicMilliseconds);

    display_ = gpu_accelerated_ ? gpu_renderer_.CreateDisplay(width_, height_)
                                : lv_display_create(width_, height_);
    if (display_ == nullptr && gpu_accelerated_) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "GPU display setup failed; restarting LVGL in "
                          "software mode");
      lv_deinit();
      gpu_renderer_.Shutdown();
      gpu_accelerated_ = false;
      lv_draw_opengles_set_enabled(false);
      lv_init();
      lv_tick_set_cb(MonotonicMilliseconds);
      display_ = lv_display_create(width_, height_);
    }
    if (display_ == nullptr) {
      Shutdown();
      return false;
    }
    lv_display_set_user_data(display_, this);
    if (gpu_accelerated_) {
      // The OpenGL texture display supplies its own dummy LVGL draw buffer.
      // Its flush callback below composites directly into imported DRM
      // scanout images.
    } else if (direct_scanout_ && !transform_.IsScaled()) {
      lv_display_set_color_format(display_, LV_COLOR_FORMAT_XRGB8888);
      lv_display_set_buffers_with_stride(
          display_, scanout_surface1_->data, scanout_surface2_->data,
          static_cast<uint32_t>(buffer_size_),
          static_cast<uint32_t>(scanout_surface1_->row_bytes),
          LV_DISPLAY_RENDER_MODE_DIRECT);
    } else {
      // A full-frame compositor is retained only as a safe non-DRM fallback.
      lv_display_set_color_format(
          display_, direct_scanout_ ? LV_COLOR_FORMAT_XRGB8888
                                    : LV_COLOR_FORMAT_ARGB8888);
      lv_display_set_buffers(display_, draw_buffer_, nullptr,
                             static_cast<uint32_t>(buffer_size_),
                             LV_DISPLAY_RENDER_MODE_FULL);
    }
    lv_display_set_flush_cb(display_, FlushDisplay);
    // The OpenGL texture remains in the panel's native portrait dimensions.
    // Let LVGL transform landscape draw coordinates into that texture instead
    // of relying on a flush callback to rotate pixels. The latter leaves stale
    // portrait regions because the OpenGL texture driver does not rotate them.
    if (gpu_accelerated_)
      lv_display_set_matrix_rotation(display_, true);

    pointer_device_ = lv_indev_create();
    if (pointer_device_ == nullptr) {
      Shutdown();
      return false;
    }
    lv_indev_set_type(pointer_device_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(pointer_device_, this);
    lv_indev_set_read_cb(pointer_device_, ReadPointer);
    /* LVGL's default 10% momentum decay feels abrupt with our normalized
     * velocity history. Four percent keeps the release natural while the
     * averaging in lv_indev prevents the old slingshot acceleration. */
    lv_indev_set_scroll_throw(pointer_device_, 4);

    ApplyStoredAppearance();
    if (fastboot_mode) {
      backend_ready_ = true;
      interactive_ready_ = true;
      current_scene_ = Action::kNone;
      on_home_ = false;
      BuildFastbootScene(lv_screen_active(), HandleSceneAction, this);
    } else {
      BuildBootScene(lv_screen_active(), HandleSceneAction, this);
    }
    initialized_ = true;
    __android_log_print(
        ANDROID_LOG_INFO, kLogTag,
        "engine initialized at logical %dx%d on physical %dx%d with %s "
        "rendering (%.2f MiB); "
        "%u Hz scheduler",
        width_, height_, physical_width_, physical_height_,
        gpu_accelerated_ ? "zero-copy Adreno/LVGL"
                         : (direct_scanout_ ? "software DRM scanout"
                                            : "minui fallback"),
        static_cast<double>(buffer_size_) / (1024.0 * 1024.0),
        kTargetRefreshHz);
    return true;
  }

  void Shutdown() {
    plugin_progress_.cancel.store(true);
    if (operation_thread_.joinable())
      operation_thread_.join();
    if (decrypt_thread_.joinable())
      decrypt_thread_.join();
    if (wifi_thread_.joinable())
      wifi_thread_.join();
    if (nas_thread_.joinable())
      nas_thread_.join();
    if (plugin_thread_.joinable())
      plugin_thread_.join();
    CancelEdgeSwipe();
    if (pointer_device_ != nullptr) {
      lv_indev_delete(pointer_device_);
    pointer_device_ = nullptr;
    }
    if (display_ != nullptr) {
      lv_display_delete(display_);
      display_ = nullptr;
    }
    if (lv_is_initialized())
      lv_deinit();
    gpu_renderer_.Shutdown();
    lv_draw_opengles_set_enabled(false);
    if (draw_buffer_ != nullptr) {
      std::free(draw_buffer_);
      draw_buffer_ = nullptr;
    }
    buffer_size_ = 0;
    scanout1_ = nullptr;
    scanout2_ = nullptr;
    scanout_surface1_ = nullptr;
    scanout_surface2_ = nullptr;
    direct_scanout_ = false;
    gpu_accelerated_ = false;
    backend_ready_ = false;
    boot_animation_complete_ = false;
    operation_running_ = false;
    decrypt_running_ = false;
    wifi_running_ = false;
    wifi_scene_report_ = false;
    nas_running_ = false;
    plugin_running_ = false;
    decryption_active_ = false;
    secondary_decryption_ = false;
    navigation_history_.clear();
    current_scene_ = Action::kBackHome;
    navigating_back_ = false;
    lock_overlay_ = nullptr;
    power_overlay_ = nullptr;
    volume_overlay_ = nullptr;
    notice_overlay_ = nullptr;
    suspended_ = false;
    interactive_ready_ = false;
    initialized_ = false;
  }

  uint32_t RunFrame() {
    if (!initialized_)
      return kFrameIntervalMs;
    if (suspended_)
      return 250;
#ifndef TW_OEM_BUILD
    if (rpc_fd >= 0) {
      pollfd rpc{rpc_fd, POLLIN, 0};
      if (poll(&rpc, 1, 0) > 0 && (rpc.revents & POLLIN))
    }
#endif
    recorder::Poll();
    if (fastboot_mode_) PollFastbootTelemetry();
    if (operation_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (operation_thread_.joinable())
        operation_thread_.join();
      const bool success = operation_result_.load(std::memory_order_acquire) == 0;
      CompleteOperationScene(
          operation_scene_, success,
          success ? "The requested operation completed. Review its output below."
                  : "The backend reported an error. Review the recovery log below.");
      operation_running_ = false;
    }
    if (operation_running_ && MonotonicMilliseconds() - last_operation_update_ >= 500) {
      last_operation_update_ = MonotonicMilliseconds();
      RefreshOperationScene(operation_scene_);
    }
    if (decrypt_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (decrypt_thread_.joinable()) decrypt_thread_.join();
      const bool success =
          decrypt_result_.load(std::memory_order_acquire) == 0;
      CompleteDecryptAttempt(decrypt_scene_, success);
      decrypt_running_ = false;
      if (success) {
        decryption_active_ = false;
        if (secondary_decryption_) {
          secondary_decryption_ = false;
          HandleSceneAction(Action::kUsers, this);
        } else {
          decryption_completion_ = DecryptionCompletion::kSuccess;
          ShowPreparing(true);
        }
      }
    }
    if (wifi_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (wifi_thread_.joinable()) wifi_thread_.join();
      const bool success = wifi_result_.load(std::memory_order_acquire) == 0;
      if (wifi_scene_report_) CompleteWifiOperation(wifi_scene_, success);
      wifi_scene_report_ = false;
      wifi_running_ = false;
    }
    if (nas_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (nas_thread_.joinable()) nas_thread_.join();
      const bool success = nas_result_.load(std::memory_order_acquire) == 0;
      CompleteNasOperation(nas_scene_, success);
      nas_running_ = false;
    }
    if (plugin_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (plugin_thread_.joinable()) plugin_thread_.join();
      const bool success = plugin_result_.load(std::memory_order_acquire) == 0;
      const std::string message = success ? plugin_progress_.status : plugin_progress_.error;
      CompletePluginOperation(plugin_scene_, success, message.c_str());
      plugin_running_ = false;
    }
    if (plugin_running_ && MonotonicMilliseconds() - last_plugin_update_ >= 120) {
      last_plugin_update_ = MonotonicMilliseconds();
      UpdatePluginProgress(plugin_scene_, plugin_progress_.value.load(),
                           plugin_progress_.downloaded_bytes.load(),
                           plugin_progress_.total_bytes.load());
    }
    const uint32_t next = lv_timer_handler();
#ifndef TW_OEM_BUILD
    // Capture only after LVGL has flushed and DRM exposes the newly presented
    // scanout buffer. The stream uses its own FIFO, so RPC input stays free.
#endif
    return std::clamp(next, 1U, kFrameIntervalMs);
  }

  Action TakeAction() {
    const Action action = pending_action_;
    pending_action_ = Action::kNone;
    return action;
  }

  DecryptionCompletion TakeDecryptionCompletion() {
    const DecryptionCompletion result = decryption_completion_;
    decryption_completion_ = DecryptionCompletion::kNone;
    return result;
  }

  void NavigateHome() {
    if (lock_overlay_ != nullptr) return;
    if (backend_ready_ && !operation_running_ && !wifi_running_ &&
        !nas_running_ && !plugin_running_ &&
        widgets::DismissModal(lv_screen_active())) return;
    if (backend_ready_ && current_tool_ == Action::kFiles && !operation_running_ &&
        !wifi_running_ && !nas_running_ && !plugin_running_ &&
        NavigateFileBack()) return;
    if (backend_ready_ && !operation_running_ && !wifi_running_ &&
        !nas_running_ && !plugin_running_ &&
        current_tool_ == Action::kFormatData) {
      HandleSceneAction(Action::kWipe, this);
      return;
    }
    if (backend_ready_ && !decryption_active_ && !on_home_ &&
        !operation_running_ && !wifi_running_ && !nas_running_ &&
        !plugin_running_) {
      navigation_history_.clear();
      current_scene_ = Action::kBackHome;
      ShowHome();
    }
  }

  void NavigateBack(bool feedback = true) {
    if (feedback) RecoveryVibrate(Haptic::kTouch);
    if (lock_overlay_ != nullptr || !backend_ready_ || decryption_active_ ||
        operation_running_ || wifi_running_ || nas_running_ || plugin_running_)
      return;
    if (widgets::DismissModal(lv_screen_active())) return;
    if (fastboot_mode_) {
      if (current_tool_ == Action::kFormatData) ShowFastboot();
      return;
    }
    if (current_scene_ == Action::kFiles && NavigateFileBack()) return;
    if (on_home_) return;

    Action target = Action::kBackHome;
    if (!navigation_history_.empty()) {
      target = navigation_history_.back();
      navigation_history_.pop_back();
    }
    navigating_back_ = true;
    HandleSceneAction(target, this);
    navigating_back_ = false;
  }

  void BeginDecryption(int credential_type, bool file_based, int user_id,
                       int pattern_grid_size) {
    if (interactive_ready_ || backend_ready_) return;
    interactive_ready_ = true;
    decryption_active_ = true;
    secondary_decryption_ = false;
    credential_type_ = credential_type;
    crypto_user_id_ = user_id;
    lv_obj_t *boot_screen = lv_screen_active();
    CompleteBootScene(boot_screen);
    lv_obj_t *decrypt = lv_obj_create(nullptr);
    decrypt_scene_ = BuildDecryptScene(
        decrypt, credential_type, file_based, user_id, pattern_grid_size,
        HandleSceneAction, this);
    on_home_ = false;
    lv_screen_load_anim(decrypt, LV_SCR_LOAD_ANIM_FADE_IN, 820, 0, true);
  }

  void SetBackendReady() {
    if (backend_ready_)
      return;
    // Shared-storage preferences may not be readable during the early boot
    // renderer. Reload them before constructing the first interactive scene.
    ApplyStoredAppearance();
    backend_ready_ = true;
    if (interactive_ready_) {
      ShowHome();
      return;
    }
    interactive_ready_ = true;
    // Backend readiness starts both halves of one synchronized handoff: the
    // wordmark zooms on the global overlay while the functional home screen
    // fades up underneath it.
    lv_obj_t *boot_screen = lv_screen_active();
    CompleteBootScene(boot_screen);
    lv_obj_t *home = lv_obj_create(nullptr);
    BuildHomeScene(home, HandleSceneAction, this);
    on_home_ = true;
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_FADE_IN, 820, 0, true);
  }

  void SetPointer(const PointerEvent &event) {
    const int32_t logical_x = transform_.ToLogicalX(event.x);
    const int32_t logical_y = transform_.ToLogicalY(event.y);
    if (PictureViewerHandlePointer(event.slot, logical_x, logical_y,
                                   event.pressed)) {
      CancelEdgeSwipe();
      pointer_.pressed = false;
      if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
      return;
    }
    if (event.slot != 0) return;
    const bool was_pressed = pointer_.pressed;
    pointer_.x = logical_x;
    pointer_.y = logical_y;
    pointer_.pressed = event.pressed;

    if (suspended_ || lock_overlay_ != nullptr || !backend_ready_ ||
        operation_running_ || wifi_running_ || nas_running_) {
      CancelEdgeSwipe();
      return;
    }

    if (event.pressed && !was_pressed) {
      const int32_t edge = std::max(72, width_ / 20);
      if (pointer_.x <= edge) BeginEdgeSwipe();
      return;
    }

    if (event.pressed && swipe_active_) {
      const int32_t vertical = std::abs(pointer_.y - swipe_start_y_);
      if (vertical > height_ / 8) {
        CancelEdgeSwipe();
        return;
      }
      swipe_last_y_ = pointer_.y;
      swipe_farthest_x_ = std::max(swipe_farthest_x_, pointer_.x);
      if (gesture_indicator_ != nullptr) {
        const int32_t progress = std::clamp(
            (swipe_farthest_x_ - swipe_start_x_) / 4, 12, 82);
        lv_obj_set_width(gesture_indicator_, progress);
        lv_obj_set_style_opa(gesture_indicator_,
                             std::clamp(70 + progress * 2, 70, 230), 0);
      }
      return;
    }

    if (!event.pressed && was_pressed && swipe_active_) {
      const int32_t horizontal = swipe_farthest_x_ - swipe_start_x_;
      // Several touch controllers zero ABS_MT_POSITION_Y when the tracking ID
      // is released. Validate direction with the last coordinate seen while
      // the contact was still active.
      const int32_t vertical = std::abs(swipe_last_y_ - swipe_start_y_);
      const bool accepted = horizontal >= width_ / 6 &&
                            horizontal > vertical * 2;
      CancelEdgeSwipe();
      if (accepted) {
        if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "left-edge Back gesture accepted");
        NavigateBack();
      }
    }
  }

  int32_t Width() const { return width_; }
  int32_t Height() const { return height_; }
  bool IsInitialized() const { return initialized_; }

  void SetSuspended(bool suspended) {
    suspended_ = suspended;
    if (!suspended_) return;
    CancelEdgeSwipe();
    pointer_.pressed = false;
    if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
  }

  void ShowLockScreen() {
    if (!initialized_ || lock_overlay_ != nullptr) return;
    CancelEdgeSwipe();
    pointer_.pressed = false;
    if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
    lock_overlay_ = BuildLockScene(lv_layer_top(), HandleSceneAction, this);
  }

  void ShowPowerMenu() {
    if (!initialized_ || power_overlay_ != nullptr || suspended_) return;
    CancelEdgeSwipe();
    pointer_.pressed = false;
    if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);

    const bool landscape = landscape_;
    auto *overlay = lv_obj_create(lv_layer_top());
    power_overlay_ = overlay;
    design::Clear(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
      auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
      if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (self != nullptr && self->power_overlay_ ==
            lv_event_get_target_obj(event)) self->power_overlay_ = nullptr;
      } else if (lv_event_get_code(event) == LV_EVENT_CLICKED && self != nullptr) {
        auto *target = lv_event_get_target_obj(event);
        if (target == self->power_overlay_) self->DismissPowerMenu();
      }
    }, LV_EVENT_ALL, this);

    auto *panel = lv_obj_create(overlay);
    design::Panel(panel, 52, design::kMainSheet);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_size(panel, landscape ? 2100 : 1180, landscape ? 620 : 780);
    lv_obj_center(panel);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, design::kMainLine, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_60, 0);
    auto *title = design::Label(panel, "Power menu", &lv_font_montserrat_48,
                                design::kText);
    lv_obj_set_pos(title, 52, 42);
    auto *detail = design::Label(panel, "Choose where AERA should go next",
                                 &lv_font_montserrat_24, design::kMuted);
    lv_obj_set_pos(detail, 52, 108);
    auto *close = widgets::Button(panel, LV_SYMBOL_CLOSE,
                                  [this] { DismissPowerMenu(); });
    lv_obj_set_size(close, 94, 94);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -38, 34);

    struct Destination {
      const char *icon;
      const char *title;
      const char *detail;
      Action action;
    };
    constexpr std::array<Destination, 4> destinations{{
      {LV_SYMBOL_HOME, "Android", "Boot the operating system", Action::kRebootSystem},
      {LV_SYMBOL_REFRESH, "Recovery", "Restart AERA", Action::kRebootRecovery},
      {LV_SYMBOL_SETTINGS, "Bootloader", "Open fastboot mode", Action::kRebootBootloader},
      {LV_SYMBOL_POWER, "Power off", "Shut down the device", Action::kPowerOff},
    }};
    for (size_t index = 0; index < destinations.size(); ++index) {
      const auto destination = destinations[index];
      const int columns = landscape ? 4 : 2;
      const int column = static_cast<int>(index) % columns;
      const int row = static_cast<int>(index) / columns;
      const int button_width = landscape ? 470 : 518;
      const int button_height = landscape ? 330 : 226;
      auto *button = lv_button_create(panel);
      design::Panel(button, 34,
                    destination.action == Action::kPowerOff
                        ? design::kRedSoft : design::kMainPanel);
      design::Interactive(button, design::kMainSelected);
      lv_obj_set_pos(button, 52 + column * (landscape ? 494 : 542),
                     190 + row * 246);
      lv_obj_set_size(button, button_width, button_height);
      widgets::OnClick(button, [this, destination] {
        pending_action_ = destination.action;
        DismissPowerMenu();
      });
      auto *icon = design::Label(button, destination.icon,
                                 &lv_font_montserrat_48,
                                 destination.action == Action::kPowerOff
                                     ? design::kRed : design::kAccent);
      lv_obj_set_pos(icon, 34, 32);
      auto *name = design::Label(button, destination.title,
                                 &lv_font_montserrat_32, design::kText);
      lv_obj_set_pos(name, 108, 38);
      auto *copy = design::Label(button, destination.detail,
                                 &lv_font_montserrat_24, design::kMuted);
      lv_obj_set_pos(copy, 34, landscape ? 132 : 132);
      lv_obj_set_width(copy, button_width - 68);
    }
    lv_obj_fade_in(panel, 160, 0);
  }

  void ShowVolume(int percent) {
    if (!initialized_) return;
    percent = std::clamp(percent, 0, 100);
    const char *symbol = percent == 0 ? LV_SYMBOL_CLOSE
                                      : (percent < 55 ? LV_SYMBOL_VOLUME_MID
                                                      : LV_SYMBOL_VOLUME_MAX);
    char amount[32];
    snprintf(amount, sizeof(amount), "Volume  %d%%", percent);

    if (volume_overlay_ == nullptr) {
      auto *panel = lv_obj_create(lv_layer_top());
      volume_overlay_ = panel;
      design::Panel(panel, 66, design::kMainSheet);
      lv_obj_set_size(panel, 1040, 196);
      lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 220);
      lv_obj_set_style_border_width(panel, 1, 0);
      lv_obj_set_style_border_color(panel, design::kMainLine, 0);
      lv_obj_set_style_border_opa(panel, LV_OPA_60, 0);
      lv_obj_set_style_pad_all(panel, 0, 0);
      lv_obj_add_event_cb(panel, [](lv_event_t *event) {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
        auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
        if (self == nullptr || self->volume_overlay_ != target) return;
        if (self->volume_hide_timer_ != nullptr) {
          lv_timer_delete(self->volume_hide_timer_);
          self->volume_hide_timer_ = nullptr;
        }
        self->volume_overlay_ = nullptr;
        self->volume_icon_ = nullptr;
        self->volume_label_ = nullptr;
        self->volume_fill_ = nullptr;
      }, LV_EVENT_DELETE, this);

      volume_icon_ = design::Label(panel, symbol, &lv_font_montserrat_48,
          percent == 0 ? design::kMuted : design::kAccent);
      lv_obj_set_pos(volume_icon_, 62, 54);
      volume_label_ = design::Label(panel, amount, &lv_font_montserrat_32,
                                    design::kText);
      lv_obj_set_pos(volume_label_, 150, 34);
      auto *track = lv_obj_create(panel);
      design::Clear(track);
      lv_obj_set_pos(track, 150, 108);
      lv_obj_set_size(track, 810, 24);
      lv_obj_set_style_radius(track, 12, 0);
      lv_obj_set_style_bg_color(track, design::kMainLine, 0);
      lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
      volume_fill_ = lv_obj_create(track);
      design::Clear(volume_fill_);
      lv_obj_set_height(volume_fill_, 24);
      lv_obj_set_style_radius(volume_fill_, 12, 0);
      lv_obj_set_style_bg_opa(volume_fill_, LV_OPA_COVER, 0);
      lv_obj_set_width(volume_fill_, std::max(1, displayed_volume_ * 810 / 100));

      lv_obj_set_style_translate_y(panel, -56, 0);
      lv_obj_set_style_opa(panel, LV_OPA_0, 0);
      lv_anim_t enter;
      lv_anim_init(&enter);
      lv_anim_set_var(&enter, panel);
      lv_anim_set_values(&enter, -56, 0);
      lv_anim_set_duration(&enter, 260);
      lv_anim_set_path_cb(&enter, lv_anim_path_overshoot);
      lv_anim_set_exec_cb(&enter, [](void *target, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(target), value, 0);
      });
      lv_anim_start(&enter);
      lv_obj_fade_in(panel, 160, 0);

      volume_hide_timer_ = lv_timer_create([](lv_timer_t *timer) {
        auto *self = static_cast<Impl *>(lv_timer_get_user_data(timer));
        if (self == nullptr) return;
        self->volume_hide_timer_ = nullptr;
        auto *panel = self->volume_overlay_;
        self->volume_overlay_ = nullptr;
        self->volume_icon_ = nullptr;
        self->volume_label_ = nullptr;
        self->volume_fill_ = nullptr;
        if (panel != nullptr) {
          lv_obj_fade_out(panel, 160, 0);
          lv_obj_delete_delayed(panel, 170);
        }
      }, 1350, this);
      lv_timer_set_repeat_count(volume_hide_timer_, 1);
    } else {
      lv_label_set_text(volume_icon_, symbol);
      lv_label_set_text(volume_label_, amount);
      lv_timer_reset(volume_hide_timer_);
      lv_timer_set_repeat_count(volume_hide_timer_, 1);
    }

    lv_obj_set_style_text_color(volume_icon_,
        percent == 0 ? design::kMuted : design::kAccent, 0);
    lv_obj_set_style_bg_color(volume_fill_,
        percent == 0 ? design::kMuted : design::kAccent, 0);
    const int start_width = lv_obj_get_width(volume_fill_);
    const int end_width = percent * 810 / 100;
    lv_anim_delete(volume_fill_, nullptr);
    lv_anim_t level;
    lv_anim_init(&level);
    lv_anim_set_var(&level, volume_fill_);
    lv_anim_set_values(&level, std::max(1, start_width),
                       std::max(1, end_width));
    lv_anim_set_duration(&level, 210);
    lv_anim_set_path_cb(&level, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&level, [](void *target, int32_t value) {
      lv_obj_set_width(static_cast<lv_obj_t *>(target), value);
    });
    lv_anim_start(&level);
    displayed_volume_ = percent;
  }

  void ShowScreenshotResult(bool success) {
    if (!initialized_) return;
    if (notice_overlay_ != nullptr) lv_obj_delete(notice_overlay_);
    auto *panel = lv_obj_create(lv_layer_top());
    notice_overlay_ = panel;
    design::Panel(panel, 58, design::kMainSheet);
    lv_obj_set_size(panel, 930, 154);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 440);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, success ? design::kGreen
                                                 : design::kRed, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_50, 0);
    lv_obj_add_event_cb(panel, [](lv_event_t *event) {
      auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
      auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
      if (self != nullptr && self->notice_overlay_ == target)
        self->notice_overlay_ = nullptr;
    }, LV_EVENT_DELETE, this);
    auto *icon = design::Label(panel, success ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                               &lv_font_montserrat_32,
                               success ? design::kGreen : design::kRed);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 46, 0);
    auto *label = design::Label(panel,
        success ? "Screenshot saved to AERA/screenshots"
                : "Screenshot could not be saved",
        &lv_font_montserrat_24, design::kText);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 118, 0);
    lv_obj_set_style_opa(panel, LV_OPA_0, 0);
    lv_obj_fade_in(panel, 180, 0);
    lv_obj_delete_delayed(panel, 1800);
  }

private:
  void ApplyStoredAppearance() {
    design::ApplySurfaceMode(RecoveryLightMode());
    design::ApplyAccent(RecoveryAccentColor());
    design::ApplyInterfaceSize(static_cast<int>(RecoveryInterfaceSize()));
  }

  void ShowFastboot() {
    current_tool_ = Action::kNone;
    on_home_ = false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    BuildFastbootScene(screen, HandleSceneAction, this);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
  }

  void DismissPowerMenu() {
    if (power_overlay_ == nullptr) return;
    auto *overlay = power_overlay_;
    power_overlay_ = nullptr;
    lv_obj_delete_async(overlay);
  }

  static void HandleSceneAction(Action action, void *context) {
    auto *self = static_cast<Impl *>(context);
    if (self == nullptr)
      return;

    if (action == Action::kBootComplete) {
      self->boot_animation_complete_ = true;
      return;
    }

    if (action == Action::kUnlock) {
      if (self->lock_overlay_ != nullptr) {
        lv_obj_t *lock = self->lock_overlay_;
        self->lock_overlay_ = nullptr;
        lv_obj_fade_out(lock, 220, 0);
        lv_obj_delete_delayed(lock, 230);
      }
      return;
    }

    if (self->decryption_active_) {
      if (action == Action::kDecryptSubmit) {
        self->StartDecryption();
      } else if (action == Action::kDecryptSkip && !self->decrypt_running_) {
        self->decryption_active_ = false;
        if (self->secondary_decryption_) {
          self->secondary_decryption_ = false;
          HandleSceneAction(Action::kUsers, self);
        } else {
          self->decryption_completion_ = DecryptionCompletion::kSkipped;
          self->ShowPreparing(false);
        }
      }
      return;
    }

    if (!self->backend_ready_)
      return;

    if (action == Action::kBack) {
      // UI back controls already provide their touch haptic. Hardware Back
      // and edge gestures call NavigateBack() directly with feedback enabled.
      self->NavigateBack(false);
      return;
    }

    if (self->operation_running_ || self->wifi_running_ || self->nas_running_ ||
        self->plugin_running_) return;
    self->CancelEdgeSwipe();

    if (self->fastboot_mode_) {
      if (action == Action::kFormatData) {
        self->current_tool_ = Action::kFormatData;
        lv_obj_t *screen = lv_obj_create(nullptr);
        BuildFastbootFormatScene(screen, HandleSceneAction, self);
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
        return;
      }
      if (action == Action::kRunOperation) {
        self->StartJob(GetJobRequest());
        return;
      }
      if (action == Action::kOpenReboot || action == Action::kBackHome ||
          action == Action::kWipe) {
        self->ShowFastboot();
        return;
      }
      if (action != Action::kRebootSystem &&
          action != Action::kRebootRecovery &&
          action != Action::kRebootBootloader &&
          action != Action::kPowerOff) return;
    }

    if (action == Action::kQuickWifiToggle) {
      WifiRequest request;
      request.operation = RecoveryWifiStatus().enabled
                              ? WifiOperation::kDisable
                              : WifiOperation::kEnable;
      self->StartWifi(request, false);
      return;
    }

    if (action == Action::kToggleRotation) {
      self->landscape_ = !self->landscape_;
      self->pointer_.pressed = false;
      if (self->pointer_device_ != nullptr)
        lv_indev_reset(self->pointer_device_, nullptr);
      lv_display_set_rotation(
          self->display_, self->landscape_ ? LV_DISPLAY_ROTATION_90
                                          : LV_DISPLAY_ROTATION_0);
      __android_log_print(ANDROID_LOG_INFO, kLogTag,
                          "display orientation changed to %s",
                          self->landscape_ ? "landscape" : "portrait");
      self->ShowHome();
      return;
    }

    if (action == Action::kToggleRecording) {
      if (recorder::Active()) recorder::Stop();
      else recorder::Start(static_cast<uint32_t>(self->physical_width_),
                           static_cast<uint32_t>(self->physical_height_));
      return;
    }

    if (action == Action::kRunOperation) {
      self->StartJob(GetJobRequest());
      return;
    }

    if (action == Action::kRunWifiOperation) {
      self->StartWifi(GetWifiRequest(), true);
      return;
    }

    if (action == Action::kRunNasOperation) {
      self->StartNas(GetNasRequest());
      return;
    }

    if (action == Action::kRunPluginOperation) {
      self->StartPlugin(GetPluginRequest());
      return;
    }

    if (action == Action::kDecryptUser) {
      const auto request = GetUserDecryptRequest();
      if (request.user.decrypted) {
        HandleSceneAction(Action::kUsers, self);
        return;
      }
      self->credential_type_ = request.user.credential_type;
      self->crypto_user_id_ = request.user.id;
      self->secondary_decryption_ = true;
      self->decryption_active_ = true;
      self->on_home_ = false;
      self->current_tool_ = Action::kUsers;
      lv_obj_t *screen = lv_obj_create(nullptr);
      self->decrypt_scene_ = BuildDecryptScene(
          screen, request.user.credential_type, true, request.user.id, 3,
          HandleSceneAction, self, request.user.name);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      if (request.user.credential_type == 0)
        self->StartDecryption("!");
      return;
    }

    if (action == Action::kBrowsePackages) {
      self->TrackScene(Action::kFiles);
      self->on_home_ = false;
      self->current_tool_ = Action::kFiles;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildFilesScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
      return;
    }

    if (action == Action::kFiles) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = Action::kFiles;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildFilesScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kPlugins) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = Action::kPlugins;
      lv_obj_t *screen = lv_obj_create(nullptr);
      self->plugin_scene_ = BuildPluginScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kInstallLocalPlugin) {
      self->TrackScene(Action::kPlugins);
      self->on_home_ = false;
      self->current_tool_ = Action::kPlugins;
      lv_obj_t *screen = lv_obj_create(nullptr);
      self->plugin_scene_ = BuildPluginScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      self->StartPlugin(GetPluginRequest());
      return;
    }

    if (action == Action::kNas) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      self->nas_scene_ = BuildNasScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kInstallPackage) {
      self->StartPackageInstall(GetSelectedPackagePath());
      return;
    }

    if (action == Action::kWeb) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildWebScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kRetroArch) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildRetroArchScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kTelegram) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildTelegramScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kGallery) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildGalleryScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kMedia) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildMediaScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kRecorder) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildRecorderScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kAppVault) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildAppVaultScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kPluginApp) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildGenericPluginScene(screen, GetSelectedPluginId(), HandleSceneAction,
                              self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kRootManager) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildRootManagerScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kTerminal) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildTerminalScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    const bool opens_tool =
        action == Action::kBackup || action == Action::kRestore ||
        action == Action::kWipe || action == Action::kFormatData ||
        action == Action::kSettings ||
        action == Action::kMounts || action == Action::kLogs ||
        action == Action::kPreferences || action == Action::kTheme ||
        action == Action::kWifi || action == Action::kUsers;
    if (action == Action::kOpenReboot || action == Action::kBackHome ||
        action == Action::kInstall || opens_tool) {
      self->TrackScene(action == Action::kInstall ? Action::kBackHome : action);
      lv_obj_t *screen = lv_obj_create(nullptr);
      if (action == Action::kOpenReboot) {
        self->current_tool_ = Action::kNone;
        self->on_home_ = false;
        BuildRebootScene(screen, HandleSceneAction, self);
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      } else if (opens_tool) {
        self->on_home_ = false;
        self->current_tool_ = action;
        if (action == Action::kWifi)
          self->wifi_scene_ = BuildWifiScene(screen, HandleSceneAction, self);
        else
          BuildToolScene(screen, action, HandleSceneAction, self);
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      } else {
        self->on_home_ = true;
        self->current_tool_ = Action::kNone;
        BuildHomeScene(screen, HandleSceneAction, self);
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      }
      return;
    }

    self->pending_action_ = action;
  }

  void ShowHome() {
    CancelEdgeSwipe();
    navigation_history_.clear();
    current_scene_ = Action::kBackHome;
    current_tool_ = Action::kNone;
    on_home_ = true;
    lv_obj_t *screen = lv_obj_create(nullptr);
    BuildHomeScene(screen, HandleSceneAction, this);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 140, 0, true);
  }

  void TrackScene(Action target) {
    if (target == Action::kNone) return;
    if (target == Action::kBackHome) {
      if (!navigating_back_) navigation_history_.clear();
      current_scene_ = target;
      return;
    }
    if (!navigating_back_ && current_scene_ != Action::kNone &&
        current_scene_ != target) {
      if (navigation_history_.empty() ||
          navigation_history_.back() != current_scene_)
        navigation_history_.push_back(current_scene_);
      if (navigation_history_.size() > 32)
        navigation_history_.erase(navigation_history_.begin());
    }
    current_scene_ = target;
  }

  void ShowPreparing(bool decrypted) {
    CancelEdgeSwipe();
    on_home_ = false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    BuildPreparingScene(screen, decrypted);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, true);
  }

  void StartDecryption(const std::string &supplied_credential = {}) {
    if (!decryption_active_ || decrypt_running_) return;
    const std::string credential = supplied_credential.empty()
                                       ? GetDecryptCredential(decrypt_scene_)
                                       : supplied_credential;
    const size_t minimum = credential_type_ == 2 ? 4U : 1U;
    if (credential.size() < minimum) {
      CompleteDecryptAttempt(decrypt_scene_, false);
      return;
    }

    SetDecryptBusy(decrypt_scene_);
    decrypt_running_ = true;
    decrypt_result_.store(-1, std::memory_order_release);
    decrypt_complete_.store(false, std::memory_order_release);
    decrypt_thread_ = std::thread([this, credential]() {
      const int result =
          recovery_ui2_decrypt_data(credential.c_str(), crypto_user_id_);
      decrypt_result_.store(result, std::memory_order_release);
      decrypt_complete_.store(true, std::memory_order_release);
    });
  }

  void StartPackageInstall(const char *path) {
    if (operation_running_ || nas_running_ || path == nullptr || path[0] == '\0')
      return;

    JobRequest request;
    request.job = Job::kInstall;
    request.path = path;
    request.title = "Install ZIP";
    StartJob(request);
  }

  void StartJob(const JobRequest &request) {
    if (operation_running_ || wifi_running_ || nas_running_ || plugin_running_)
      return;
    current_tool_ = Action::kNone;
    on_home_ = false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    operation_scene_ = BuildJobScene(screen, request,
                                           HandleSceneAction, this);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);

    operation_running_ = true;
    operation_result_.store(-1, std::memory_order_release);
    operation_complete_.store(false, std::memory_order_release);
    operation_thread_ = std::thread([this, request]() {
      const int result = RecoveryRunJob(request);
      operation_result_.store(result, std::memory_order_release);
      operation_complete_.store(true, std::memory_order_release);
    });
  }

  void StartWifi(const WifiRequest &request, bool report_to_scene) {
    if (wifi_running_ || operation_running_ || nas_running_) return;
    wifi_scene_report_ = report_to_scene;
    if (wifi_scene_report_) SetWifiBusy(wifi_scene_, request);
    wifi_running_ = true;
    wifi_result_.store(-1, std::memory_order_release);
    wifi_complete_.store(false, std::memory_order_release);
    wifi_thread_ = std::thread([this, request]() {
      const int result = RecoveryRunWifi(request);
      wifi_result_.store(result, std::memory_order_release);
      wifi_complete_.store(true, std::memory_order_release);
    });
  }

  void StartNas(const NasRequest &request) {
    if (nas_running_ || operation_running_ || wifi_running_ || plugin_running_)
      return;
    SetNasBusy(nas_scene_, request);
    nas_running_ = true;
    nas_result_.store(-1, std::memory_order_release);
    nas_complete_.store(false, std::memory_order_release);
    nas_thread_ = std::thread([this, request]() {
      const int result = RecoveryRunNas(request);
      nas_result_.store(result, std::memory_order_release);
      nas_complete_.store(true, std::memory_order_release);
    });
  }

  void StartPlugin(const plugins::Request &request) {
    if (plugin_running_ || operation_running_ || wifi_running_ || nas_running_)
      return;
    SetPluginBusy(plugin_scene_, request);
    plugin_progress_.value.store(0);
    plugin_progress_.downloaded_bytes.store(0);
    plugin_progress_.total_bytes.store(0);
    plugin_progress_.cancel.store(false);
    plugin_progress_.status.clear();
    plugin_progress_.error.clear();
    plugin_running_ = true;
    plugin_result_.store(-1, std::memory_order_release);
    plugin_complete_.store(false, std::memory_order_release);
    plugin_thread_ = std::thread([this, request]() {
      const bool result = plugins::Run(request, plugin_progress_);
      plugin_result_.store(result ? 0 : -1, std::memory_order_release);
      plugin_complete_.store(true, std::memory_order_release);
    });
  }

  void BeginEdgeSwipe() {
    CancelEdgeSwipe();
    swipe_active_ = true;
    swipe_start_x_ = pointer_.x;
    swipe_start_y_ = pointer_.y;
    swipe_last_y_ = pointer_.y;
    swipe_farthest_x_ = pointer_.x;

    gesture_indicator_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(gesture_indicator_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(gesture_indicator_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(gesture_indicator_, 0,
                   std::clamp(pointer_.y - 74, 80, height_ - 228));
    lv_obj_set_size(gesture_indicator_, 12, 148);
    lv_obj_set_style_radius(gesture_indicator_, 0, 0);
    lv_obj_set_style_radius(gesture_indicator_, 74, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gesture_indicator_, design::kAccent, 0);
    lv_obj_set_style_bg_opa(gesture_indicator_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gesture_indicator_, 0, 0);
    lv_obj_set_style_opa(gesture_indicator_, LV_OPA_30, 0);
    lv_obj_move_foreground(gesture_indicator_);
  }

  void CancelEdgeSwipe() {
    swipe_active_ = false;
    if (gesture_indicator_ != nullptr) {
      lv_obj_delete(gesture_indicator_);
      gesture_indicator_ = nullptr;
    }
  }

  bool PresentScaledSoftware(const uint8_t *pixels) {
    const unsigned int index = next_software_scanout_;
    GRSurface *surface = index == 0 ? scanout_surface1_ : scanout_surface2_;
    gr_surface target = index == 0 ? scanout1_ : scanout2_;
    if (surface == nullptr || target == nullptr || pixels == nullptr)
      return false;

    // This path is only used when the device has direct DRM scanout but the
    // optional Adreno renderer cannot start. It deliberately favors a small,
    // dependency-free nearest-neighbour fallback over making recovery fail.
    const auto *source = reinterpret_cast<const uint32_t *>(pixels);
    for (int32_t y = 0; y < physical_height_; ++y) {
      const int32_t source_y = static_cast<int32_t>(
          static_cast<int64_t>(y) * height_ / physical_height_);
      auto *destination = reinterpret_cast<uint32_t *>(
          surface->data + static_cast<size_t>(y) * surface->row_bytes);
      for (int32_t x = 0; x < physical_width_; ++x) {
        const int32_t source_x = static_cast<int32_t>(
            static_cast<int64_t>(x) * width_ / physical_width_);
        destination[x] = source[static_cast<size_t>(source_y) * width_ +
                                source_x];
      }
    }
    if (gr_drm_present(target) != 0) return false;
    next_software_scanout_ = 1U - index;
    return true;
  }

  static void FlushDisplay(lv_display_t *display, const lv_area_t *area,
                           uint8_t *pixels) {
    auto *self = static_cast<Impl *>(lv_display_get_user_data(display));
    if (self == nullptr || area == nullptr || pixels == nullptr) {
      lv_display_flush_ready(display);
      return;
    }

    if (self->gpu_accelerated_) {
      if (lv_display_flush_is_last(display)) {
        if (!self->gpu_renderer_.Present(display)) {
          __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                              "Adreno rejected the LVGL frame");
        } else {
          self->TrackSubmittedFrame();
          recorder::CapturePresentedFrame();
        }
      }
      lv_display_flush_ready(display);
      return;
    }

    if (self->direct_scanout_) {
      if (lv_display_flush_is_last(display)) {
        bool presented = false;
        if (self->transform_.IsScaled()) {
          presented = self->PresentScaledSoftware(pixels);
        } else {
          gr_surface target = nullptr;
          if (pixels == self->scanout_surface1_->data)
            target = self->scanout1_;
          else if (pixels == self->scanout_surface2_->data)
            target = self->scanout2_;
          presented = target != nullptr && gr_drm_present(target) == 0;
        }

        if (!presented) {
          __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                              "DRM rejected LVGL scanout buffer %p", pixels);
        } else {
          self->TrackSubmittedFrame();
          recorder::CapturePresentedFrame();
        }
      }
      lv_display_flush_ready(display);
      return;
    }

    GGLSurface surface{};
    surface.version = sizeof(surface);
    surface.width = static_cast<GGLuint>(self->width_);
    surface.height = static_cast<GGLuint>(self->height_);
    surface.stride = self->width_;
    surface.data = pixels;
    surface.format = GGL_PIXEL_FORMAT_BGRA_8888;

    if (lv_display_flush_is_last(display)) {
      gr_surface output = static_cast<gr_surface>(&surface);
      gr_surface scaled = nullptr;
      int32_t output_width = self->width_;
      int32_t output_height = self->height_;
      if (self->transform_.IsScaled()) {
        if (res_scale_surface(output, &scaled,
                              static_cast<float>(self->physical_width_) /
                                  self->width_,
                              static_cast<float>(self->physical_height_) /
                                  self->height_) == 0) {
          output = scaled;
          output_width = self->physical_width_;
          output_height = self->physical_height_;
        } else {
          __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                              "minui could not scale the adaptive frame");
        }
      }
      gr_blit(output, 0, 0, output_width, output_height, 0, 0);
      gr_flip();
      if (scaled != nullptr) res_free_surface(scaled);
      self->TrackSubmittedFrame();
      recorder::CapturePresentedFrame();
    }
    lv_display_flush_ready(display);
  }

  bool ConfigureDirectScanout() {
    if (gr_drm_get_scanout_buffers(&scanout1_, &scanout2_) != 0)
      return false;

    scanout_surface1_ = static_cast<GRSurface *>(scanout1_);
    scanout_surface2_ = static_cast<GRSurface *>(scanout2_);
    const bool valid =
        scanout_surface1_ != nullptr && scanout_surface2_ != nullptr &&
        scanout_surface1_->width == physical_width_ &&
        scanout_surface1_->height == physical_height_ &&
        scanout_surface2_->width == physical_width_ &&
        scanout_surface2_->height == physical_height_ &&
        scanout_surface1_->pixel_bytes == static_cast<int>(kBytesPerPixel) &&
        scanout_surface2_->pixel_bytes == static_cast<int>(kBytesPerPixel) &&
        scanout_surface1_->format == GGL_PIXEL_FORMAT_BGRA_8888 &&
        scanout_surface2_->format == GGL_PIXEL_FORMAT_BGRA_8888 &&
        scanout_surface1_->row_bytes == scanout_surface2_->row_bytes;
    if (!valid) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "DRM scanout format is incompatible; using fallback");
      scanout1_ = nullptr;
      scanout2_ = nullptr;
      scanout_surface1_ = nullptr;
      scanout_surface2_ = nullptr;
      return false;
    }

    buffer_size_ = static_cast<size_t>(scanout_surface1_->row_bytes) *
                   physical_height_;
    return buffer_size_ <= UINT32_MAX;
  }

  void TrackSubmittedFrame() {
    const uint32_t now = MonotonicMilliseconds();
    if (last_submit_ms_ == 0 || now - last_submit_ms_ > 250) {
      submit_window_start_ms_ = now;
      submitted_frames_ = 1;
      last_submit_ms_ = now;
      return;
    }

    ++submitted_frames_;
    last_submit_ms_ = now;
    const uint32_t elapsed = now - submit_window_start_ms_;
    if (elapsed < 1000)
      return;

    const double fps = static_cast<double>(submitted_frames_ - 1) * 1000.0 /
                       static_cast<double>(elapsed);
    __android_log_print(
        ANDROID_LOG_INFO, kLogTag,
        "active animation submitted %.1f fps (%u frames / %u ms)", fps,
        submitted_frames_, elapsed);
    submit_window_start_ms_ = now;
    submitted_frames_ = 1;
  }

  static void ReadPointer(lv_indev_t *input, lv_indev_data_t *data) {
    auto *self = static_cast<Impl *>(lv_indev_get_user_data(input));
    if (self == nullptr)
      return;
    data->point.x = self->pointer_.x;
    data->point.y = self->pointer_.y;
    data->state = self->pointer_.pressed ? LV_INDEV_STATE_PRESSED
                                         : LV_INDEV_STATE_RELEASED;
  }

  int32_t physical_width_ = 0;
  int32_t physical_height_ = 0;
  int32_t width_ = 0;
  Action current_tool_ = Action::kNone;
  Action current_scene_ = Action::kBackHome;
  int32_t height_ = 0;
  DisplayTransform transform_{};
  size_t buffer_size_ = 0;
  void *draw_buffer_ = nullptr;
  gr_surface scanout1_ = nullptr;
  gr_surface scanout2_ = nullptr;
  GRSurface *scanout_surface1_ = nullptr;
  GRSurface *scanout_surface2_ = nullptr;
  GpuRenderer gpu_renderer_{};
  lv_display_t *display_ = nullptr;
  lv_indev_t *pointer_device_ = nullptr;
  lv_obj_t *gesture_indicator_ = nullptr;
  lv_obj_t *lock_overlay_ = nullptr;
  lv_obj_t *power_overlay_ = nullptr;
  lv_obj_t *volume_overlay_ = nullptr;
  lv_obj_t *volume_icon_ = nullptr;
  lv_obj_t *volume_label_ = nullptr;
  lv_obj_t *volume_fill_ = nullptr;
  lv_timer_t *volume_hide_timer_ = nullptr;
  lv_obj_t *notice_overlay_ = nullptr;
  PointerEvent pointer_{};
  OperationScene operation_scene_{};
  DecryptScene decrypt_scene_{};
  WifiScene wifi_scene_{};
  NasScene nas_scene_{};
  PluginScene plugin_scene_{};
  std::string selected_package_;
  std::thread operation_thread_;
  std::thread decrypt_thread_;
  std::thread wifi_thread_;
  std::thread nas_thread_;
  std::thread plugin_thread_;
  std::atomic<int> operation_result_{-1};
  std::atomic<bool> operation_complete_{false};
  std::atomic<int> decrypt_result_{-1};
  std::atomic<bool> decrypt_complete_{false};
  std::atomic<int> wifi_result_{-1};
  std::atomic<bool> wifi_complete_{false};
  std::atomic<int> nas_result_{-1};
  std::atomic<bool> nas_complete_{false};
  std::atomic<int> plugin_result_{-1};
  std::atomic<bool> plugin_complete_{false};
  plugins::Progress plugin_progress_{};
  Action pending_action_ = Action::kNone;
  DecryptionCompletion decryption_completion_ = DecryptionCompletion::kNone;
  uint32_t submit_window_start_ms_ = 0;
  uint32_t last_submit_ms_ = 0;
  uint32_t submitted_frames_ = 0;
  uint32_t last_operation_update_ = 0;
  uint32_t last_plugin_update_ = 0;
  int32_t swipe_start_x_ = 0;
  int32_t swipe_start_y_ = 0;
  int32_t swipe_last_y_ = 0;
  int32_t swipe_farthest_x_ = 0;
  bool direct_scanout_ = false;
  unsigned int next_software_scanout_ = 0;
  bool gpu_accelerated_ = false;
  bool backend_ready_ = false;
  bool boot_animation_complete_ = false;
  bool operation_running_ = false;
  int credential_type_ = 0;
  int crypto_user_id_ = 0;
  bool decrypt_running_ = false;
  bool wifi_running_ = false;
  bool wifi_scene_report_ = false;
  bool nas_running_ = false;
  bool plugin_running_ = false;
  bool decryption_active_ = false;
  bool secondary_decryption_ = false;
  bool interactive_ready_ = false;
  bool navigating_back_ = false;
  bool swipe_active_ = false;
  bool suspended_ = false;
  bool fastboot_mode_ = false;
  bool on_home_ = false;
  bool landscape_ = false;
  bool initialized_ = false;
  std::vector<Action> navigation_history_;
  int displayed_volume_ = 30;
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

bool Engine::Initialize(bool fastboot_mode, bool adaptive_resolution,
                        int32_t logical_height) {
  return impl_->Initialize(fastboot_mode, adaptive_resolution, logical_height);
}
void Engine::Shutdown() { impl_->Shutdown(); }
void Engine::AbandonForTerminalAction() { impl_.release(); }
uint32_t Engine::RunFrame() { return impl_->RunFrame(); }
Action Engine::TakeAction() { return impl_->TakeAction(); }
void Engine::NavigateHome() { impl_->NavigateHome(); }
void Engine::NavigateBack() { impl_->NavigateBack(); }
void Engine::SetBackendReady() { impl_->SetBackendReady(); }
void Engine::BeginDecryption(int credential_type, bool file_based, int user_id,
                             int pattern_grid_size) {
  impl_->BeginDecryption(credential_type, file_based, user_id,
                         pattern_grid_size);
}
DecryptionCompletion Engine::TakeDecryptionCompletion() {
  return impl_->TakeDecryptionCompletion();
}
void Engine::SetPointer(const PointerEvent &event) { impl_->SetPointer(event); }
void Engine::SetSuspended(bool suspended) { impl_->SetSuspended(suspended); }
void Engine::ShowLockScreen() { impl_->ShowLockScreen(); }
void Engine::ShowPowerMenu() { impl_->ShowPowerMenu(); }
void Engine::ShowVolume(int percent) { impl_->ShowVolume(percent); }
void Engine::ShowScreenshotResult(bool success) {
  impl_->ShowScreenshotResult(success);
}
int32_t Engine::Width() const { return impl_->Width(); }
int32_t Engine::Height() const { return impl_->Height(); }
bool Engine::IsInitialized() const { return impl_->IsInitialized(); }

} // namespace recovery_ui2
