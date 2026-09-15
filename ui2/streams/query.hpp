/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace recovery_ui2::streams {

bool RunQuery(const std::string& runtime, const std::vector<std::string>& arguments,
              std::atomic<bool>& cancel, std::string& json, std::string& error);

}  // namespace recovery_ui2::streams
