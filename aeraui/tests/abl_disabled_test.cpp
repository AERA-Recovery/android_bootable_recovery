// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../aera_efisp.hpp"
#include "../../aera_abl.hpp"
#include <cassert>
int main() {
  assert(!aera_efisp::Enabled());
  assert(aera_efisp::CaptureAblBaseline().empty());
  assert(aera_efisp::CurrentAblSamples().empty());
  assert(aera_efisp::AblExitWarning().text.empty());
}
