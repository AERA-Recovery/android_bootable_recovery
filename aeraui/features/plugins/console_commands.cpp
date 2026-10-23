/* SPDX-License-Identifier: Apache-2.0 */
#include "console_commands.hpp"
#include "plugin_manager.hpp"
#include "../browser/runtime.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aeraui::plugins {
namespace {
constexpr char kMarker[] = "#!/system/bin/sh\n# AERA plugin command v1\n";
volatile sig_atomic_t gChild = 0;
void ForwardSignal(int signal) { if (gChild > 0) kill(gChild, signal); }
bool OwnedWrapper(const std::string &path, std::string *content = nullptr) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st{}; char buffer[1024];
  const bool regular = !fstat(fd, &st) && S_ISREG(st.st_mode) &&
      st.st_uid == geteuid() && st.st_nlink == 1 &&
      st.st_size >= 0 && st.st_size < static_cast<off_t>(sizeof(buffer));
  const ssize_t size = regular ? read(fd, buffer, sizeof(buffer)) : -1;
  close(fd);
  if (size < static_cast<ssize_t>(sizeof(kMarker) - 1) ||
      memcmp(buffer, kMarker, sizeof(kMarker) - 1)) return false;
  if (content) content->assign(buffer, static_cast<size_t>(size));
  return true;
}
bool WriteWrapper(const std::string &path, const std::string &content) {
  std::string existing;
  if (OwnedWrapper(path, &existing) && existing == content) return true;
  struct stat st{};
  if (lstat(path.c_str(), &st) == 0) {
    if (!OwnedWrapper(path)) return false;
  } else if (errno != ENOENT) return false;
  std::string temporary = path + ".aera-XXXXXX";
  const int fd = mkstemp(temporary.data());
  if (fd < 0) return false;
  const bool ok = write(fd, content.data(), content.size()) ==
      static_cast<ssize_t>(content.size()) && fchmod(fd, 0755) == 0;
  close(fd);
  if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
    unlink(temporary.c_str()); return false;
  }
  return true;
}
}
bool ValidConsoleName(const std::string &name) {
  return !name.empty() && name.size() <= 32 && name[0] >= 'a' && name[0] <= 'z' &&
      std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      });
}
bool ValidConsoleExecutable(const std::string &path) {
  if (path.empty() || path.size() > 200 || path.front() == '/' || path.back() == '/') return false;
  size_t pos = 0;
  while (pos < path.size()) {
    const auto end = path.find('/', pos);
    const auto part = path.substr(pos, end == std::string::npos ? end : end - pos);
    if (part.empty() || part == "." || part == ".." ||
        !std::all_of(part.begin(), part.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        })) return false;
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return true;
}
bool RefreshConsoleCommands(std::string *error, const std::string &directory) {
  if (error) error->clear();
  const int lock = open((directory + "/.aera-plugin-commands.lock").c_str(),
      O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
  struct stat st{};
  if (lock < 0) return false;
  if (fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0022) || flock(lock, LOCK_EX) != 0) { close(lock); return false; }
  std::map<std::string, std::string> commands;
  std::set<std::string> collisions;
  for (const auto &plugin : Installed()) for (const auto &command : plugin.commands) {
    if (!ValidConsoleName(command.name) || !ValidConsoleExecutable(command.executable)) continue;
    if (!commands.emplace(command.name, plugin.id).second) collisions.insert(command.name);
  }
  for (const auto &name : collisions) commands.erase(name);
  bool ok = true;
  DIR *entries = opendir(directory.c_str());
  if (!entries) ok = false;
  else {
    while (const auto *entry = readdir(entries)) {
      const std::string name = entry->d_name;
      if (ValidConsoleName(name) && !commands.count(name) && OwnedWrapper(directory + "/" + name))
        ok &= unlink((directory + "/" + name).c_str()) == 0;
    }
    closedir(entries);
  }
  for (const auto &entry : commands) {
    const std::string script = std::string(kMarker) + "exec /system/bin/recovery --aera-plugin-command '" +
        entry.second + "' '" + entry.first + "' \"$@\"\n";
    if (!WriteWrapper(directory + "/" + entry.first, script)) {
      ok = false;
      if (error && error->empty()) *error = "Console command conflicts with an existing file: " + entry.first;
    }
  }
  close(lock);
  return ok;
}
int RunConsoleCommand(int argc, char **argv) {
  if (argc < 4 || geteuid() != 0 || !ValidConsoleName(argv[3])) return 127;
  Plugin plugin;
  if (!FindInstalled(argv[2], plugin)) {
    fprintf(stderr, "The plugin providing %s is not installed or is unavailable.\n", argv[3]); return 127;
  }
  const auto command = std::find_if(plugin.commands.begin(), plugin.commands.end(),
      [&](const ConsoleCommand &entry) { return entry.name == argv[3]; });
  if (command == plugin.commands.end()) return 127;
  web::Preparation runtime;
  web::PreparePluginRuntime(runtime, plugin.id.c_str(), plugin.type.c_str(), plugin.entry.c_str());
  if (!runtime.verified) { fprintf(stderr, "%s\n", runtime.error.c_str()); return 1; }
  const auto program = runtime.directory + "/" + command->executable;
  struct stat executable{};
  if (lstat(program.c_str(), &executable) || !S_ISREG(executable.st_mode) ||
      executable.st_uid != 0 || !(executable.st_mode & 0111)) {
    web::RemoveRuntime(runtime.directory);
    fprintf(stderr, "The plugin console executable is missing or invalid.\n"); return 126;
  }
  const int signals[] = {SIGINT, SIGTERM, SIGQUIT};
  sigset_t blocked, previous_mask;
  sigemptyset(&blocked);
  for (const int signal : signals) sigaddset(&blocked, signal);
  if (sigprocmask(SIG_BLOCK, &blocked, &previous_mask) != 0) {
    web::RemoveRuntime(runtime.directory); return 1;
  }
  const pid_t child = fork();
  if (child == 0) {
    for (const int signal : signals) ::signal(signal, SIG_DFL);
    sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    setenv("AERA_PLUGIN_ROOT", runtime.directory.c_str(), 1);
    std::vector<char *> arguments{argv[3]};
    for (int i = 4; i < argc; ++i) arguments.push_back(argv[i]);
    arguments.push_back(nullptr);
    execv(program.c_str(), arguments.data());
    perror("Plugin command"); _exit(126);
  }
  int status = 0;
  if (child < 0) {
    sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    status = -1;
  }
  else {
    gChild = child;
    struct sigaction action{}, previous[3]{};
    action.sa_handler = ForwardSignal; sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < 3; ++i) sigaction(signals[i], &action, &previous[i]);
    sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    if (result < 0) status = -1;
    gChild = 0;
    for (size_t i = 0; i < 3; ++i) sigaction(signals[i], &previous[i], nullptr);
  }
  web::RemoveRuntime(runtime.directory);
  return status < 0 ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) :
      WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}
}  // namespace aeraui::plugins
