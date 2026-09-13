/*
 * Copyright (C) 2026 Recovery UI2 contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "recovery_ui2/runner.hpp"

#include <linux/input.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <android/log.h>
#include <minuitwrp/minui.h>

#include "recovery_ui2/engine.hpp"
#include "recovery_ui2/status_bar.hpp"

namespace recovery_ui2 {
namespace {

constexpr char kLogTag[] = "RecoveryUI2";
constexpr char kPrimeMinFrequency[] =
    "/sys/devices/system/cpu/cpufreq/policy6/scaling_min_freq";
constexpr char kPrimeMaxFrequency[] =
    "/sys/devices/system/cpu/cpufreq/policy6/cpuinfo_max_freq";
constexpr char kGpuMinPowerLevel[] =
    "/sys/class/kgsl/kgsl-3d0/min_pwrlevel";
constexpr char kGpuPowerLevelCount[] =
    "/sys/class/kgsl/kgsl-3d0/num_pwrlevels";

uint64_t MonotonicMilliseconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
           static_cast<uint64_t>(now.tv_nsec) / 1000000ULL;
}

bool ReadControl(const char* path, std::string* value) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char data[64]{};
    const ssize_t count = read(fd, data, sizeof(data) - 1);
    close(fd);
    if (count <= 0) return false;
    data[count] = '\0';
    *value = data;
    while (!value->empty() &&
           (value->back() == '\n' || value->back() == '\r' ||
            value->back() == ' '))
        value->pop_back();
    return !value->empty();
}

bool WriteControl(const char* path, const std::string& value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool complete =
        write(fd, value.data(), value.size()) ==
        static_cast<ssize_t>(value.size());
    close(fd);
    return complete;
}

class InteractionBoost final {
  public:
    InteractionBoost() {
        affinity_valid_ = sched_getaffinity(0, sizeof(original_affinity_),
                                            &original_affinity_) == 0;
        CPU_ZERO(&prime_affinity_);
        const long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu_count >= 2) {
            CPU_SET(cpu_count - 1, &prime_affinity_);
            CPU_SET(cpu_count - 2, &prime_affinity_);
            prime_affinity_valid_ = true;
        }
        DiscoverControls();
    }

    ~InteractionBoost() { Restore(); }

    void Boost() { BoostFor(2000); }

    void BoostFor(uint64_t duration_ms) {
        const uint64_t now = MonotonicMilliseconds();
        deadline_ms_ = std::max(deadline_ms_, now + duration_ms);
        // RunLoop starts before KGSL has necessarily published its sysfs
        // controls. Retry discovery after renderer initialization instead of
        // permanently disabling the GPU boost on that first ENOENT.
        DiscoverControls();
        if (!active_ && prime_affinity_valid_)
            sched_setaffinity(0, sizeof(prime_affinity_), &prime_affinity_);
        // A vendor power service can restore the default floor after GPU
        // bring-up. Reassert at a low rate while the boot/interaction window
        // is active; never write sysfs once per rendered frame.
        if (!active_ || now >= next_reassert_ms_) {
            if (cpu_controls_valid_)
                WriteControl(kPrimeMinFrequency, boost_cpu_min_);
            if (gpu_controls_valid_)
                WriteControl(kGpuMinPowerLevel, boost_gpu_level_);
            next_reassert_ms_ = now + 500;
        }
        active_ = true;
    }

    void Update() {
        if (active_ && MonotonicMilliseconds() >= deadline_ms_) Restore();
    }

  private:
    void DiscoverControls() {
        if (!cpu_controls_valid_) {
            cpu_controls_valid_ =
                ReadControl(kPrimeMinFrequency, &original_cpu_min_) &&
                ReadControl(kPrimeMaxFrequency, &boost_cpu_min_);
        }
        if (!gpu_controls_valid_) {
            std::string levels;
            gpu_controls_valid_ =
                ReadControl(kGpuMinPowerLevel, &original_gpu_level_) &&
                ReadControl(kGpuPowerLevelCount, &levels);
            if (gpu_controls_valid_) {
                const int count = std::max(1, atoi(levels.c_str()));
                /* Dodge maps lower KGSL power-level numbers to faster clocks.
                 * The upper-quarter level is 900 MHz on Adreno 830: enough
                 * headroom for 120 Hz composition without forcing 1.10 GHz. */
                boost_gpu_level_ = std::to_string((count - 1) / 4);
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "KGSL boost controls ready: level %s of %d",
                                    boost_gpu_level_.c_str(), count);
            }
        }
    }

    void Restore() {
        if (!active_) return;
        if (gpu_controls_valid_)
            WriteControl(kGpuMinPowerLevel, original_gpu_level_);
        if (cpu_controls_valid_)
            WriteControl(kPrimeMinFrequency, original_cpu_min_);
        if (affinity_valid_)
            sched_setaffinity(0, sizeof(original_affinity_),
                              &original_affinity_);
        active_ = false;
        next_reassert_ms_ = 0;
    }

    cpu_set_t original_affinity_{};
    cpu_set_t prime_affinity_{};
    std::string original_cpu_min_;
    std::string boost_cpu_min_;
    std::string original_gpu_level_;
    std::string boost_gpu_level_;
    uint64_t deadline_ms_ = 0;
    uint64_t next_reassert_ms_ = 0;
    bool affinity_valid_ = false;
    bool prime_affinity_valid_ = false;
    bool cpu_controls_valid_ = false;
    bool gpu_controls_valid_ = false;
    bool active_ = false;
};

