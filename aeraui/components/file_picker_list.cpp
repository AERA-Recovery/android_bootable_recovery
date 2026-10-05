/* SPDX-License-Identifier: Apache-2.0 */
// The file picker's folder model, apart from its LVGL view so host tests can
// run it.
#include "file_picker.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

namespace aeraui::file_picker {
namespace {

bool Under(const std::string &path, const std::string &root) {
  return path == root || (path.compare(0, root.size(), root) == 0 &&
                          path.size() > root.size() &&
                          path[root.size()] == '/');
}

std::string Extension(const std::string &name) {
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return {};
  std::string extension = name.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return extension;
}
}  // namespace

std::string Join(const std::string &directory, const std::string &name) {
  return (directory == "/" ? "" : directory) + "/" + name;
}

bool Allowed(const Request &request, const std::string &path) {
  if (path.empty() || path[0] != '/' ||
      path.find("/../") != std::string::npos ||
      (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0))
    return false;
  if (request.roots.empty()) return true;
  return std::any_of(request.roots.begin(), request.roots.end(),
                     [&](const auto &root) { return Under(path, root.second); });
}

std::string Parent(const Request &request, const std::string &directory) {
  std::string path = directory;
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  if (!request.roots.empty()) {
    for (const auto &root : request.roots)
      if (path == root.second) return {};
  }
  const auto slash = path.find_last_of('/');
  std::string parent = slash == 0 || slash == std::string::npos
      ? "/" : path.substr(0, slash);
  if (!request.roots.empty() && !Allowed(request, parent)) return {};
  return parent;
}

std::vector<Entry> List(const Request &request, const std::string &directory,
                        bool hidden_files, std::string &error) {
  std::vector<Entry> entries;
  if (!Allowed(request, directory)) {
    error = strerror(EACCES);
    return entries;
  }
  DIR *folder = opendir(directory.c_str());
  if (!folder) {
    error = strerror(errno);
    return entries;
  }
  const bool confined = !request.roots.empty();
  while (auto *item = readdir(folder)) {
    if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
    if (item->d_name[0] == '.' && !hidden_files) continue;
    std::string path = Join(directory, item->d_name);
    if (request.max_path && path.size() >= request.max_path) continue;
    struct stat info{};
    // A confined picker never follows a link out of its roots.
    if ((confined ? lstat(path.c_str(), &info) : stat(path.c_str(), &info)) != 0)
      continue;
    const bool is_directory = S_ISDIR(info.st_mode);
    if (!is_directory && !S_ISREG(info.st_mode)) continue;
    if (!is_directory && request.mode == Mode::kFolder) continue;
    if (!is_directory && !request.extensions.empty() &&
        std::find(request.extensions.begin(), request.extensions.end(),
                  Extension(item->d_name)) == request.extensions.end())
      continue;
    entries.push_back({item->d_name, std::move(path), is_directory,
                       static_cast<uint64_t>(std::max<off_t>(0, info.st_size))});
  }
  closedir(folder);
  std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
    if (a.directory != b.directory) return a.directory;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return entries;
}

}  // namespace aeraui::file_picker
