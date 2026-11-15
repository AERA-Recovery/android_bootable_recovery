// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>
#include "aeraui/core/runner.cpp"

using namespace std::chrono_literals;
using aeraui::RunResult;
static std::atomic<bool> release_ui{false};
static std::atomic<bool> cleanup_entered{false};
static std::atomic<unsigned> ui_runs{0};
static pthread_key_t graphics_key;
static RunResult selected;

// Model an EGL TLS destructor that stalls while releasing live scanout state.
static void GraphicsCleanup(void*) {
    cleanup_entered = true;
    if (selected != RunResult::kUnexpectedExit)
        for (;;) std::this_thread::sleep_for(10ms);
}

namespace aeraui {
RunResult TestRunLoop(bool, const DisplayMetrics&, bool) {
    ++ui_runs;
    pthread_setspecific(graphics_key, reinterpret_cast<void*>(1));
    while (!release_ui.load()) std::this_thread::sleep_for(1ms);
    return selected;
}
}

int main(int argc, char** argv) {
    if (argc != 3 || pthread_key_create(&graphics_key, GraphicsCleanup)) return 10;
    selected = static_cast<RunResult>(std::atoi(argv[1]));
    const bool precompleted = std::strcmp(argv[2], "precompleted") == 0;
    const bool decrypt = std::strcmp(argv[2], "decrypt") == 0;
    const bool inline_ui = std::strcmp(argv[2], "inline") == 0;
    const bool repeat = std::strcmp(argv[2], "repeat") == 0;
    if (precompleted || inline_ui) release_ui = true;
    if (!inline_ui) aeraui::StartAeraUiEarly();
    std::atomic<bool> decrypt_done{false};
    std::thread decrypt_waiter;
    if (decrypt) {
        decrypt_waiter = std::thread([&] {
            const auto result = aeraui::RunAeraUiDecryption(3, true, 0, 3);
            decrypt_done = result == aeraui::DecryptionResult::kTerminalAction;
        });
        while (!aeraui::gDecryptRequested.load()) std::this_thread::sleep_for(1ms);
    }
    if (precompleted) std::this_thread::sleep_for(20ms);
    std::atomic<bool> request_ready{false};
    RunResult received = RunResult::kUnexpectedExit;
    std::thread recovery([&] {
        received = aeraui::RunAeraUi();
        request_ready = true;
    });
    release_ui = true;
    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    while (!request_ready.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    if (!request_ready.load()) std::_Exit(20);
    if (received != selected) std::_Exit(21);
    const bool terminal = selected != RunResult::kUnexpectedExit;
    if (terminal && cleanup_entered.load()) std::_Exit(22);
    if (!terminal && !cleanup_entered.load()) std::_Exit(23);
    recovery.join();
    if (repeat) {
        // Startup's system-readonly page can consume the first result before
        // main calls gui_start(). A terminal action must survive that call.
        if (aeraui::RunAeraUi() != selected ||
            aeraui::RunAeraUiResume() != selected ||
            aeraui::RunAeraUiFastboot() != selected) std::_Exit(25);
        if (ui_runs.load() != 1) std::_Exit(26);
        if (aeraui::RunAeraUiDecryption(3, true, 0, 3) !=
            aeraui::DecryptionResult::kTerminalAction) std::_Exit(27);
    }
    if (decrypt) {
        decrypt_waiter.join();
        if (!decrypt_done.load()) std::_Exit(24);
    }
    // Terminal graphics threads deliberately live until init ends recovery.
    std::_Exit(0);
}