std::atomic<bool> gBackendReady{false};
std::atomic<bool> gDecryptRequested{false};
std::atomic<int> gCredentialType{0};
std::atomic<int> gCryptoUserId{0};
std::atomic<int> gPatternGridSize{3};
std::atomic<bool> gFileBasedEncryption{false};
std::mutex gEarlyMutex;
std::mutex gDecryptMutex;
std::condition_variable gDecryptCondition;
std::thread gEarlyThread;
bool gEarlyStarted = false;
bool gDecryptResolved = false;
DecryptionResult gDecryptResult = DecryptionResult::kUnavailable;
RunResult gEarlyResult = RunResult::kUnexpectedExit;
DisplayMetrics gDisplayMetrics{};

struct HardwareState {
    bool screen_off = false;
    bool power_down = false;
    bool volume_down = false;
    bool woke_on_power = false;
    bool power_long_press = false;
    bool screenshot_chord = false;
    bool screenshot_pending = false;
    bool suppress_volume_down = false;
    uint64_t power_pressed_at_ms = 0;
    int volume = 30;
};

void WriteVolume(int volume) {
    constexpr char kVolume[] = "/tmp/aera-audio-volume";
    const int fd = open(kVolume, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        0600);
    if (fd < 0) return;
    const std::string value = std::to_string(volume);
    if (write(fd, value.data(), value.size()) !=
        static_cast<ssize_t>(value.size())) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "could not update browser audio volume");
    }
    close(fd);
}

bool EnsureDirectory(const char* path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

void CaptureHardwareScreenshot(Engine& engine) {
    bool saved = false;
    if (EnsureDirectory("/sdcard/AERA") &&
        EnsureDirectory("/sdcard/AERA/screenshots")) {
        const time_t now = time(nullptr);
        struct tm local {};
        char stamp[32] = "unknown";
        if (localtime_r(&now, &local) != nullptr)
            strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
        const std::string path =
            std::string("/sdcard/AERA/screenshots/AERA-") + stamp + ".png";
        saved = gr_save_screenshot(path.c_str()) == 0;
        if (saved) {
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "hardware screenshot saved to %s", path.c_str());
        }
    }
    if (!saved)
        gr_save_screenshot("/tmp/AERA-Screenshot.png");
    engine.ShowScreenshotResult(saved);
}

void AdjustVolume(Engine& engine, InteractionBoost& performance,
                  HardwareState& hardware, int amount) {
    if (hardware.screen_off) return;
    hardware.volume = std::clamp(hardware.volume + amount, 0, 100);
    WriteVolume(hardware.volume);
    performance.Boost();
    engine.ShowVolume(hardware.volume);
}

