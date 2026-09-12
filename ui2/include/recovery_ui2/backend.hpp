/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recovery_ui2 {
enum class Job {
  kInstall,
  kFlashImage,
  kBackup,
  kRestore,
  kWipe,
  kMount,
  kUnmount,
  kFormatData
};
struct Volume {
  std::string name;
  std::string path;
  uint64_t bytes = 0;
  bool selected = false;
};
struct JobRequest {
  Job job = Job::kInstall;
  std::string title;
  std::string path;
  std::vector<std::string> partitions;
  bool compression = true;
  bool both_slots = false;
  std::string confirmation;
};
inline bool FormatDataAuthorized(const JobRequest &request) {
  return request.job == Job::kFormatData && request.path == "/data" &&
         request.confirmation == "yes" && request.partitions.empty();
}
// Implemented beside the stock GUI bridge, using the same recovery backend.
std::vector<Volume> RecoveryVolumes(const std::string &kind);
std::vector<Volume> RecoveryImageVolumes();
std::vector<Volume> RecoveryRestoreVolumes(const std::string &folder);
std::string RecoveryStorage();
std::string RecoveryBackupRoot();
std::string RecoverySlot();
bool RecoverySetActiveSlot(const std::string &slot);
bool RecoveryDataLocked();
bool RecoverySetStorage(const std::string &path);
int RecoveryRunJob(const JobRequest &request);
int RecoveryProgress();
std::string RecoveryOperationDetail();
int RecoveryBrightness();
void RecoverySetBrightness(int percent);
bool RecoveryFlashlightSupported();
bool RecoveryFlashlightEnabled();
bool RecoverySetFlashlight(bool enabled);
bool RecoveryMtpEnabled();
bool RecoverySetMtp(bool enabled);
enum class Preference { kClock24, kHiddenFiles, kCompression, kSha256, kVerifyZip };
bool RecoveryPreference(Preference preference);
bool RecoverySetPreference(Preference preference, bool enabled);
bool RecoverySha256Available();
int RecoveryUtcOffset();  // Minutes east of UTC.
bool RecoverySetUtcOffset(int minutes);
uint32_t RecoveryAccentColor();
bool RecoverySetAccentColor(uint32_t rgb);
bool RecoveryLightMode();
bool RecoverySetLightMode(bool enabled);
enum class InterfaceSize { kSmall = 0, kNormal = 1, kLarge = 2 };
InterfaceSize RecoveryInterfaceSize();
bool RecoverySetInterfaceSize(InterfaceSize size);
enum class KeyboardLayout { kQwerty = 0, kQwertz = 1 };
KeyboardLayout RecoveryKeyboardLayout();
bool RecoverySetKeyboardLayout(KeyboardLayout layout);
int RecoveryHomeGridColumns();
bool RecoverySetHomeGridColumns(int columns);
enum class DockLayout { kGlass = 0, kCompact = 1, kMinimal = 2 };
DockLayout RecoveryDockLayout();
bool RecoverySetDockLayout(DockLayout layout);
int RecoveryDockTransparency();
bool RecoverySetDockTransparency(int percent);
int RecoveryDockBlur();
bool RecoverySetDockBlur(int percent);
bool RecoveryDockHideInApps();
bool RecoverySetDockHideInApps(bool enabled);
bool RecoverySavePreferences();
enum class Haptic { kTouch, kKeyboard, kAction };
bool RecoveryHapticsAvailable();
int RecoveryHapticDuration(Haptic haptic);
bool RecoverySetHapticDuration(Haptic haptic, int milliseconds);
void RecoveryVibrate(Haptic haptic);

// Thin frontend bridge to OrangeFox's existing libvterm + PTY terminal. AERA
// owns only presentation; the shell and terminal state remain stock.
enum class TerminalKey { kUp, kDown, kLeft, kRight, kTab, kEscape, kInterrupt };
void RecoveryTerminalStart(int columns, int rows, int pixel_width,
                           int pixel_height);
bool RecoveryTerminalPoll();
int RecoveryTerminalUpdateCounter();
std::vector<std::string> RecoveryTerminalLines(size_t maximum_lines);
void RecoveryTerminalWrite(const std::string &text);
void RecoveryTerminalSendKey(TerminalKey key);
void RecoveryTerminalClear();
bool RecoveryTerminalRunning();

// Native AERA presentation for OrangeFox 16's existing supplicant, DHCP and
// encrypted credential store. UI code never shells out or persists passwords.
struct WifiNetwork {
  std::string ssid;
  std::string security;
  bool saved = false;
  bool connected = false;
};
struct WifiStatus {
  bool supported = false;
  bool enabled = false;
  bool connected = false;
  bool busy = false;
  std::string state;
  std::string ssid;
  std::string ip_address;
  std::string test_result;
  std::vector<WifiNetwork> networks;
};
struct WifiConnection {
  bool connected = false;
  std::string ssid;
};
enum class WifiOperation {
  kEnable, kDisable, kDisconnect, kScan, kConnect, kForget, kTest
};
struct WifiRequest {
  WifiOperation operation = WifiOperation::kScan;
  std::string ssid;
  std::string password;
  bool use_saved_credentials = false;
};
void RecoveryWifiInitialize();
WifiConnection RecoveryWifiConnection();
WifiStatus RecoveryWifiStatus();
int RecoveryRunWifi(const WifiRequest &request);
bool RecoveryWifiAutoEnable();
bool RecoveryWifiAutoConnect();
bool RecoverySetWifiAutoEnable(bool enabled);
bool RecoverySetWifiAutoConnect(bool enabled);

// Native presentation of OrangeFox's existing rclone/FUSE NAS manager.
struct NasConfig {
  std::string type = "sftp";
  std::string host;
  std::string port = "22";
  std::string share = "Backups";
  std::string path;
  std::string user;
  std::string password;
  std::string domain = "WORKGROUP";
  std::string cache_mode = "off";
};
struct NasStatus {
  bool supported = false;
  bool mounted = false;
  bool selected = false;
  std::string status;
  std::string error;
  NasConfig config;
};
enum class NasOperation { kMountAndUse, kUse, kUnmount };
struct NasRequest { NasOperation operation = NasOperation::kMountAndUse; };
NasStatus RecoveryNasStatus();
bool RecoverySetNasConfig(const NasConfig &config, std::string *error);
int RecoveryRunNas(const NasRequest &request);
}  // namespace recovery_ui2
