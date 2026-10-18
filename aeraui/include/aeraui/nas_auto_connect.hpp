/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <string>

namespace aeraui {
class NasAutoConnectPolicy {
 public:
  bool Ready(const std::string &network, const std::string &profile,
             bool enabled, bool mounted, uint32_t now) {
    if (!enabled || network.empty()) {
      key_.clear(); attempts_ = 0; suppressed_ = false;
      return false;
    }
    const std::string key = network + "\n" + profile;
    if (key != key_) {
      if (!key_.empty()) suppressed_ = false;
      key_ = key; attempts_ = 0;
    }
    if (mounted || suppressed_ || attempts_ >= 3 ||
        (attempts_ && now - last_attempt_ < 30000)) return false;
    ++attempts_; last_attempt_ = now;
    return true;
  }
  void Suppress() { suppressed_ = true; }
 private:
  std::string key_;
  unsigned attempts_ = 0;
  uint32_t last_attempt_ = 0;
  bool suppressed_ = false;
};
}  // namespace aeraui
