/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recovery_ui2 {

// Resolves and rasterizes Android APK icon resources into a square BGRA image.
// Supports bitmap, WebP, vector, adaptive, layer-list and simple shape icons.
bool DecodeAndroidIcon(const std::string &apk,
                       const std::vector<std::string> &resources,
                       uint32_t size, std::vector<uint8_t> &pixels);

}  // namespace recovery_ui2
