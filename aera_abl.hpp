// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aera_efisp {
// Structural EFISP-loader detection; not OEM-signature or rollback verification.
enum class AblStatus { Unknown, EfispLoader, NoEfispLoader };
struct AblSample {
  std::string name, digest;
  AblStatus status = AblStatus::Unknown;
};
struct AblWarning { std::string text, token; };
const char* AblStatusName(AblStatus status);
AblStatus InspectAbl(const std::vector<unsigned char>& image);
// Opens only O_RDONLY. Regular files are accepted for host inspection/tests.
AblSample ReadAbl(const std::string& path, const std::string& name,
                 const std::vector<AblSample>& cache = {});
class AblSession {
 public:
  void Capture(const std::vector<AblSample>& samples);
  AblWarning Check(const std::vector<AblSample>& samples) const;
 private:
  bool captured_ = false;
  std::vector<AblSample> baseline_;
};
// Runtime discovery accepts only exact abl / abl_a / abl_b partition aliases.
// All access is read-only. Baseline survives live recovery/fastbootd transitions.
std::string CaptureAblBaseline();
AblWarning AblExitWarning();
std::vector<AblSample> CurrentAblSamples();
void AcknowledgeAblExit(const std::string& token);
void ClearAblExitAcknowledgement();
} // namespace aera_efisp
