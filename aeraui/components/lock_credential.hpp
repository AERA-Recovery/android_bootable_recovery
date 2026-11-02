// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <lvgl.h>
#include <aeraui/screen_lock.hpp>
#include <functional>
namespace aeraui {
enum class LockCredentialAction { Verify, SetPin, SetPassword, Remove, EnableFingerprint, DisableFingerprint };
lv_obj_t* OpenLockCredential(lv_obj_t* parent, LockCredentialAction action,
                             std::function<void()> completed, uint64_t generation=0);
}
