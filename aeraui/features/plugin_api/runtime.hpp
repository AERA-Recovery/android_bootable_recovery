/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string>

namespace aeraui {
// All methods run on the UI thread, including while the display is suspended.
void PollGenericPluginRuntimes();
void ShutdownGenericPluginRuntimes();
bool CancelGenericPluginActivity(const std::string &plugin);
void StopGenericPluginRuntime(const std::string &plugin);
}
