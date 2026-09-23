#include "aera_wifi_dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <sys/stat.h>

#include "data.hpp"
#include "aeraui/platform/aera_ui_host.h"
#include "wlan.hpp"

namespace {

constexpr uint32_t kStart = 1U << 0;
constexpr uint32_t kStop = 1U << 1;
constexpr uint32_t kRefresh = 1U << 2;
constexpr uint32_t kRestore = 1U << 3;
constexpr auto kLinkPollInterval = std::chrono::seconds(2);
constexpr auto kRadioSettleDelay = std::chrono::milliseconds(1500);

std::mutex g_lock;
std::condition_variable g_wakeup;
std::once_flag g_start_once;
uint32_t g_pending = 0;
std::atomic<bool> g_active{false};

bool RadioEnabled() {
    return DataManager::GetIntValue("tw_wlan_enabled") == 1;
}

bool LinkConnected() {
    return DataManager::GetIntValue("tw_wlan_connected") == 1;
}

void Publish(const char* state) {
    DataManager::SetValue("wlan_state", state);
}

void PublishRestingState() {
    if (!RadioEnabled()) {
        Publish("disabled");
    } else {
        Publish(LinkConnected() ? "connected" : "enabled");
    }
}

void Toast(const char* message, int duration) {
    gui_print("I:Wi-Fi: %s\n", message);
    (void)duration;
}

bool ScanProducedResults() {
    struct stat info {};
    return stat("/tmp/wlan/list.txt", &info) == 0 && info.st_size > 0;
}

void RefreshAccessPoints() {
    std::this_thread::sleep_for(kRadioSettleDelay);
    DataManager::SetValue("wlan_refreshing", 1);
    const bool scanned = Wlan::Scan();
    DataManager::SetValue("wlan_refreshing", 0);
    Wlan::RefreshWlanPageIfShown();

    if (!scanned || !ScanProducedResults())
        Toast("[warning]No networks found", 120);
}

void StartRadio() {
    Publish("enabling");
    if (!Wlan::Enable() || !RadioEnabled()) {
        Publish("error");
        Toast("[error]Failed to enable WiFi", 150);
        return;
    }

    PublishRestingState();
    RefreshAccessPoints();
}

void StopRadio() {
    Publish("disabling");
    Wlan::Disable();
    Publish("disabled");
}

void RestoreSession() {
    if (DataManager::GetIntValue("of_wlan_auto_enable") != 1)
        return;

    if (!RadioEnabled()) {
        Publish("enabling");
        Wlan::Enable();
    }

    if (!RadioEnabled()) {
        Publish("error");
        Toast("[error]Failed to enable WiFi", 150);
        return;
    }

    PublishRestingState();
    RefreshAccessPoints();

    if (DataManager::GetIntValue("of_wlan_auto_connect") != 1 || LinkConnected())
        return;

    const std::string ssid = DataManager::GetStrValue("of_wlan_last_ssid");
    if (ssid.empty())
        return;

    DataManager::SetValue("wlanselectedid", ssid);
    Wlan::ConnectSaved();
    PublishRestingState();
    Wlan::RefreshWlanPageIfShown();
}

uint32_t TakeNextRequest() {
    if (g_pending & kRestore) {
        g_pending &= ~kRestore;
        return kRestore;
    }
    if (g_pending & kStop) {
        g_pending &= ~kStop;
        return kStop;
    }
    if (g_pending & kStart) {
        g_pending &= ~kStart;
        return kStart;
    }
    if (g_pending & kRefresh) {
        g_pending &= ~kRefresh;
        return kRefresh;
    }
    return 0;
}

void RunRequest(uint32_t request) {
    if (request == kRestore) {
        RestoreSession();
    } else if (request == kStop) {
        StopRadio();
    } else if (request == kStart) {
        StartRadio();
    } else if (request == kRefresh) {
        Wlan::Info();
    }
}

void DispatchLoop() {
    for (;;) {
        uint32_t request = 0;
        {
            std::unique_lock<std::mutex> guard(g_lock);
            if (g_pending == 0) {
                if (RadioEnabled()) {
                    g_wakeup.wait_for(guard, kLinkPollInterval,
                                      [] { return g_pending != 0; });
                } else {
                    g_wakeup.wait(guard, [] { return g_pending != 0; });
                }
            }

            request = TakeNextRequest();
        }

        if (request == 0) {
            if (RadioEnabled())
                Wlan::RefreshInfoIfIdle();
            continue;
        }

        g_active.store(true, std::memory_order_release);
        RunRequest(request);
        g_active.store(false, std::memory_order_release);
    }
}

uint32_t RequestBit(AeraWifiDispatcher::Request request) {
    switch (request) {
        case AeraWifiDispatcher::Request::StartRadio:
            return kStart;
        case AeraWifiDispatcher::Request::StopRadio:
            return kStop;
        case AeraWifiDispatcher::Request::RefreshStatus:
            return kRefresh;
        case AeraWifiDispatcher::Request::RestoreSession:
            return kRestore;
    }
    return 0;
}

}  // namespace

void AeraWifiDispatcher::Submit(Request request) {
    std::call_once(g_start_once, [] { std::thread(DispatchLoop).detach(); });

    {
        std::lock_guard<std::mutex> guard(g_lock);
        const uint32_t bit = RequestBit(request);
        if (bit == kStart || bit == kStop)
            g_pending &= ~(kStart | kStop | kRestore);
        g_pending |= bit;
    }
    g_wakeup.notify_one();
}

bool AeraWifiDispatcher::Active() {
    return g_active.load(std::memory_order_acquire);
}
