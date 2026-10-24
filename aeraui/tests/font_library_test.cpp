/* SPDX-License-Identifier: Apache-2.0 */
#include "fonts/font_library.hpp"
#include "localization/fonts/custom_font_storage.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

int main() {
  using namespace aeraui::fonts;
  char fixture[] = "/tmp/aera-font-library-test-XXXXXX";
  assert(mkdtemp(fixture));
  const std::filesystem::path root(fixture);
  std::filesystem::create_directories(root / "Fonts/Library");
  const auto source = root / "source.ttf";
  { std::ofstream f(source); f << "first font data"; }
  FontLibrary library((root / "Fonts/Library").string());
  SavedFont metadata;
  metadata.family = "Roboto"; metadata.provider_id = "roboto";
  metadata.category = "sans-serif"; metadata.license = "OFL-1.1";
  metadata.style = "normal"; metadata.subset = "latin"; metadata.weight = 400;
  metadata.url = "https://cdn.jsdelivr.net/fontsource/fonts/roboto@5.3.0/latin-400-normal.ttf";
  std::string first, second, repeated, error;
  assert(library.Save(source.string(), metadata, first, &error));
  assert(library.Save(source.string(), metadata, repeated, &error));
  assert(first == repeated && library.List().size() == 1);
  { std::ofstream f(source); f << "second font data"; }
  metadata.weight = 700;
  assert(library.Save(source.string(), metadata, second, &error));
  assert(first != second && library.List().size() == 2);
  std::filesystem::remove(source);
  SavedFont loaded;
  assert(FontLibrary((root / "Fonts/Library").string()).Find(first, loaded));
  assert(loaded.family == "Roboto" && loaded.weight == 400);
  assert(std::filesystem::exists(loaded.path));
  CustomFontStorage manual((root / "Fonts").string());
  const auto manual_id = manual.Import(loaded.path, &error);
  assert(!manual_id.empty());
  manual.Prune("");
  assert(library.List().size() == 2 && library.Find(first, loaded));
  assert(!library.Find("../../elsewhere", loaded));
  assert(!library.Remove("../../elsewhere", &error));
  SavedFont invalid = metadata; invalid.family = "bad\nname";
  assert(!library.Save(loaded.path, invalid, repeated, &error));
  assert(library.List().size() == 2);
  assert(library.Remove(first, &error));
  assert(!library.Find(first, loaded));
  assert(library.Find(second, loaded));
  assert(library.List().size() == 1);
  const auto metadata_path = root / "Fonts/Library" / (second + ".json");
  std::filesystem::remove(metadata_path);
  std::filesystem::create_symlink(root / "unrelated.json", metadata_path);
  assert(!library.Find(second, loaded));
  assert(library.List().empty());
  std::filesystem::remove_all(root);
  std::cout << "Font library: persistent variants, deduplication, manual-import isolation and deletion passed\n";
}
