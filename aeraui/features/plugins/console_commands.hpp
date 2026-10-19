/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string>
namespace aeraui::plugins {
bool ValidConsoleName(const std::string &name);
bool ValidConsoleExecutable(const std::string &path);
bool RefreshConsoleCommands(std::string *error = nullptr,
                            const std::string &directory = "/system/bin");
int RunConsoleCommand(int argc, char **argv);
}
