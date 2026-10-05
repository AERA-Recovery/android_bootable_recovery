/* SPDX-License-Identifier: Apache-2.0 */
// A full-screen file picker over the current scene, for any feature that
// asks the user for a file, several files, a folder or a place to save. It
// browses like Files: it starts in the current storage, Parent folder goes
// up to /, and Storage and Root jump to a volume or the root. A caller may
// confine it to a set of storage roots instead (Telegram does, since its
// worker only sees those).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <lvgl.h>

namespace aeraui::file_picker {

enum class Mode {
  kFile,    // one existing file
  kFiles,   // one or more existing files
  kFolder,  // one folder
  kSave,    // a new or existing file name in a folder
};

struct Request {
  Mode mode = Mode::kFile;
  // Heading, translated like any label ("Choose a file" when empty).
  std::string title;
  // Folder to open in; the current storage when empty or unreadable.
  std::string start;
  // File extensions to show, lower case without the dot ("zip"); all files
  // when empty. Folders are always shown.
  std::vector<std::string> extensions;
  // The name to offer in kSave.
  std::string suggested_name;
  // When not empty, only these folders and what is below them can be
  // browsed or chosen, symbolic links are skipped, and Parent folder at a
  // root returns to the list of roots. Each entry is {name, path}.
  std::vector<std::pair<std::string, std::string>> roots;
  // Paths this long or longer are not offered (0: no limit), for callers
  // that pass the path through a fixed-size message.
  size_t max_path = 0;
  // The most files kFiles lets the user select (0: no limit).
  size_t max_count = 0;
};

// Called once with the chosen absolute paths (never empty).
using Chosen = std::function<void(std::vector<std::string>)>;
// Called once when the picker closes without a choice (Back, the close
// button, or Dismiss()). Not called by Forget().
using Cancelled = std::function<void()>;

// Opens the picker on `screen` and returns its overlay. The overlay is a
// modal: AERA's Back closes it, and scenes that route touches elsewhere
// while a modal shows (pixel plugins) leave it the input.
lv_obj_t *Show(lv_obj_t *screen, Request request, Chosen chosen,
               Cancelled cancelled = {});

// Closes `picker` as a cancellation.
void Dismiss(lv_obj_t *picker);

// Drops both callbacks of `picker` without calling them, for a caller that
// is going away while the picker is still open. The picker stays open
// until it is closed or its screen is deleted.
void Forget(lv_obj_t *picker);

// The entries of `directory` a picker for `request` shows; exposed for
// tests. Folders first, then by name ignoring case.
struct Entry {
  std::string name;
  std::string path;
  bool directory = false;
  uint64_t size = 0;
};
std::vector<Entry> List(const Request &request, const std::string &directory,
                        bool hidden_files, std::string &error);

// The folder Parent folder leads to, or "" for the list of roots of a
// confined picker.
std::string Parent(const Request &request, const std::string &directory);

// Whether `path` may be browsed or chosen under `request`.
bool Allowed(const Request &request, const std::string &path);

// `name` inside `directory`.
std::string Join(const std::string &directory, const std::string &name);

}  // namespace aeraui::file_picker
