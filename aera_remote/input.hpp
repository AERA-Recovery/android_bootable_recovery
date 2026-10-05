/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>

namespace aera::remote::input {

// Creates the virtual input device ahead of the first event, so recovery's
// input reader has opened it by the time a touch arrives. Touch() and Key()
// still create it on demand when this fails.
bool Prepare();
bool Touch(const std::string& action, int x, int y);
bool Key(const std::string& name);
void Shutdown();

}  // namespace aera::remote::input
