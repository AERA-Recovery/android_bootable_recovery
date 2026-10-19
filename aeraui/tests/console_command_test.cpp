/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/plugins/console_commands.hpp"
#include "../features/plugins/plugin_manager.hpp"
#include "../features/browser/runtime.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

static std::vector<aeraui::plugins::Plugin> installed;
namespace aeraui::plugins {
std::vector<Plugin> Installed() { return installed; }
bool FindInstalled(const std::string &, Plugin &) { return false; }
}
namespace aeraui::web {
void PreparePluginRuntime(Preparation &, const char *, const char *, const char *, const char *) { assert(false); }
void RemoveRuntime(const std::string &) { assert(false); }
}
static std::string Read(const std::string &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main() {
  using namespace aeraui::plugins;
  for (const char *name : {"codex", "my-tool", "tool_2"}) assert(ValidConsoleName(name));
  for (const char *name : {"", "../codex", "bad;id", "bad name", "'", "--flag"}) assert(!ValidConsoleName(name));
  assert(ValidConsoleExecutable("usr/bin/codex"));
  for (const char *path : {"", "/bin/sh", "../bin/sh", "usr//bin/x", "usr/./x", "usr/../x", "bin/x;id", "bin/"})
    assert(!ValidConsoleExecutable(path));
  char root[] = "/tmp/aera-console-commands-XXXXXX"; assert(mkdtemp(root));
  const std::string directory = root;
  std::ofstream(directory + "/sh") << "system shell";
  Plugin plugin; plugin.id = "codex-cli"; plugin.commands.push_back({"codex", "usr/bin/codex"});
  Plugin legacy; legacy.id = "legacy";
  installed = {legacy};
  assert(RefreshConsoleCommands(nullptr, directory));
  assert(Read(directory + "/sh") == "system shell");
  installed.push_back(plugin);
  assert(RefreshConsoleCommands(nullptr, directory));
  auto script = Read(directory + "/codex");
  assert(script.find("--aera-plugin-command 'codex-cli' 'codex' \"$@\"") != std::string::npos);
  assert(RefreshConsoleCommands(nullptr, directory));
  assert(Read(directory + "/codex") == script);
  plugin.commands.push_back({"sh", "usr/bin/other"}); installed = {plugin};
  std::string error;
  assert(!RefreshConsoleCommands(&error, directory) && !error.empty());
  assert(Read(directory + "/sh") == "system shell");
  installed.clear();
  assert(RefreshConsoleCommands(nullptr, directory));
  assert(!std::filesystem::exists(directory + "/codex"));
  assert(Read(directory + "/sh") == "system shell");
  std::ofstream(directory + "/foreign") << "foreign file";
  assert(symlink((directory + "/foreign").c_str(), (directory + "/codex").c_str()) == 0);
  installed = {plugin};
  assert(!RefreshConsoleCommands(nullptr, directory));
  assert(Read(directory + "/foreign") == "foreign file");
  assert(unlink((directory + "/codex").c_str()) == 0);
  Plugin duplicate = plugin; duplicate.id = "other-plugin";
  installed = {plugin, duplicate};
  RefreshConsoleCommands(nullptr, directory);
  assert(!std::filesystem::exists(directory + "/codex"));
  std::filesystem::remove_all(directory);
  puts("Console commands: legacy manifests, registration, cleanup, collisions, paths and foreign-file protection passed");
}
