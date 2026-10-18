/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace aeraui {
enum class WallpaperMode { kFill, kFit, kCenter, kStretch, kCustom };
struct WallpaperLayout {
  WallpaperMode mode = WallpaperMode::kFill;
  int zoom = 100;
  int x = 500, y = 500; // Position within the available crop, in thousandths.
};
inline bool ValidWallpaperLayout(const WallpaperLayout &value) {
  return static_cast<int>(value.mode) >= 0 && static_cast<int>(value.mode) <= 4 &&
      value.zoom >= 100 && value.zoom <= 400 && value.x >= 0 && value.x <= 1000 &&
      value.y >= 0 && value.y <= 1000;
}
inline std::string EncodeWallpaperLayout(const WallpaperLayout &value) {
  return "1 " + std::to_string(static_cast<int>(value.mode)) + " " +
      std::to_string(value.zoom) + " " + std::to_string(value.x) + " " + std::to_string(value.y);
}
inline WallpaperLayout DecodeWallpaperLayout(const std::string &text) {
  WallpaperLayout value;
  int version = 0, mode = 0;
  std::istringstream input(text);
  if (!(input >> version >> mode >> value.zoom >> value.x >> value.y) || version != 1)
    return {};
  value.mode = static_cast<WallpaperMode>(mode);
  input >> std::ws;
  return ValidWallpaperLayout(value) && input.eof() ? value : WallpaperLayout{};
}
struct WallpaperRect { int x, y, width, height; };
inline WallpaperRect WallpaperPlacement(int width, int height, int source_width,
    int source_height, const WallpaperLayout &settings, double native_scale = 1.0) {
  if (width <= 0 || height <= 0 || source_width <= 0 || source_height <= 0) return {};
  const auto value = ValidWallpaperLayout(settings) ? settings : WallpaperLayout{};
  const double fill = std::max(double(width) / source_width, double(height) / source_height);
  double scale = fill;
  if (value.mode == WallpaperMode::kFit)
    scale = std::min(double(width) / source_width, double(height) / source_height);
  else if (value.mode == WallpaperMode::kCenter) scale = native_scale;
  else if (value.mode == WallpaperMode::kCustom) scale *= value.zoom / 100.0;
  const int w = value.mode == WallpaperMode::kStretch ? width :
      std::max(1, static_cast<int>(std::lround(source_width * scale)));
  const int h = value.mode == WallpaperMode::kStretch ? height :
      std::max(1, static_cast<int>(std::lround(source_height * scale)));
  return {value.mode == WallpaperMode::kCustom ? int(-int64_t(w - width) * value.x / 1000) : (width - w) / 2,
          value.mode == WallpaperMode::kCustom ? int(-int64_t(h - height) * value.y / 1000) : (height - h) / 2, w, h};
}
}  // namespace aeraui
