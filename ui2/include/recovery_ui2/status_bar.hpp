/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

#include "recovery_ui2/engine.hpp"

struct _lv_obj_t;

namespace recovery_ui2 {

enum class StatusBarAction {
  kNone,
  kPower,
  kBack,
};

// Set once before the first UI2 scene is constructed. Values are expressed in
// UI2's 1440-wide logical coordinate space.
void ConfigureStatusBar(int32_t height, int32_t left_indent,
                        int32_t right_indent);
int32_t StatusBarHeight();
// True while the pull-down shade (including its closing animation) owns
// pointer input above the current scene.
bool StatusBarShadeOpen();

// Adds the persistent clock and battery chrome used by native recovery pages.
void AttachStatusBar(_lv_obj_t *screen, void (*callback)(Action, void *),
                     void *context, StatusBarAction action, bool soft_surface = false);

}  // namespace recovery_ui2
