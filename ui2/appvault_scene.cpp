/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <androidfw/Asset.h>
#include <androidfw/AssetManager.h>
#include <androidfw/ResourceTypes.h>
#include <json/json.h>
#include <utils/String8.h>

#include "android_icon.hpp"
#include "browser/runtime.hpp"
#include "phone_keyboard.hpp"
#include "ui_components.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace recovery_ui2 {
namespace {
using namespace design;
using namespace widgets;

constexpr char kPluginId[] = "appvault";
constexpr char kPluginType[] = "app-runtime";
constexpr char kPluginEntry[] = "appvault";
constexpr char kDefaultRepository[] = "/sdcard/AERA/AppBackupVault";
constexpr char kTag[] = "aera-appvault";
constexpr uint32_t kLabelAttribute = 0x01010001;
constexpr uint32_t kIconAttribute = 0x01010002;
constexpr uint32_t kRoundIconAttribute = 0x0101052c;
constexpr uint32_t kIconPixels = 76;

enum class Work { kNone, kInitialize, kBackup, kRefresh, kRestore };

struct AppIcon {
  std::vector<uint8_t> pixels;
  lv_image_dsc_t descriptor{};
};

struct App {
  std::string package;
  std::string name;
  std::string apk;
  std::vector<std::string> paths;
  std::unique_ptr<AppIcon> icon;
  bool system = true;
  bool selected = false;
};

struct SnapshotApp {
  std::string package;
  std::string name;
  std::vector<std::string> paths;
  bool selected = true;
};

struct Snapshot {
  std::string id;
  std::string time;
  uint64_t bytes = 0;
  std::vector<SnapshotApp> apps;
};

struct State {
  lv_obj_t *screen = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *repository_label = nullptr;
  lv_obj_t *password = nullptr;
  lv_obj_t *selected = nullptr;
  lv_obj_t *system_filter_label = nullptr;
  lv_obj_t *select_all_label = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *initialize = nullptr;
  lv_obj_t *backup = nullptr;
  lv_obj_t *refresh_button = nullptr;
  lv_obj_t *restore = nullptr;
  lv_timer_t *timer = nullptr;
  web::Preparation preparation;
  std::thread prepare_thread;
  std::thread worker;
  std::atomic<bool> preparation_handled{false};
  std::atomic<bool> metadata_done{false};
  std::atomic<bool> busy{false};
  std::atomic<bool> done{false};
  std::atomic<bool> cancel{false};
  std::atomic<pid_t> child{-1};
  std::atomic<unsigned> work_progress{0};
  std::atomic<bool> work_success{false};
  std::mutex result_mutex;
  std::string runtime;
  std::string repository = kDefaultRepository;
  std::string password_value;
  std::string result;
  std::string latest_id;
  std::string latest_time;
  std::string restore_snapshot;
  unsigned snapshots = 0;
  bool browse_after_refresh = false;
  bool show_system = false;
  Work work = Work::kNone;
  std::vector<App> apps;
  std::vector<Snapshot> snapshot_items;
};

bool SafePackage(const char *name) {
  if (!name || !*name || strlen(name) > 255) return false;
  if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
  for (const unsigned char c : std::string(name))
    if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
  return true;
}

bool Directory(const std::string &path) {
  struct stat info{};
  return !lstat(path.c_str(), &info) && S_ISDIR(info.st_mode);
}

std::set<std::string> UserPackages() {
  std::set<std::string> result;
  FILE *file = fopen("/data/system/packages.list", "re");
  if (!file) return result;
  char *line = nullptr;
  size_t capacity = 0;
  while (getline(&line, &capacity, file) >= 0) {
    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (stream >> field) fields.push_back(std::move(field));
    // Android 16's package list carries SELinux package provenance in field
    // five. System packages contain partition=system/product/vendor/etc and
    // end in @system; Play Store, sideloaded and adb-installed apps do not.
    if (fields.size() >= 6 && SafePackage(fields[0].c_str()) &&
        fields[4].find("partition=") == std::string::npos &&
        fields.back() != "@system") result.insert(fields[0]);
  }
  free(line);
  fclose(file);
  return result;
}

void AddCodePath(std::map<std::string, std::string> &result,
                 const std::string &path, const std::string &leaf) {
  const size_t split = leaf.find('-');
  const std::string package = split == std::string::npos
      ? leaf : leaf.substr(0, split);
  if (SafePackage(package.c_str()) && Directory(path) &&
      access((path + "/base.apk").c_str(), R_OK) == 0)
    result[package] = path;
}

std::map<std::string, std::string> DataAppCodePaths() {
  std::map<std::string, std::string> result;
  DIR *root = opendir("/data/app");
  if (!root) return result;
  while (const dirent *outer = readdir(root)) {
    if (!strcmp(outer->d_name, ".") || !strcmp(outer->d_name, "..")) continue;
    const std::string outer_path = std::string("/data/app/") + outer->d_name;
    if (!Directory(outer_path)) continue;
    AddCodePath(result, outer_path, outer->d_name);
    DIR *container = opendir(outer_path.c_str());
    if (!container) continue;
    while (const dirent *inner = readdir(container)) {
      if (!strcmp(inner->d_name, ".") || !strcmp(inner->d_name, "..")) continue;
      AddCodePath(result, outer_path + "/" + inner->d_name, inner->d_name);
    }
    closedir(container);
  }
  closedir(root);
  return result;
}

std::vector<std::string> SiblingApks(const std::string &base) {
  std::vector<std::string> paths;
  const size_t slash = base.find_last_of('/');
  if (slash == std::string::npos) return paths;
  const std::string directory = base.substr(0, slash);
  DIR *handle = opendir(directory.c_str());
  if (!handle) return paths;
  while (const dirent *entry = readdir(handle)) {
    const std::string name = entry->d_name;
    if (name == "." || name == ".." || name == "base.apk" ||
        name.size() < 4 || name.compare(name.size() - 4, 4, ".apk")) continue;
    paths.push_back(directory + "/" + name);
  }
  closedir(handle);
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string FriendlyPackageName(const std::string &package) {
  const size_t dot = package.find_last_of('.');
  std::string name = package.substr(dot == std::string::npos ? 0 : dot + 1);
  for (char &c : name) {
    if (c == '_' || c == '-') c = ' ';
  }
  bool capitalize = true;
  for (char &c : name) {
    if (capitalize && std::isalpha(static_cast<unsigned char>(c)))
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    capitalize = c == ' ';
  }
  return name.empty() ? package : name;
}

android::String8 ResolvedAttribute(const android::ResTable &resources,
                                   const android::ResXMLTree &tree,
                                   uint32_t attribute) {
  ssize_t index = -1;
  for (size_t i = 0; i < tree.getAttributeCount(); ++i) {
    if (tree.getAttributeNameResID(i) == attribute) {
      index = static_cast<ssize_t>(i);
      break;
    }
  }
  if (index < 0) return {};
  android::Res_value value{};
  if (tree.getAttributeValue(index, &value) == android::BAD_TYPE) return {};
  if (value.dataType == android::Res_value::TYPE_STRING) {
    size_t length = 0;
    const char16_t *text = tree.getAttributeStringValue(index, &length);
    return text ? android::String8(text, length) : android::String8();
  }
  const ssize_t block = resources.resolveReference(&value, 0);
  if (block < 0 || value.dataType != android::Res_value::TYPE_STRING) return {};
  size_t length = 0;
  const char16_t *text = resources.valueToString(
      &value, static_cast<size_t>(block), nullptr, &length);
  return text ? android::String8(text, length) : android::String8();
}

std::unique_ptr<AppIcon> LoadIcon(const std::string &apk,
                                  const std::vector<std::string> &resources) {
  auto icon = std::make_unique<AppIcon>();
  if (!DecodeAndroidIcon(apk, resources, kIconPixels, icon->pixels))
    return nullptr;
  auto &descriptor = icon->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = kIconPixels;
  descriptor.header.h = kIconPixels;
  descriptor.header.stride = kIconPixels * 4;
  descriptor.data_size = icon->pixels.size();
  descriptor.data = icon->pixels.data();
  return icon;
}

void ReadAppMetadata(App &app) {
  if (app.apk.empty()) return;
  android::AssetManager assets;
  int32_t cookie = 0;
  if (!assets.addAssetPath(android::String8(app.apk.c_str()), &cookie)) return;
  for (const auto &split : SiblingApks(app.apk)) {
    int32_t split_cookie = 0;
    assets.addAssetPath(android::String8(split.c_str()), &split_cookie);
  }
  android::ResTable_config configuration{};
  configuration.density = 480;
  assets.setConfiguration(configuration, "en-US");
  std::unique_ptr<android::Asset> manifest(assets.openNonAsset(
      cookie, "AndroidManifest.xml", android::Asset::ACCESS_BUFFER));
  if (!manifest) return;
  android::ResXMLTree tree;
  if (tree.setTo(manifest->getBuffer(true), manifest->getLength()) != android::NO_ERROR)
    return;
  while (true) {
    const auto event = tree.next();
    if (event == android::ResXMLTree::END_DOCUMENT ||
        event == android::ResXMLTree::BAD_DOCUMENT) break;
    if (event != android::ResXMLTree::START_TAG) continue;
    size_t length = 0;
    const char16_t *tag_text = tree.getElementName(&length);
    if (!tag_text || android::String8(tag_text, length) != "application") continue;
    const android::ResTable &resources = assets.getResources();
    const android::String8 label = ResolvedAttribute(resources, tree, kLabelAttribute);
    if (!label.empty()) app.name = label.c_str();
    std::vector<std::string> icons;
    const android::String8 icon = ResolvedAttribute(resources, tree, kIconAttribute);
    const android::String8 round = ResolvedAttribute(resources, tree, kRoundIconAttribute);
    if (!icon.empty()) icons.emplace_back(icon.c_str());
    if (!round.empty() && round != icon) icons.emplace_back(round.c_str());
    app.icon = LoadIcon(app.apk, icons);
    break;
  }
}

std::vector<App> DiscoverApps(const std::atomic<bool> &cancel) {
  std::vector<App> apps;
  DIR *directory = opendir("/data/user/0");
  if (!directory) return apps;
  const auto user_packages = UserPackages();
  const auto code_paths = DataAppCodePaths();
  while (const dirent *entry = readdir(directory)) {
    if (!SafePackage(entry->d_name)) continue;
    App app;
    app.package = entry->d_name;
    app.name = FriendlyPackageName(app.package);
    const auto code = code_paths.find(app.package);
    app.system = user_packages.count(app.package) == 0;
    app.selected = !app.system;
    const std::vector<std::string> candidates = {
        "/data/user/0/" + app.package,
        "/data/user_de/0/" + app.package,
        "/data/media/0/Android/data/" + app.package,
        "/data/media/0/Android/obb/" + app.package,
        "/data/media/0/Android/media/" + app.package};
    for (const auto &path : candidates)
      if (Directory(path)) app.paths.push_back(path);
    if (code != code_paths.end()) {
      app.paths.push_back(code->second);
      app.apk = code->second + "/base.apk";
    }
    if (!app.paths.empty()) apps.push_back(std::move(app));
  }
  closedir(directory);
  for (auto &app : apps) {
    if (cancel.load(std::memory_order_relaxed)) break;
    ReadAppMetadata(app);
  }
  std::sort(apps.begin(), apps.end(), [](const App &left, const App &right) {
    std::string left_name = left.name;
    std::string right_name = right.name;
    std::transform(left_name.begin(), left_name.end(), left_name.begin(), ::tolower);
    std::transform(right_name.begin(), right_name.end(), right_name.begin(), ::tolower);
    return left_name == right_name ? left.package < right.package : left_name < right_name;
  });
  return apps;
}

bool ValidRepository(const std::string &path) {
  if (path.empty() || path.size() > 1024 || path.find("..") != std::string::npos)
    return false;
  return path.compare(0, 8, "/sdcard/") == 0 ||
      path.compare(0, 14, "/data/media/0/") == 0 ||
      path.compare(0, 9, "/mnt/nas/") == 0;
}

bool MakeDirectories(const std::string &path) {
  if (!ValidRepository(path)) return false;
  std::string current;
  size_t start = 1;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    current = path.substr(0, slash == std::string::npos ? path.size() : slash);
    if (!current.empty() && mkdir(current.c_str(), 0700) && errno != EEXIST)
      return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return Directory(path);
}

std::string LastUsefulLine(const std::string &text) {
  size_t end = text.find_last_not_of("\r\n ");
  if (end == std::string::npos) return "The backup engine did not return details.";
  size_t begin = text.rfind('\n', end);
  std::string line = text.substr(begin == std::string::npos ? 0 : begin + 1,
                                 end - (begin == std::string::npos ? 0 : begin + 1) + 1);
  Json::Value message;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (reader->parse(line.data(), line.data() + line.size(), &message, &errors) &&
      message.isObject() && message["message"].isString()) {
    line = message["message"].asString();
    if (line.compare(0, 7, "Fatal: ") == 0) line.erase(0, 7);
  }
  if (line.size() > 220) line.resize(220);
  return line;
}

void ParseProgress(State *state, const std::string &text) {
  size_t pos = 0;
  while ((pos = text.find("\"percent_done\"", pos)) != std::string::npos) {
    pos = text.find(':', pos);
    if (pos == std::string::npos) return;
    char *end = nullptr;
    const double value = strtod(text.c_str() + pos + 1, &end);
    if (end != text.c_str() + pos + 1)
      state->work_progress.store(static_cast<unsigned>(
          std::clamp(value * 100.0, 1.0, 99.0)));
    pos++;
  }
}

bool WriteAll(int fd, const std::string &value) {
  size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t count = write(fd, value.data() + offset, value.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

int RunRestic(State *state, const std::vector<std::string> &arguments,
              std::string &output) {
  const std::string program = state->runtime + "/usr/bin/restic";
  struct stat info{};
  if (lstat(program.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_uid ||
      (info.st_mode & 0022) || !(info.st_mode & 0100)) {
    output = "The verified backup engine is unavailable.";
    return -1;
  }
  char password_path[] = "/tmp/aera-vault-password-XXXXXX";
  const int password_fd = mkstemp(password_path);
  if (password_fd < 0 || fchmod(password_fd, 0600) ||
      !WriteAll(password_fd, state->password_value + "\n")) {
    if (password_fd >= 0) close(password_fd);
    unlink(password_path);
    output = "Could not create a private password channel.";
    return -1;
  }
  fsync(password_fd);
  close(password_fd);
  mkdir("/tmp/aera-restic-cache", 0700);

  int channel[2] = {-1, -1};
  if (pipe2(channel, O_CLOEXEC)) {
    unlink(password_path);
    output = "Could not create the backup engine channel.";
    return -1;
  }
  std::vector<std::string> values = {program, "--repo", state->repository,
                                     "--password-file", password_path};
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  const pid_t child = fork();
  if (!child) {
    dup2(channel[1], STDOUT_FILENO);
    dup2(channel[1], STDERR_FILENO);
    close(channel[0]);
    close(channel[1]);
    setenv("TMPDIR", "/tmp", 1);
    setenv("RESTIC_CACHE_DIR", "/tmp/aera-restic-cache", 1);
    setenv("GOMAXPROCS", "4", 1);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  close(channel[1]);
  if (child < 0) {
    close(channel[0]);
    unlink(password_path);
    output = "Could not start the backup engine.";
    return -1;
  }
  state->child.store(child);
  char buffer[8192];
  while (true) {
    const ssize_t count = read(channel[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    output.append(buffer, static_cast<size_t>(count));
    ParseProgress(state, output);
    if (output.size() > (1U << 20)) output.erase(0, output.size() - (1U << 19));
    if (state->cancel.load()) kill(child, SIGTERM);
  }
  close(channel[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  state->child.store(-1);
  unlink(password_path);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string PackageFromBackupPath(std::string path) {
  if (path.empty()) return {};
  if (path[0] != '/') path.insert(path.begin(), '/');
  static constexpr const char *prefixes[] = {
      "/data/user/0/", "/data/user_de/0/",
      "/data/media/0/Android/data/", "/data/media/0/Android/obb/",
      "/data/media/0/Android/media/"};
  for (const char *prefix : prefixes) {
    if (path.compare(0, strlen(prefix), prefix)) continue;
    std::string package = path.substr(strlen(prefix));
    const size_t slash = package.find('/');
    if (slash != std::string::npos) package.resize(slash);
    return SafePackage(package.c_str()) ? package : std::string();
  }
  if (path.compare(0, 10, "/data/app/") == 0) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
      std::string package = path.substr(slash + 1);
      const size_t suffix = package.find('-');
      if (suffix != std::string::npos) package.resize(suffix);
      if (SafePackage(package.c_str())) return package;
    }
  }
  return {};
}

const App *InstalledApp(const State *state, const std::string &package) {
  const auto match = std::find_if(state->apps.begin(), state->apps.end(),
      [&](const App &app) { return app.package == package; });
  return match == state->apps.end() ? nullptr : &*match;
}

std::string SnapshotDate(const std::string &time) {
  if (time.size() >= 16) {
    std::string date = time.substr(0, 16);
    if (date[10] == 'T') date[10] = ' ';
    return date;
  }
  return time.empty() ? "Unknown date" : time;
}

std::string HumanBytes(uint64_t bytes) {
  static constexpr const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit;
  }
  char text[48];
  snprintf(text, sizeof(text), unit > 1 ? "%.1f %s" : "%.0f %s", value,
           units[unit]);
  return text;
}

void ParseSnapshots(State *state, const std::string &output) {
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(output.data(), output.data() + output.size(), &root,
                     &errors) || !root.isArray()) return;
  std::lock_guard<std::mutex> lock(state->result_mutex);
  state->snapshots = root.size();
  state->latest_id.clear();
  state->latest_time.clear();
  state->snapshot_items.clear();
  for (const auto &snapshot : root) {
    Snapshot item;
    item.id = snapshot["short_id"].asString();
    if (item.id.empty()) item.id = snapshot["id"].asString();
    item.time = snapshot["time"].asString();
    if (snapshot["summary"].isObject())
      item.bytes = snapshot["summary"]["total_bytes_processed"].asUInt64();
    std::map<std::string, std::vector<std::string>> app_paths;
    for (const auto &entry : snapshot["paths"]) {
      const std::string path = entry.asString();
      const std::string package = PackageFromBackupPath(path);
      if (!package.empty()) app_paths[package].push_back(path);
    }
    for (auto &[package, paths] : app_paths) {
      SnapshotApp app;
      app.package = package;
      app.paths = std::move(paths);
      const App *installed = InstalledApp(state, package);
      app.name = installed ? installed->name : FriendlyPackageName(package);
      item.apps.push_back(std::move(app));
    }
    std::sort(item.apps.begin(), item.apps.end(),
        [](const SnapshotApp &left, const SnapshotApp &right) {
          return left.name < right.name;
        });
    state->snapshot_items.push_back(std::move(item));
    const std::string time = snapshot["time"].asString();
    if (time >= state->latest_time) {
      state->latest_time = time;
      state->latest_id = snapshot["short_id"].asString();
      if (state->latest_id.empty()) state->latest_id = snapshot["id"].asString();
    }
  }
  std::sort(state->snapshot_items.begin(), state->snapshot_items.end(),
      [](const Snapshot &left, const Snapshot &right) {
        return left.time > right.time;
      });
}

void Worker(State *state, Work work, std::vector<std::string> paths) {
  std::string output;
  std::vector<std::string> arguments;
  if (work == Work::kInitialize) {
    if (!MakeDirectories(state->repository)) {
      std::lock_guard<std::mutex> lock(state->result_mutex);
      state->result = "Could not create the vault repository.";
      state->done.store(true);
      return;
    }
    arguments = {"init"};
  } else if (work == Work::kBackup) {
    if (access((state->repository + "/config").c_str(), R_OK) != 0) {
      if (!MakeDirectories(state->repository)) {
        std::lock_guard<std::mutex> lock(state->result_mutex);
        state->result = "Could not create the vault repository.";
        state->work_success.store(false);
        state->done.store(true);
        return;
      }
      const int initialize = RunRestic(state, {"init"}, output);
      if (initialize) {
        std::lock_guard<std::mutex> lock(state->result_mutex);
        state->result = LastUsefulLine(output);
        state->work_success.store(false);
        state->done.store(true);
        return;
      }
      output.clear();
      state->work_progress.store(8);
    }
    arguments = {"backup", "--json", "--tag", kTag, "--host", "aera-recovery"};
    arguments.insert(arguments.end(), paths.begin(), paths.end());
  } else if (work == Work::kRestore) {
    arguments = {"restore", state->restore_snapshot, "--tag", kTag, "--target", "/",
                 "--exclude-xattr", "security.selinux"};
    for (const auto &path : paths) {
      arguments.push_back("--include");
      arguments.push_back(path);
    }
  } else {
    arguments = {"snapshots", "--json", "--tag", kTag};
  }
  const int result = RunRestic(state, arguments, output);
  if (!result && work == Work::kRefresh) ParseSnapshots(state, output);
  {
    std::lock_guard<std::mutex> lock(state->result_mutex);
    if (!result) {
      state->result = work == Work::kInitialize ? "Encrypted vault created." :
          work == Work::kBackup ? "App backup completed successfully." :
          work == Work::kRestore ? "Latest app backup restored." :
          "Vault snapshots refreshed.";
    } else {
      state->result = LastUsefulLine(output);
    }
  }
  state->work_success.store(!result);
  state->work_progress.store(result ? 0 : 100);
  state->done.store(true, std::memory_order_release);
}

void UpdateSelected(State *state) {
  const auto visible = [state](const App &app) {
    return state->show_system || !app.system;
  };
  const size_t total = std::count_if(state->apps.begin(), state->apps.end(), visible);
  const size_t count = std::count_if(state->apps.begin(), state->apps.end(),
      [&](const App &app) { return visible(app) && app.selected; });
  const std::string label = std::to_string(count) + " of " +
      std::to_string(total) + " selected";
  lv_label_set_text(state->selected, label.c_str());
  if (state->system_filter_label)
    lv_label_set_text(state->system_filter_label,
                      state->show_system ? "System apps  On" : "System apps  Off");
  if (state->select_all_label)
    lv_label_set_text(state->select_all_label,
                      total && count == total ? "Clear all" : "Select all");
}

void RenderApps(State *state) {
  lv_obj_clean(state->list);
  int y = 0;
  for (size_t index = 0; index < state->apps.size(); ++index) {
    auto &app = state->apps[index];
    if (app.system && !state->show_system) continue;
    auto *row = lv_button_create(state->list);
    Clear(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, std::max(300, static_cast<int>(lv_obj_get_width(state->list))), 128);
    lv_obj_set_style_radius(row, 24, 0);
    lv_obj_set_style_bg_color(row, kMainSelected, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);
    auto *icon_plate = lv_obj_create(row);
    Panel(icon_plate, 22, kMainPanel);
    lv_obj_set_pos(icon_plate, 18, 26);
    lv_obj_set_size(icon_plate, 76, 76);
    lv_obj_remove_flag(icon_plate, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(icon_plate, LV_OBJ_FLAG_SCROLLABLE);
    if (app.icon) {
      auto *image = lv_image_create(icon_plate);
      lv_image_set_src(image, &app.icon->descriptor);
      lv_image_set_antialias(image, true);
      lv_obj_center(image);
      lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    } else {
      char initial[2] = {'?', 0};
      for (const unsigned char c : app.name) {
        if (std::isalnum(c)) {
          initial[0] = static_cast<char>(std::toupper(c));
          break;
        }
      }
      auto *letter = Label(icon_plate, initial, &lv_font_montserrat_32, kAccent);
      lv_obj_center(letter);
    }
    auto *name = Label(row, app.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 118, 18);
    lv_obj_set_width(name, std::max(200, static_cast<int>(lv_obj_get_width(state->list)) - 210));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    auto *package = Label(row, app.package.c_str(), &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(package, 118, 68);
    lv_obj_set_width(package,
        std::max(200, static_cast<int>(lv_obj_get_width(state->list)) - 300));
    lv_label_set_long_mode(package, LV_LABEL_LONG_DOT);
    if (app.system) {
      auto *badge = Kicker(row, "SYSTEM", kMutedStrong);
      lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -98, 0);
    }
    auto *indicator = lv_obj_create(row);
    Clear(indicator);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(indicator, 58, 58);
    lv_obj_align(indicator, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_radius(indicator, 18, 0);
    lv_obj_set_style_border_width(indicator, 2, 0);
    lv_obj_set_style_border_color(indicator, app.selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_bg_color(indicator, app.selected ? kAccent : kMainPanel, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    auto *mark = Label(indicator, app.selected ? LV_SYMBOL_OK : "",
                       &lv_font_montserrat_24,
                       app.selected ? kOnAccent : kMuted);
    lv_obj_center(mark);
    OnClick(row, [state, index, indicator, mark] {
      auto &selected_app = state->apps[index];
      selected_app.selected = !selected_app.selected;
      lv_obj_set_style_border_color(indicator,
          selected_app.selected ? kAccent : kMainLine, 0);
      lv_obj_set_style_bg_color(indicator,
          selected_app.selected ? kAccent : kMainPanel, 0);
      lv_label_set_text(mark, selected_app.selected ? LV_SYMBOL_OK : "");
      lv_obj_set_style_text_color(mark,
          selected_app.selected ? kOnAccent : kMuted, 0);
      UpdateSelected(state);
    });
    y += 136;
  }
  const bool any_visible = std::any_of(state->apps.begin(), state->apps.end(),
      [state](const App &app) { return state->show_system || !app.system; });
  if (!any_visible) {
    auto *empty = Label(state->list,
        RecoveryDataLocked() ? "Unlock data to discover installed apps." :
        "No user app data was found.", &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 100);
  }
  UpdateSelected(state);
}

void SetActions(State *state, bool enabled) {
  for (auto *button : {state->initialize, state->backup,
                       state->refresh_button, state->restore}) {
    if (!button) continue;
    if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
  }
}

void StartWork(State *state, Work work) {
  if (!state || state->busy.load() || state->runtime.empty()) return;
  if (!ValidRepository(state->repository)) {
    Sheet(state->screen, "Invalid repository",
          "Choose a folder inside /sdcard, /data/media/0 or mounted /mnt/nas.");
    return;
  }
  if (state->password_value.size() < 8) {
    Sheet(state->screen, "Vault password required",
          "Set a password of at least 8 characters. AERA never stores it.");
    return;
  }
  std::vector<std::string> paths;
  if (work == Work::kBackup) {
    for (const auto &app : state->apps)
      if (app.selected) paths.insert(paths.end(), app.paths.begin(), app.paths.end());
    if (paths.empty()) {
      Sheet(state->screen, "Nothing selected",
            "Select at least one app before starting the backup.");
      return;
    }
  } else if (work == Work::kRestore) {
    const auto snapshot = std::find_if(
        state->snapshot_items.begin(), state->snapshot_items.end(),
        [state](const Snapshot &item) { return item.id == state->restore_snapshot; });
    if (snapshot == state->snapshot_items.end()) {
      Sheet(state->screen, "Choose a backup",
            "Open Restore apps and select the backup you want to use.");
      return;
    }
    for (const auto &app : snapshot->apps)
      if (app.selected) paths.insert(paths.end(), app.paths.begin(), app.paths.end());
    if (paths.empty()) {
      Sheet(state->screen, "Nothing selected",
            "Select at least one app from this backup before restoring.");
      return;
    }
  }
  if (state->worker.joinable()) state->worker.join();
  state->work = work;
  state->done.store(false);
  state->work_success.store(false);
  state->cancel.store(false);
  state->busy.store(true);
  state->work_progress.store(4);
  SetActions(state, false);
  lv_obj_remove_flag(state->progress, LV_OBJ_FLAG_HIDDEN);
  lv_bar_set_value(state->progress, 4, LV_ANIM_OFF);
  const char *title = work == Work::kInitialize ? "Creating encrypted vault…" :
      work == Work::kBackup ? "Backing up selected apps…" :
      work == Work::kRestore ? "Restoring latest snapshot…" :
      "Reading vault snapshots…";
  lv_label_set_text(state->status, title);
  state->worker = std::thread(Worker, state, work, std::move(paths));
}

void EditValue(State *state, bool password) {
  if (!state || state->busy.load()) return;
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  const bool landscape = Landscape(state->screen);
  lv_obj_set_size(sheet, landscape ? 1900 : 1312, landscape ? 1180 : 1260);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_set_style_pad_all(sheet, 48, 0);
  auto *title = Label(sheet, password ? "Vault password" : "Repository folder",
                      &lv_font_montserrat_48, kText);
  auto *hint = Label(sheet, password ?
      "Used only for this recovery session. AERA never saves the password." :
      "Use internal storage or a mounted NAS folder.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 0, 76);
  auto *input = lv_textarea_create(sheet);
  lv_obj_set_pos(input, 0, 150);
  lv_obj_set_size(input, landscape ? 1804 : 1216, 126);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_max_length(input, 1024);
  lv_textarea_set_password_mode(input, password);
  lv_textarea_set_password_show_time(input, 0);
  lv_textarea_set_text(input, password ? "" : state->repository.c_str());
  lv_textarea_set_placeholder_text(input, password ? "Password" : kDefaultRepository);
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_bg_color(input, kMainPanel, 0);
  lv_obj_set_style_border_color(input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(input, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(input, 24, 0);
  auto *keyboard = lv_keyboard_create(sheet);
  phone_keyboard::Apply(keyboard);
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard, 0, 306);
  lv_obj_set_size(keyboard, landscape ? 1804 : 1216, landscape ? 570 : 630);
  lv_keyboard_set_textarea(keyboard, input);
  const int button_y = landscape ? 920 : 986;
  const int total_width = landscape ? 1804 : 1216;
  const int button_width = (total_width - 56) / 2;
  auto *cancel = Button(sheet, "Cancel", [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_set_pos(cancel, 0, button_y);
  lv_obj_set_size(cancel, button_width, 116);
  auto *save = Button(sheet, "Save", [state, password, input, overlay] {
    const std::string value = lv_textarea_get_text(input);
    if (password) {
      state->password_value = value;
      lv_label_set_text(state->password, value.empty() ? "Not set" : "Set for this session");
    } else if (ValidRepository(value)) {
      state->repository = value;
      lv_label_set_text(state->repository_label, value.c_str());
    } else {
      Sheet(state->screen, "Invalid repository",
            "Choose a folder inside /sdcard, /data/media/0 or mounted /mnt/nas.");
      return;
    }
    lv_obj_delete_async(overlay);
  }, true);
  lv_obj_set_pos(save, button_width + 56, button_y);
  lv_obj_set_size(save, button_width, 116);
  lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
  AnimateEnter(sheet, 0, 38);
}

void ShowSnapshotChooser(State *state);

void UpdateSnapshotCount(const Snapshot &snapshot, lv_obj_t *label) {
  const size_t selected = std::count_if(
      snapshot.apps.begin(), snapshot.apps.end(),
      [](const SnapshotApp &app) { return app.selected; });
  const std::string text = std::to_string(selected) + " of " +
      std::to_string(snapshot.apps.size()) + " apps selected";
  lv_label_set_text(label, text.c_str());
}

void ShowSnapshotApps(State *state, size_t snapshot_index) {
  if (!state || snapshot_index >= state->snapshot_items.size()) return;
  auto &snapshot = state->snapshot_items[snapshot_index];
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  const bool landscape = Landscape(state->screen);
  const int sheet_width = landscape ? 2920 : 1312;
  const int sheet_height = landscape ? 1280 : 2520;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, LV_ALIGN_CENTER, 0, landscape ? 70 : 80);

  auto *back = Button(sheet, LV_SYMBOL_LEFT, [state, overlay] {
    lv_obj_delete_async(overlay);
    ShowSnapshotChooser(state);
  });
  lv_obj_set_pos(back, 36, 30);
  lv_obj_set_size(back, 112, 92);
  auto *title = Label(sheet, "Restore apps", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 176, 30);
  auto *subtitle = Label(sheet, SnapshotDate(snapshot.time).c_str(),
                         &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, 178, 88);
  auto *selected = Label(sheet, "", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(selected, 38, 150);
  UpdateSnapshotCount(snapshot, selected);
  auto *select_all = Button(sheet, "Select all", [state, snapshot_index, overlay] {
    auto &item = state->snapshot_items[snapshot_index];
    const bool all = !item.apps.empty() && std::all_of(
        item.apps.begin(), item.apps.end(),
        [](const SnapshotApp &app) { return app.selected; });
    for (auto &app : item.apps) app.selected = !all;
    lv_obj_delete_async(overlay);
    ShowSnapshotApps(state, snapshot_index);
  });
  lv_obj_align(select_all, LV_ALIGN_TOP_RIGHT, -36, 32);
  lv_obj_set_size(select_all, 300, 92);

  auto *list = lv_obj_create(sheet);
  Clear(list);
  lv_obj_set_pos(list, 28, 205);
  lv_obj_set_size(list, sheet_width - 56, sheet_height - 390);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, kAccent, LV_PART_SCROLLBAR);
  int y = 0;
  for (size_t index = 0; index < snapshot.apps.size(); ++index) {
    auto &saved = snapshot.apps[index];
    auto *row = lv_button_create(list);
    Clear(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, sheet_width - 80, 128);
    lv_obj_set_style_radius(row, 24, 0);
    lv_obj_set_style_bg_color(row, kMainSelected, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);
    auto *icon_plate = lv_obj_create(row);
    Panel(icon_plate, 22, kMainPanel);
    lv_obj_set_pos(icon_plate, 18, 26);
    lv_obj_set_size(icon_plate, 76, 76);
    lv_obj_remove_flag(icon_plate, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(icon_plate, LV_OBJ_FLAG_SCROLLABLE);
    const App *installed = InstalledApp(state, saved.package);
    if (installed && installed->icon) {
      auto *image = lv_image_create(icon_plate);
      lv_image_set_src(image, &installed->icon->descriptor);
      lv_image_set_antialias(image, true);
      lv_obj_center(image);
      lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    } else {
      char initial[2] = {'?', 0};
      for (const unsigned char c : saved.name) {
        if (std::isalnum(c)) {
          initial[0] = static_cast<char>(std::toupper(c));
          break;
        }
      }
      auto *letter = Label(icon_plate, initial, &lv_font_montserrat_32, kAccent);
      lv_obj_center(letter);
    }
    auto *name = Label(row, saved.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 118, 18);
    lv_obj_set_width(name, sheet_width - 350);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    auto *package = Label(row, saved.package.c_str(),
                          &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(package, 118, 68);
    lv_obj_set_width(package, sheet_width - 410);
    lv_label_set_long_mode(package, LV_LABEL_LONG_DOT);
    auto *indicator = lv_obj_create(row);
    Clear(indicator);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(indicator, 58, 58);
    lv_obj_align(indicator, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_radius(indicator, 18, 0);
    lv_obj_set_style_border_width(indicator, 2, 0);
    lv_obj_set_style_border_color(indicator, saved.selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_bg_color(indicator, saved.selected ? kAccent : kMainPanel, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    auto *mark = Label(indicator, saved.selected ? LV_SYMBOL_OK : "",
                       &lv_font_montserrat_24,
                       saved.selected ? kOnAccent : kMuted);
    lv_obj_center(mark);
    OnClick(row, [state, snapshot_index, index, indicator, mark, selected] {
      auto &app = state->snapshot_items[snapshot_index].apps[index];
      app.selected = !app.selected;
      lv_obj_set_style_border_color(indicator,
          app.selected ? kAccent : kMainLine, 0);
      lv_obj_set_style_bg_color(indicator,
          app.selected ? kAccent : kMainPanel, 0);
      lv_label_set_text(mark, app.selected ? LV_SYMBOL_OK : "");
      lv_obj_set_style_text_color(mark, app.selected ? kOnAccent : kMuted, 0);
      UpdateSnapshotCount(state->snapshot_items[snapshot_index], selected);
    });
    y += 136;
  }
  if (snapshot.apps.empty()) {
    auto *empty = Label(list, "This backup does not contain recognizable app data.",
                        &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 120);
  }
  auto *restore = Button(sheet, "Restore selected apps",
      [state, snapshot_index, overlay] {
        auto &item = state->snapshot_items[snapshot_index];
        const size_t count = std::count_if(
            item.apps.begin(), item.apps.end(),
            [](const SnapshotApp &app) { return app.selected; });
        if (!count) return;
        state->restore_snapshot = item.id;
        lv_obj_delete_async(overlay);
        const std::string message = "Restore " + std::to_string(count) +
            " selected apps from " + SnapshotDate(item.time) + "?";
        Sheet(state->screen, "Restore selected apps?", message.c_str(),
              [state] { StartWork(state, Work::kRestore); });
      }, true);
  lv_obj_set_size(restore, sheet_width - 72, 116);
  lv_obj_align(restore, LV_ALIGN_BOTTOM_MID, 0, -28);
  AnimateEnter(sheet, 0, 28);
}

void ShowSnapshotChooser(State *state) {
  if (!state) return;
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  const bool landscape = Landscape(state->screen);
  const int sheet_width = landscape ? 2920 : 1312;
  const int sheet_height = landscape ? 1280 : 2200;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, LV_ALIGN_CENTER, 0, landscape ? 70 : 40);
  auto *title = Label(sheet, "Restore apps", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 40, 34);
  auto *hint = Label(sheet, "Choose a backup to see exactly which apps it contains.",
                     &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 42, 98);
  auto *close = Button(sheet, LV_SYMBOL_CLOSE,
                       [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -36, 30);
  lv_obj_set_size(close, 112, 92);
  auto *list = lv_obj_create(sheet);
  Clear(list);
  lv_obj_set_pos(list, 28, 170);
  lv_obj_set_size(list, sheet_width - 56, sheet_height - 210);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  int y = 0;
  for (size_t index = 0; index < state->snapshot_items.size(); ++index) {
    const auto &snapshot = state->snapshot_items[index];
    auto *card = Button(list, "", [state, index, overlay] {
      lv_obj_delete_async(overlay);
      ShowSnapshotApps(state, index);
    });
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, sheet_width - 80, 174);
    lv_obj_set_style_radius(card, 30, 0);
    auto *date = Label(card, SnapshotDate(snapshot.time).c_str(),
                       &lv_font_montserrat_36, kText);
    lv_obj_set_pos(date, 30, 24);
    std::string detail = std::to_string(snapshot.apps.size()) + " apps";
    if (snapshot.bytes) detail += " • " + HumanBytes(snapshot.bytes);
    auto *details = Label(card, detail.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(details, 32, 86);
    auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kAccent);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -34, 0);
    y += 190;
  }
  if (state->snapshot_items.empty()) {
    auto *empty = Label(list, "No app backups found in this vault.",
                        &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 140);
  }
  AnimateEnter(sheet, 0, 28);
}

void Poll(State *state) {
  if (!state) return;
  if (state->preparation.done.load(std::memory_order_acquire) &&
      state->metadata_done.load(std::memory_order_acquire) &&
      !state->preparation_handled.exchange(true)) {
    if (state->prepare_thread.joinable()) state->prepare_thread.join();
    if (state->preparation.verified) {
      state->runtime = state->preparation.directory;
      lv_label_set_text(state->status, "Ready");
      const size_t user_apps = std::count_if(state->apps.begin(), state->apps.end(),
          [](const App &app) { return !app.system; });
      const size_t system_apps = state->apps.size() - user_apps;
      const std::string detail = std::to_string(user_apps) +
          " user apps • " + std::to_string(system_apps) + " system apps hidden";
      lv_label_set_text(state->detail, detail.c_str());
      RenderApps(state);
      SetActions(state, !RecoveryDataLocked());
    } else {
      lv_label_set_text(state->status, "Engine unavailable");
      lv_label_set_text(state->detail, state->preparation.error.c_str());
      SetActions(state, false);
    }
  }
  if (state->busy.load())
    lv_bar_set_value(state->progress, state->work_progress.load(), LV_ANIM_ON);
  if (state->busy.load() && state->done.exchange(false)) {
    if (state->worker.joinable()) state->worker.join();
    std::string result;
    {
      std::lock_guard<std::mutex> lock(state->result_mutex);
      result = state->result;
    }
    state->busy.store(false);
    SetActions(state, true);
    lv_bar_set_value(state->progress, state->work_progress.load(), LV_ANIM_ON);
    if (state->work_success.load()) {
      lv_label_set_text(state->status, result.c_str());
    } else {
      const char *failure = state->work == Work::kInitialize ? "Could not create vault" :
          state->work == Work::kBackup ? "App backup failed" :
          state->work == Work::kRestore ? "App restore failed" :
          "Could not read snapshots";
      lv_label_set_text(state->status, failure);
      lv_label_set_text(state->detail, result.c_str());
    }
    if (state->work_success.load() && state->work == Work::kRefresh) {
      std::string detail = std::to_string(state->snapshots) + " app backups";
      if (!state->latest_time.empty())
        detail += " • latest " + SnapshotDate(state->latest_time);
      lv_label_set_text(state->detail, detail.c_str());
      if (state->browse_after_refresh) ShowSnapshotChooser(state);
    }
    state->browse_after_refresh = false;
  }
}

void Destroy(State *state) {
  if (!state) return;
  if (state->timer) lv_timer_delete(state->timer);
  state->preparation.cancel.store(true);
  state->cancel.store(true);
  const pid_t child = state->child.load();
  if (child > 0) kill(child, SIGTERM);
  if (state->prepare_thread.joinable()) state->prepare_thread.join();
  if (state->worker.joinable()) state->worker.join();
  if (state->list) lv_obj_clean(state->list);
  for (const auto &app : state->apps)
    if (app.icon) lv_image_cache_drop(&app.icon->descriptor);
  web::RemoveRuntime(state->runtime.empty() ? state->preparation.directory : state->runtime);
  std::fill(state->password_value.begin(), state->password_value.end(), '\0');
  delete state;
}

}  // namespace

void BuildAppVaultScene(lv_obj_t *screen, ActionCallback callback,
                        void *context) {
  Header(screen, "App Backup Vault",
         "Encrypted per-app backups on internal or network storage.",
         callback, context);
  auto *state = new State;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    Destroy(static_cast<State *>(lv_event_get_user_data(event)));
  }, LV_EVENT_DELETE, state);

  const bool landscape = Landscape(screen);
  auto *hero = lv_obj_create(screen);
  Panel(hero, 46, kMainSheet);
  lv_obj_set_pos(hero, 64, landscape ? 320 : 450);
  lv_obj_set_size(hero, landscape ? 900 : 1312, landscape ? 780 : 600);
  auto *plate = IconPlate(hero, LV_SYMBOL_SAVE, kCyan, kMainPanel, 116);
  lv_obj_set_pos(plate, 38, 38);
  state->status = Label(hero, "Preparing secure engine…",
                        &lv_font_montserrat_48, kText);
  lv_obj_set_pos(state->status, 184, 44);
  lv_obj_set_width(state->status, landscape ? 650 : 1040);
  lv_label_set_long_mode(state->status, LV_LABEL_LONG_DOT);
  state->detail = Label(hero, "The plugin is verified and expanded only in RAM.",
                        &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->detail, 184, 104);
  lv_obj_set_width(state->detail, landscape ? 650 : 1040);
  lv_label_set_long_mode(state->detail, LV_LABEL_LONG_WRAP);

  auto *repo_row = Button(hero, "", [state] { EditValue(state, false); });
  lv_obj_set_pos(repo_row, 36, 196);
  lv_obj_set_size(repo_row, landscape ? 828 : 1240, 134);
  lv_obj_set_style_radius(repo_row, 30, 0);
  auto *repo_title = Label(repo_row, "Repository", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(repo_title, 28, 18);
  state->repository_label = Label(repo_row, state->repository.c_str(),
                                  &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->repository_label, 28, 62);
  lv_obj_set_width(state->repository_label, landscape ? 700 : 1100);
  lv_label_set_long_mode(state->repository_label, LV_LABEL_LONG_DOT);

  auto *password_row = Button(hero, "", [state] { EditValue(state, true); });
  lv_obj_set_pos(password_row, 36, 350);
  lv_obj_set_size(password_row, landscape ? 828 : 1240, 134);
  lv_obj_set_style_radius(password_row, 30, 0);
  auto *password_title = Label(password_row, "Vault password",
                               &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(password_title, 28, 18);
  state->password = Label(password_row, "Not set", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->password, 28, 62);

  state->progress = lv_bar_create(hero);
  lv_obj_set_pos(state->progress, 38, landscape ? 548 : 522);
  lv_obj_set_size(state->progress, landscape ? 824 : 1236, 12);
  lv_bar_set_range(state->progress, 0, 100);
  lv_obj_set_style_bg_color(state->progress, kMainPanel, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(state->progress, LV_OBJ_FLAG_HIDDEN);

  auto *apps_panel = lv_obj_create(screen);
  Panel(apps_panel, 46, kMainSheet);
  lv_obj_set_pos(apps_panel, landscape ? 994 : 64, landscape ? 320 : 1080);
  lv_obj_set_size(apps_panel, landscape ? 2110 : 1312,
                  landscape ? 780 : 1340);
  auto *apps_title = Label(apps_panel, "Apps", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(apps_title, 34, 24);
  state->selected = Label(apps_panel, "Discovering apps…",
                          &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->selected, 36, 82);
  const int filter_width = landscape ? 410 : 330;
  auto *select_all = Button(apps_panel, "", [state] {
    const bool all_selected = std::all_of(state->apps.begin(), state->apps.end(),
        [state](const App &app) {
          return (!state->show_system && app.system) || app.selected;
        });
    for (auto &app : state->apps)
      if (state->show_system || !app.system) app.selected = !all_selected;
    RenderApps(state);
  });
  lv_obj_set_size(select_all, filter_width, 82);
  lv_obj_align(select_all, LV_ALIGN_TOP_RIGHT, -(filter_width + 44), 24);
  state->select_all_label = lv_obj_get_child(select_all, 0);
  lv_obj_set_style_text_font(state->select_all_label,
                             UiFont(&lv_font_montserrat_24), 0);
  auto *system_filter = Button(apps_panel, "", [state] {
    state->show_system = !state->show_system;
    RenderApps(state);
  });
  lv_obj_set_size(system_filter, filter_width, 82);
  lv_obj_align(system_filter, LV_ALIGN_TOP_RIGHT, -24, 24);
  state->system_filter_label = lv_obj_get_child(system_filter, 0);
  lv_obj_set_style_text_font(state->system_filter_label,
                             UiFont(&lv_font_montserrat_24), 0);
  state->list = lv_obj_create(apps_panel);
  Clear(state->list);
  lv_obj_set_pos(state->list, 24, 132);
  lv_obj_set_size(state->list, landscape ? 2062 : 1264,
                  landscape ? 610 : 1168);
  lv_obj_add_flag(state->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(state->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(state->list, kAccent, LV_PART_SCROLLBAR);

  auto *actions = lv_obj_create(screen);
  Clear(actions);
  lv_obj_set_pos(actions, 64, landscape ? 1124 : 2450);
  lv_obj_set_size(actions, landscape ? 3040 : 1312, landscape ? 118 : 330);
  const int gap = 20;
  const int columns = landscape ? 4 : 2;
  const int width = ((landscape ? 3040 : 1312) - gap * (columns - 1)) / columns;
  auto add = [&](lv_obj_t **target, const char *text, int index,
                 Handler action, bool primary = false) {
    *target = Button(actions, text, std::move(action), primary);
    const int row = index / columns;
    const int column = index % columns;
    lv_obj_set_pos(*target, column * (width + gap), row * 146);
    lv_obj_set_size(*target, width, 116);
  };
  add(&state->initialize, "Create vault", 0,
      [state] { StartWork(state, Work::kInitialize); });
  add(&state->backup, "Back up selected", 1,
      [state] { StartWork(state, Work::kBackup); }, true);
  add(&state->refresh_button, "Refresh backups", 2, [state] {
    state->browse_after_refresh = false;
    StartWork(state, Work::kRefresh);
  });
  add(&state->restore, "Restore apps", 3, [state] {
    state->browse_after_refresh = true;
    StartWork(state, Work::kRefresh);
  });
  SetActions(state, false);
  AnimateEnter(hero, 20, 12);
  AnimateEnter(apps_panel, 50, 12);
  Navigation(screen, Action::kBackHome, callback, context, true);

  state->prepare_thread = std::thread([state] {
    web::PreparePluginRuntime(state->preparation, kPluginId, kPluginType,
                              kPluginEntry);
    if (state->preparation.verified)
      state->apps = DiscoverApps(state->preparation.cancel);
    state->metadata_done.store(true, std::memory_order_release);
  });
  state->timer = lv_timer_create([](lv_timer_t *timer) {
    Poll(static_cast<State *>(lv_timer_get_user_data(timer)));
  }, 100, state);
}

}  // namespace recovery_ui2
