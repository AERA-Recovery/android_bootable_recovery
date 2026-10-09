/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>
#include <aeraui/backend.hpp>

namespace aera::pc {
struct Status {
  bool enabled = false;
  bool remember_available = false;
  bool manually_disabled = false;
  std::string address;
  std::string ip_address;
  std::string certificate;
  std::string error;
  std::string pending_connection;
};
struct ConnectionRequest {
  std::string id;
  std::string name;
  std::string fingerprint;
  bool remember_available = false;
};
struct InstallRequest {
  std::string id;
  std::string name;
  std::string computer;
  uint64_t bytes = 0;
  aeraui::JobRequest job;
};
bool SetEnabled(bool enabled, int port = 443, bool manual = true);
// AERA Remote takes priority on port 80; the PC workspace remains on HTTPS.
void ReleaseHttpRedirect();
void RestoreHttpRedirect();
std::string HttpRedirectLocation(const std::string &host);
Status GetStatus();
bool NeedsUiAttention();
void SetRecoveryBusy(bool busy);
bool TakeRebootRequest(std::string *target);
// These methods are called only by the UI thread, never by network handlers.
void RefreshPlatformInfo();
bool TakeConnectionRequest(ConnectionRequest *request);
void ResolveConnection(const std::string &id, bool allow, bool remember);
bool TakeInstallRequest(InstallRequest *request);
bool BeginInstall(const std::string &id);
void RejectInstall(const std::string &id);
void UpdateInstall(int percent, const std::string &detail, const std::string &log);
void CompleteInstall(const std::string &id, int result);
void UpdateInstallerPrompt(const aeraui::InstallerPrompt &prompt);
void UpdateInstallerPresentation(const aeraui::InstallerPresentation &presentation);
bool TakePromptAnswer(std::string *id, bool *accepted);
bool ForgetComputers();
}  // namespace aera::pc
