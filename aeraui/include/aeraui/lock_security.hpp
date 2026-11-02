// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <mutex>
#include <string>

namespace aeraui::lock_security {
enum class Kind { None, Pin, Password };
struct Status {
  bool available = false, configured = false, valid = false, fingerprint = false;
  Kind kind = Kind::None;
  uint32_t retry_seconds = 0;
  std::string error;
};
struct Result { bool ok = false; std::string error; };
class Store {
 public:
  explicit Store(std::string directory);
  Status Inspect() const;
  Result Verify(const std::string& secret);
  Result Set(const std::string& current, Kind kind, const std::string& secret,
             const std::string& confirmation);
  Result Remove(const std::string& current);
  Result SetFingerprint(const std::string& current, bool enabled);
  static bool ValidSecret(Kind kind, const std::string& value);
 private:
  struct Record;
  Record Read() const;
  Result Write(const Record& record);
  Result Check(const Record& record, const std::string& value);
  uint32_t RetrySeconds() const;
  void Failed();
  std::string directory_;
  std::mutex operation_;
  mutable std::mutex rate_;
  unsigned failures_ = 0;
  uint64_t retry_at_ = 0;
};
} // namespace aeraui::lock_security
