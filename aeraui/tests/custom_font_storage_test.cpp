/* SPDX-License-Identifier: Apache-2.0 */
#include "localization/fonts/custom_font_storage.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace aeraui::fonts;

int main() {
  char temporary[] = "/tmp/aera-custom-font-test-XXXXXX";
  assert(mkdtemp(temporary));
  const fs::path root = temporary;
  fs::create_directories(root / "Fonts");
  fs::create_directories(root / "early" / "Fonts");
  CustomFontStorage storage((root / "Fonts").string());
  CustomFontStorage early((root / "early" / "Fonts").string());
  const auto source = (root / "source.ttf").string();
  std::ofstream(source) << "first font bytes";
  std::string error;
  const auto first = storage.Import(source, &error);
  assert(CustomFontId(first) && storage.Has(first));
  assert(storage.Import(source, &error) == first);
  assert(storage.List().size() == 1);
  assert(!CustomFontId("../source") && storage.Path("../source").empty());
  assert(early.Import(storage.Path(first), &error) == first);
  assert(early.Has(first));
  fs::remove(source);
  assert(storage.Has(first) && early.Has(first));
  std::ofstream(source) << "second font bytes";
  const auto second = storage.Import(source, &error);
  assert(CustomFontId(second) && first != second);
  assert(storage.Has(first) && storage.Has(second));
  std::ofstream(root / "Fonts" / "user-file.ttf") << "not an imported font";
  storage.Prune(second);
  assert(!storage.Has(first) && storage.Has(second));
  assert(fs::exists(root / "Fonts" / "user-file.ttf"));
  assert(early.Import(storage.Path(second), &error) == second);
  early.Prune(second);
  assert(!early.Has(first) && early.Has(second));
  early.Prune("");
  assert(early.List().empty());
  assert(storage.Import((root / "missing.ttf").string(), &error).empty());
  std::ofstream(source, std::ios::trunc);
  assert(storage.Import(source, &error).empty());
  const int fd = open(source.c_str(), O_WRONLY);
  assert(fd >= 0 && ftruncate(fd, CustomFontStorage::kMaximumBytes + 1) == 0);
  close(fd);
  assert(storage.Import(source, &error).empty());
  assert(storage.Has(second));
  fs::remove(source);
  fs::create_symlink(storage.Path(second), source);
  assert(storage.Import(source, &error).empty());
  for (const auto& entry : fs::directory_iterator(root / "Fonts"))
    assert(entry.path().filename().string().find(".font-") != 0);
  fs::remove_all(root);
  puts("Custom fonts: stable IDs, replacement, early copy, cleanup and failed imports passed");
}
