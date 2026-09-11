/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "recovery_ui2/engine.hpp"

struct _lv_obj_t;

namespace recovery_ui2 {

enum class StatusBarAction {
  kNone,
  kPower,
  kBack,
};

// Adds the persistent clock and battery chrome used by native recovery pages.
void AttachStatusBar(_lv_obj_t *screen, void (*callback)(Action, void *),
                     void *context, StatusBarAction action, bool soft_surface = false);

}  // namespace recovery_ui2