bool HandleEvent(Engine& engine, InteractionBoost& performance,
                 HardwareState& hardware, const input_event& event) {
    if (event.type == EV_ABS) {
        if (hardware.screen_off) return false;
        performance.Boost();
        PointerEvent pointer;
        pointer.x = event.value >> 16;
        pointer.y = event.value & 0xffff;
        pointer.slot = event.code >= 2 ? 1 : 0;
        pointer.pressed = event.code == 1 || event.code == 3;
        engine.SetPointer(pointer);
        if (!pointer.pressed) {
            __android_log_print(ANDROID_LOG_DEBUG, kLogTag, "touch released at %d,%d", pointer.x,
                                pointer.y);
        }
        return false;
    }

    if (event.type != EV_KEY) return false;

    if (event.code == KEY_POWER) {
        if (event.value == 1) {
            hardware.power_down = true;
            hardware.power_long_press = false;
            hardware.power_pressed_at_ms = MonotonicMilliseconds();
            hardware.screenshot_chord = false;
            if (hardware.screen_off) {
                hardware.woke_on_power = true;
                engine.ShowLockScreen();
                engine.SetSuspended(false);
                performance.Boost();
                engine.RunFrame();
                gr_fb_blank(false);
                hardware.screen_off = false;
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "display woke into AERA lock screen");
            } else if (hardware.volume_down) {
                hardware.screenshot_chord = true;
                hardware.suppress_volume_down = true;
                hardware.screenshot_pending = true;
            }
        } else if (event.value == 0) {
            if (!hardware.screen_off && !hardware.woke_on_power &&
                !hardware.power_long_press && !hardware.screenshot_chord) {
                engine.SetSuspended(true);
                gr_fb_blank(true);
                hardware.screen_off = true;
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "display suspended by power key");
            }
            hardware.power_down = false;
            hardware.power_long_press = false;
            hardware.power_pressed_at_ms = 0;
            hardware.woke_on_power = false;
            hardware.screenshot_chord = false;
        }
        return false;
    }

    if (event.code == KEY_VOLUMEDOWN) {
        if (event.value == 1) {
            hardware.volume_down = true;
            if (hardware.power_down && !hardware.screen_off &&
                !hardware.woke_on_power && !hardware.screenshot_chord) {
                hardware.screenshot_chord = true;
                hardware.suppress_volume_down = true;
                hardware.screenshot_pending = true;
            }
        } else if (event.value == 0) {
            hardware.volume_down = false;
            if (hardware.suppress_volume_down)
                hardware.suppress_volume_down = false;
            else
                AdjustVolume(engine, performance, hardware, -5);
        }
        return false;
    }

    if (event.code == KEY_VOLUMEUP && event.value == 0) {
        AdjustVolume(engine, performance, hardware, 5);
        return false;
    }

    if (event.code == KEY_BACK && event.value == 0 && !hardware.screen_off) {
        engine.NavigateBack();
    }
    return false;
}

void CaptureFrameIfRequested() {
    constexpr char kRequest[] = "/tmp/recovery-ui2-capture";
    if (access(kRequest, F_OK) != 0) return;
    unlink(kRequest);
    const int result = gr_save_screenshot("/tmp/recovery-ui2.png");
    __android_log_print(result == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kLogTag,
                        "frame capture finished with result %d", result);
}

RunResult ToRunResult(Action action) {
    switch (action) {
        case Action::kRebootSystem: return RunResult::kRebootSystem;
        case Action::kRebootRecovery: return RunResult::kRebootRecovery;
        case Action::kRebootBootloader: return RunResult::kRebootBootloader;
        case Action::kRebootFastbootd: return RunResult::kRebootFastbootd;
        case Action::kPowerOff: return RunResult::kPowerOff;
        default: return RunResult::kUnexpectedExit;
    }
}

}  // namespace

RunResult RunLoop(bool fastboot_mode = false,
                  const DisplayMetrics& metrics = {}) {
    InteractionBoost performance;
    performance.Boost();
    HardwareState hardware;
    WriteVolume(hardware.volume);
    ConfigureStatusBar(metrics.status_bar_height, metrics.status_indent_left,
                       metrics.status_indent_right);
    Engine engine;
    if (!engine.Initialize(fastboot_mode, metrics.adaptive_resolution,
                           metrics.logical_height))
        return RunResult::kEngineFailure;

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "AERA Recovery Project native engine active; hardware Back and edge swipe use navigation history");

    bool backend_ready_sent = fastboot_mode;
    bool decrypt_request_sent = false;
    for (;;) {
        /* Boot and decryption have no continuous touch stream to renew the
         * interaction window. Keep the clocks up until the recovery backend
         * is ready, then retain the boost long enough for the wordmark outro
         * and the first home frames to finish without a late clock drop. */
        if (!backend_ready_sent)
            performance.Boost();
        if (hardware.power_down && !hardware.power_long_press &&
            !hardware.screenshot_chord && hardware.power_pressed_at_ms != 0 &&
            MonotonicMilliseconds() - hardware.power_pressed_at_ms >= 2000) {
            hardware.power_long_press = true;
            engine.ShowPowerMenu();
            performance.Boost();
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "power key held for 3 seconds; showing power menu");
        }
        if (!fastboot_mode && !decrypt_request_sent &&
            gDecryptRequested.load(std::memory_order_acquire)) {
            engine.BeginDecryption(
                gCredentialType.load(std::memory_order_acquire),
                gFileBasedEncryption.load(std::memory_order_acquire),
                gCryptoUserId.load(std::memory_order_acquire),
                gPatternGridSize.load(std::memory_order_acquire));
            decrypt_request_sent = true;
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "native decrypt page shown at stock startup gate");
        }
        if (!backend_ready_sent && gBackendReady.load(std::memory_order_acquire)) {
            engine.SetBackendReady();
            backend_ready_sent = true;
            performance.BoostFor(5000);
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "recovery backend ready; native workflows unlocked");
        }
        const int wait_ms = static_cast<int>(engine.RunFrame());
        // The display backend may still own/import the current GPU buffer
        // while an input event is being dispatched. Capture only here, after
        // LVGL has completed and presented a frame; direct capture from the
        // key callback could restart recovery on the Adreno/KMS backend.
        if (hardware.screenshot_pending) {
            hardware.screenshot_pending = false;
            CaptureHardwareScreenshot(engine);
        }
        performance.Update();
        const DecryptionCompletion decrypt = engine.TakeDecryptionCompletion();
        if (decrypt != DecryptionCompletion::kNone) {
            {
                std::lock_guard<std::mutex> lock(gDecryptMutex);
                gDecryptResult = decrypt == DecryptionCompletion::kSuccess
                                     ? DecryptionResult::kSuccess
                                     : DecryptionResult::kSkipped;
                gDecryptResolved = true;
            }
            gDecryptCondition.notify_all();
        }
        const Action action = engine.TakeAction();
        if (action != Action::kNone) {
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "scene requested action %d",
                                static_cast<int>(action));
            // init will reboot or shut down immediately after this result is
            // dispatched.  Do not tear down a live Adreno scanout context on
            // the way out: the Qualcomm EGL driver aborts in that destructor,
            // preventing the power-control request from ever being sent.
            engine.AbandonForTerminalAction();
            return ToRunResult(action);
        }
        CaptureFrameIfRequested();
        input_event event{};
        int result = ev_get(&event, wait_ms);
        if (result >= 0 && HandleEvent(engine, performance, hardware, event))
            return RunResult::kUnexpectedExit;

        // Drain the queue so touch movement reaches LVGL before the next frame.
        while ((result = ev_get(&event, 0)) >= 0) {
            if (HandleEvent(engine, performance, hardware, event))
                return RunResult::kUnexpectedExit;
        }
    }
}

