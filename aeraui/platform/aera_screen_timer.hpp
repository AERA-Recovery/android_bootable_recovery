// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pthread.h>
#include <time.h>

#include <string>

class AeraScreenTimer {
 public:
  AeraScreenTimer();

  void SetTimeout(int seconds);
  void CheckTimeout();
  void Wake();
  void Toggle();
  void Blank();
  bool IsScreenOff();

 private:
  enum class State { kOn, kDim, kOff, kBlanked };

  void ResetClock();
  std::string CurrentBrightness();

  pthread_mutex_t mutex_{};
  State state_ = State::kOn;
  timespec started_{};
  int timeout_seconds_ = 0;
  std::string original_brightness_;
};

extern AeraScreenTimer aeraScreenTimer;
