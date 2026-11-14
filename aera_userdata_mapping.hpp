// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cerrno>
#include <sys/stat.h>

namespace aera::userdata_mapping {

enum class State { kAbsent, kPresent, kError };

// A dangling mapper link is still present. Only ENOENT proves absence;
// permission errors and malformed parent paths must not authorize a format.
inline State Probe(const char* path) {
  struct stat status;
  if (lstat(path, &status) == 0) return State::kPresent;
  return errno == ENOENT ? State::kAbsent : State::kError;
}

// Keep the existing device-tree selection and priority. Forced removal remains
// opt-in through AERA_USE_DMSETUP; no new removal policy is enabled by default.
#if defined(OF_USE_DMCTL) || defined(TW_USE_DMCTL)
inline constexpr const char* kCommand = "dmctl delete userdata";
#elif defined(AERA_USE_DMSETUP)
inline constexpr const char* kCommand = "dmsetup remove -f userdata";
#else
inline constexpr const char* kCommand = nullptr;
#endif

inline constexpr unsigned kPollIntervalUs = 50000;
inline constexpr unsigned kMaxPolls = 40;

// Operations supplies the command, probe, logging and sleep boundaries so the
// production policy can be tested without touching any real device mappings.
template <typename Operations>
bool Release(Operations& operations) {
  if (kCommand == nullptr) return true;
  const State initial = operations.GetState();
  if (initial == State::kAbsent) return true;
  if (initial == State::kError) return false;

  const int result = operations.Remove(kCommand);
  operations.LogResult(kCommand, result);
  if (result != 0) return false;

  for (unsigned poll = 0; poll <= kMaxPolls; ++poll) {
    const State state = operations.GetState();
    if (state == State::kAbsent) return true;
    if (state == State::kError) return false;
    if (poll != kMaxPolls) operations.Sleep(kPollIntervalUs);
  }
  return false;
}

}  // namespace aera::userdata_mapping