void StartRecoveryUi2Early(const DisplayMetrics& metrics) {
    std::lock_guard<std::mutex> lock(gEarlyMutex);
    if (gEarlyStarted) return;

    gBackendReady.store(false, std::memory_order_release);
    gDecryptRequested.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> decrypt_lock(gDecryptMutex);
        gDecryptResolved = false;
        gDecryptResult = DecryptionResult::kUnavailable;
    }
    gEarlyResult = RunResult::kUnexpectedExit;
    gDisplayMetrics = metrics;
    gEarlyStarted = true;
    gEarlyThread = std::thread([] {
        gEarlyResult = RunLoop(false, gDisplayMetrics);
        {
            std::lock_guard<std::mutex> lock(gDecryptMutex);
            if (gDecryptRequested.load(std::memory_order_acquire) &&
                !gDecryptResolved) {
                gDecryptResult = DecryptionResult::kUnavailable;
                gDecryptResolved = true;
            }
        }
        gDecryptCondition.notify_all();
    });
}

DecryptionResult RunRecoveryUi2Decryption(int credential_type,
                                          bool file_based, int user_id,
                                          int pattern_grid_size) {
    {
        std::lock_guard<std::mutex> lock(gEarlyMutex);
        if (!gEarlyStarted) return DecryptionResult::kUnavailable;
    }
    {
        std::lock_guard<std::mutex> lock(gDecryptMutex);
        gDecryptResolved = false;
        gDecryptResult = DecryptionResult::kUnavailable;
    }
    gCredentialType.store(credential_type, std::memory_order_release);
    gFileBasedEncryption.store(file_based, std::memory_order_release);
    gCryptoUserId.store(user_id, std::memory_order_release);
    gPatternGridSize.store(pattern_grid_size, std::memory_order_release);
    gDecryptRequested.store(true, std::memory_order_release);

    std::unique_lock<std::mutex> lock(gDecryptMutex);
    gDecryptCondition.wait(lock, [] { return gDecryptResolved; });
    return gDecryptResult;
}

RunResult RunRecoveryUi2(const DisplayMetrics& metrics) {
    std::thread early_thread;
    bool run_inline = false;
    {
        std::lock_guard<std::mutex> lock(gEarlyMutex);
        if (!gEarlyStarted) {
            gBackendReady.store(true, std::memory_order_release);
            run_inline = true;
        } else {
            gBackendReady.store(true, std::memory_order_release);
            early_thread = std::move(gEarlyThread);
        }
    }

    if (run_inline) return RunLoop(false, metrics);

    if (early_thread.joinable()) early_thread.join();

    std::lock_guard<std::mutex> lock(gEarlyMutex);
    gEarlyStarted = false;
    return gEarlyResult;
}

RunResult RunRecoveryUi2Fastboot(const DisplayMetrics& metrics) {
    return RunLoop(true, metrics);
}

}  // namespace recovery_ui2
