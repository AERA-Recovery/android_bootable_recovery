/*
 * Copyright (C) 2026 Recovery UI2 contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

namespace recovery_ui2 {

enum class RunResult {
    kEngineFailure = -1,
    kUnexpectedExit = 1,
    kRebootSystem,
    kRebootRecovery,
    kRebootBootloader,
    kRebootFastbootd,
    kPowerOff,
};

enum class DecryptionResult {
    kUnavailable = -1,
    kSuccess = 0,
    kSkipped = 1,
};

// Starts the native renderer while the recovery core continues partition and
// decryption setup. Must be called after gr_init() and ev_init().
void StartRecoveryUi2Early();

// Displays AERA's native credential page and blocks recovery startup until
// data is unlocked or the user explicitly chooses to continue encrypted.
// credential_type uses the stock TW_CRYPTO_PWTYPE values (2 pattern, 3 PIN,
// all other non-zero values password).
DecryptionResult RunRecoveryUi2Decryption(int credential_type,
                                          bool file_based, int user_id,
                                          int pattern_grid_size);

// Marks the recovery backend ready, then joins the already-running native UI.
// Hardware Back and ordinary workflows stay inside the native engine.
RunResult RunRecoveryUi2();

// Runs a dedicated userspace-fastboot surface. This deliberately bypasses
// boot animation, decryption, recovery navigation and plugin initialization.
RunResult RunRecoveryUi2Fastboot();

}  // namespace recovery_ui2
