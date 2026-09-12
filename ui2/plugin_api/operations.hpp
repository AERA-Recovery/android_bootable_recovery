/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "protocol.hpp"
#include "../plugins/plugin_manager.hpp"

#include <string>

namespace recovery_ui2::plugin_api {
enum class MirrorMode {
  kOff,
  kUsb,
  kWifi,
};

bool OperationAllowed(const plugins::Plugin &plugin, Operation operation);
bool RunOperation(const plugins::Plugin &plugin, Operation operation,
                  std::string &result);
MirrorMode ActiveMirrorMode();
const char *OperationTitle(Operation operation);
const char *OperationPrompt(Operation operation);
}  // namespace recovery_ui2::plugin_api
