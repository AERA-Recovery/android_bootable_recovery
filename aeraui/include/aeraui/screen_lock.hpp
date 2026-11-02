// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lock_security.hpp"
#include <cstdint>
namespace aeraui {
#if defined(AERA_SUPPORT_FINGERPRINT) && AERA_SUPPORT_FINGERPRINT == 1
inline constexpr bool kFingerprintSupported = true;
#else
inline constexpr bool kFingerprintSupported = false;
#endif
lock_security::Status RecoveryScreenLockStatus();
lock_security::Result RecoverySetScreenLock(const std::string& current, lock_security::Kind kind,
                                          const std::string& secret, const std::string& confirmation);
lock_security::Result RecoveryRemoveScreenLock(const std::string& current);
lock_security::Result RecoverySetScreenFingerprint(const std::string& current, bool enabled);
// Every visible lock owns one generation. Results from deleted/sleeping scenes cannot unlock a newer lock.
uint64_t RecoveryBeginScreenLock(bool ambient);
void RecoveryEndScreenLock(uint64_t generation, bool keep_sensor_warm = false);
// Warm only the HAL while asleep; authentication still belongs to a lock generation.
void RecoveryPrepareScreenFingerprint();
void RecoveryShutdownScreenFingerprint();
lock_security::Result RecoveryVerifyScreenLock(uint64_t generation, const std::string& secret);
bool RecoveryConsumeScreenUnlock(uint64_t generation);
struct ScreenFingerprint {
 bool ready=false, authenticated=false, illumination=false;
 int x=0,y=0,radius=0,display_width=0,display_height=0;
 std::string message;
};
ScreenFingerprint RecoveryPollScreenFingerprint(uint64_t generation);
void RecoveryScreenFingerprintTouch(uint64_t generation, bool down, int x, int y);
}
