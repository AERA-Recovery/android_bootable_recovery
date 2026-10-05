/* SPDX-License-Identifier: Apache-2.0 */
#include "file_picker.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace aeraui::file_picker;

int main() {
  const fs::path base = fs::temp_directory_path() /
      ("aera-file-picker-" + std::to_string(getpid()));
  fs::create_directories(base / "Music");
  fs::create_directories(base / "docs");
  std::ofstream(base / "b.ZIP") << "zip";
  std::ofstream(base / "a.txt") << "text";
  std::ofstream(base / ".hidden") << "x";
  fs::create_symlink(base / "docs", base / "link");
  const std::string root = base.string();
  std::string error;

  Request any;
  auto entries = List(any, root, false, error);
  assert(error.empty());
  // Folders first (a followed link is a folder), then names ignoring case.
  assert(entries.size() == 5);
  assert(entries[0].name == "docs" && entries[0].directory);
  assert(entries[1].name == "link" && entries[1].directory);
  assert(entries[2].name == "Music");
  assert(entries[3].name == "a.txt" && entries[3].size == 4);
  assert(entries[4].name == "b.ZIP");
  assert(List(any, root, true, error).size() == 6);

  Request zips;
  zips.extensions = {"zip"};
  entries = List(zips, root, false, error);
  assert(entries.size() == 4 && entries[3].name == "b.ZIP");

  Request folders;
  folders.mode = Mode::kFolder;
  assert(List(folders, root, false, error).size() == 3);

  Request confined;
  confined.roots = {{"Base", root}};
  entries = List(confined, root, false, error);
  // Links are skipped when confined.
  assert(entries.size() == 4);
  assert(Allowed(confined, root + "/docs"));
  assert(!Allowed(confined, "/"));
  assert(!Allowed(confined, root + "/../x"));
  assert(!Allowed(confined, root + "x"));
  assert(Parent(confined, root).empty());
  assert(Parent(confined, root + "/docs") == root);
  assert(List(confined, "/", false, error).empty() && !error.empty());

  assert(Parent(any, "/") == "/");
  assert(Parent(any, "/sdcard") == "/");
  assert(Parent(any, "/sdcard/Download/") == "/sdcard");
  assert(!Allowed(any, "relative"));
  assert(!Allowed(any, "/sdcard/.."));
  assert(Join("/", "sdcard") == "/sdcard");

  Request limited;
  limited.max_path = root.size() + 6;
  entries = List(limited, root, false, error);
  // "/docs" and "/link" fit, longer names are left out.
  assert(entries.size() == 2);

  fs::remove_all(base);
  return 0;
}
