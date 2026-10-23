/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "aeraui/fonts.hpp"
#include <string>
#include <vector>
#include <utility>

namespace aeraui::fonts {
class FontLibrary {
 public:
  explicit FontLibrary(std::string directory) : directory_(std::move(directory)) {}
  bool Find(const std::string& id, SavedFont& font) const;
  std::vector<SavedFont> List() const;
  bool Save(const std::string& source, const SavedFont& metadata,
            std::string& id, std::string* error) const;
  bool Remove(const std::string& id, std::string* error) const;
 private:
  std::string directory_;
};
}
