// Copyright (C) 2026 Recovery AERA UI contributors
// SPDX-License-Identifier: Apache-2.0
// Thread handoff excerpt from pinned AERA e04e0f5. Hardware is supplied by the test.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>
#include "aeraui/include/aeraui/runner.hpp"
namespace aeraui {
enum class Action { kNone, kRebootSystem, kRebootRecovery, kRebootBootloader, kRebootFastbootd, kPowerOff };
RunResult RunLoop(bool fastboot_mode = false, const DisplayMetrics& metrics = {}, bool resume_recovery = false);
RunResult TestRunLoop(bool fastboot_mode, const DisplayMetrics& metrics, bool resume_recovery);
namespace {
constexpr auto kLogTag = "AERA UI";
constexpr int ANDROID_LOG_INFO = 4;
int __android_log_print(int, const char*, const char*, ...) { return 0; }
std::atomic<bool> gBackendReady{false};
std::atomic<bool> gDecryptRequested{false};
std::atomic<int> gCredentialType{0};
std::atomic<int> gCryptoUserId{0};
std::atomic<int> gPatternGridSize{3};
std::atomic<bool> gFileBasedEncryption{false};
// 0 means no native UI, 1 recovery, 2 userspace fastboot.
std::atomic<int> gUiMode{0};
std::atomic<int> gModeTransitionRequest{0};
std::atomic<bool> gModeTransitionBusy{false};
std::mutex gEarlyMutex;
std::mutex gDecryptMutex;
std::condition_variable gDecryptCondition;
std::thread gEarlyThread;
bool gEarlyStarted = false;
bool gDecryptResolved = false;
DecryptionResult gDecryptResult = DecryptionResult::kUnavailable;
RunResult gEarlyResult = RunResult::kUnexpectedExit;
DisplayMetrics gDisplayMetrics{};

void CaptureFrameIfRequested() {
    const int result = 0;
    __android_log_print(result == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_INFO, kLogTag,
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

RunResult RunLoop(bool fastboot, const DisplayMetrics& metrics, bool resume) {
    for (;;) {
        return TestRunLoop(fastboot, metrics, resume);
    }
}

void StartAeraUiEarly(const DisplayMetrics& metrics) {
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
                switch (gEarlyResult) {
                    case RunResult::kRebootSystem:
                    case RunResult::kRebootRecovery:
                    case RunResult::kRebootBootloader:
                    case RunResult::kRebootFastbootd:
                    case RunResult::kPowerOff:
                        gDecryptResult = DecryptionResult::kTerminalAction;
                        break;
                    default:
                        gDecryptResult = DecryptionResult::kUnavailable;
                        break;
                }
                gDecryptResolved = true;
            }
        }
        gDecryptCondition.notify_all();
    });
}

DecryptionResult RunAeraUiDecryption(int credential_type,
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

RunResult RunAeraUi(const DisplayMetrics& metrics) {
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

RunResult RunAeraUiResume(const DisplayMetrics& metrics) {
    return RunLoop(false, metrics, true);
}

RunResult RunAeraUiFastboot(const DisplayMetrics& metrics) {
    return RunLoop(true, metrics);
}

}  // namespace aeraui
