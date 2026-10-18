/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <mutex>
#include <cstdint>
#include <unistd.h>

namespace aera::backup {
struct Cancellation {
  uint64_t token = 0;
  bool active = false, restore = false, requested = false;
};
class Control {
 public:
  uint64_t Begin(bool restore) {
    std::lock_guard<std::mutex> guard(lock_);
    ++state_.token; state_.active = true; state_.restore = restore;
    state_.requested = false; requested_.store(false); active_.store(true);
    return state_.token;
  }
  void Finish(uint64_t token) {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_.token != token) return;
    state_.active = false; active_.store(false);
  }
  bool Request(uint64_t token, bool acknowledge_restore) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!state_.active || state_.token != token ||
        (state_.restore && !acknowledge_restore)) return false;
    state_.requested = true; requested_.store(true); return true;
  }
  Cancellation Snapshot() {
    std::lock_guard<std::mutex> guard(lock_); return state_;
  }
  bool Active() const { return active_.load(); }
  bool Requested() const { return active_.load() && requested_.load(); }
 private:
  std::mutex lock_;
  Cancellation state_;
  std::atomic<bool> active_{false}, requested_{false};
};
inline Control &Operations() { static Control control; return control; }
}  // namespace aera::backup

namespace aera::pc {
#ifndef BUILD_TWRPTAR_MAIN
bool BackupOperationActive();
bool BackupCancelRequested();
#else
inline bool BackupOperationActive() { return false; }
inline bool BackupCancelRequested() { return false; }
#endif

// Only the parent watches cancellation. Stop before reaping to prevent PID
// reuse, and signal the child's own group so compressors cannot linger.
class BackupChildCancellation {
 public:
  explicit BackupChildCancellation(pid_t child) {
    if (child <= 0 || !BackupOperationActive()) return;
    setpgid(child, child);
    watcher_ = std::thread([this, child] {
      while (!stop_.load()) {
        if (BackupCancelRequested()) {
          kill(-child, SIGTERM);
          for (unsigned i = 0; i < 20 && !stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (!stop_.load()) kill(-child, SIGKILL);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  ~BackupChildCancellation() { Stop(); }
  void Stop() { stop_.store(true); if (watcher_.joinable()) watcher_.join(); }
 private:
  std::atomic<bool> stop_{false};
  std::thread watcher_;
};
}  // namespace aera::pc
