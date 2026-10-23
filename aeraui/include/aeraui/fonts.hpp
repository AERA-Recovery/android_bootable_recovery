/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string>

namespace aeraui::fonts {
struct SavedFont {
  std::string id, family, provider_id, category, license, subset, style, url, path;
  int weight = 400;
};
}
