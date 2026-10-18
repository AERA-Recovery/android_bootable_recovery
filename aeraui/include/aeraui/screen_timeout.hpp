/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
namespace aeraui {
inline bool ValidScreenTimeout(int seconds) {
  return seconds == 0 || (seconds >= 30 && seconds <= 300);
}
inline int ScreenTimeoutPosition(int seconds) {
  return seconds == 0 ? 10 : std::clamp((seconds + 15) / 30 - 1, 0, 9);
}
inline int ScreenTimeoutSeconds(int position) {
  position = std::clamp(position, 0, 10);
  return position == 10 ? 0 : (position + 1) * 30;
}
}  // namespace aeraui
