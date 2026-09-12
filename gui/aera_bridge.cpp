/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include <recovery_ui2/backend.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cutils/properties.h>

#include "../data.hpp"
#include "../partitions.hpp"
#include "../twrp-functions.hpp"
#include "../variables.h"
#include <set_metadata.h>
#include "gui.hpp"
#include "objects.hpp"
#include <twinstall.h>

#ifdef OF_ENABLE_WLAN
#include "../nas/NasManager.hpp"
#include "../wlan.hpp"
#endif

extern "C" int recovery_ui2_install_package(const char *path) {
  if (!path || !path[0]) return 1;
  int wipe_cache = 0;
  TWFunc::SetPerformanceMode(true);
  int result = TWinstall_zip(path, &wipe_cache, false);
  if (result == 0 && wipe_cache)
    result = PartitionManager.Wipe_By_Path("/cache") ? 0 : 1;
  PartitionManager.Update_System_Details();
  TWFunc::SetPerformanceMode(false);
  return result;
}

extern "C" int recovery_ui2_decrypt_data(const char *credential, int user_id) {
  if (!credential) return 1;
  const std::string password(credential);
  DataManager::SetValue("tw_crypto_password", password);
  DataManager::SetValue("tw_password_fail", 0);
  int result;
  if (DataManager::GetIntValue(TW_IS_FBE)) {
    DataManager::SetValue("tw_crypto_user_id", std::to_string(user_id));
    result = PartitionManager.Decrypt_Device(password, user_id);
    if (user_id != 0) return result == 0 ? 0 : 1;
  } else {
    result = PartitionManager.Decrypt_Device(password);
  }
  if (result != 0) {
    DataManager::SetValue("tw_password_fail", 1);
    return 1;
  }
  DataManager::SetValue(TW_IS_ENCRYPTED, 0);
  DataManager::SetValue(TW_IS_DECRYPTED, 1);
  DataManager::SetValue("data_decrypted", 1);
  DataManager::SetValue("tw_password_fail", 0);
  if (DataManager::GetIntValue(TW_HAS_DATA_MEDIA) != 0) {
    if (tw_get_default_metadata(DataManager::GetSettingsStoragePath().c_str()) != 0 &&
        tw_get_default_metadata(DataManager::GetCurrentStoragePath().c_str()) != 0)
      LOGINFO("Failed to get default contexts and file mode for storage files.\n");
  }
  PartitionManager.Decrypt_Adopted();
  PartitionManager.Update_System_Details();
  return 0;
}

namespace recovery_ui2 {
namespace {
void LoadAeraPreferencesIfAvailable();
bool SaveAeraPreferences();
}

std::vector<Volume> RecoveryVolumes(const std::string &kind) {
  std::vector<PartitionList> source;
  PartitionManager.Get_Partition_List(kind, &source);
  std::vector<Volume> result;
  for (const auto &part : source) {
    std::string name = part.Display_Name;
    const auto suffix = name.find(" (");
    if (suffix != std::string::npos) name.resize(suffix);
    if (part.Mount_Point == "DALVIK") name = "Dalvik / ART cache";
    if (part.Mount_Point == "INTERNAL") name = "Internal storage";
    if (name.empty() || name.find('{') != std::string::npos) name = part.Mount_Point;
    result.push_back({name, part.Mount_Point, part.PartitionSize, part.selected != 0});
  }
  return result;
}

std::vector<Volume> RecoveryImageVolumes() {
  static const std::vector<std::string> allowed = {
      "/boot", "/init_boot", "/vendor_boot", "/recovery", "/dtbo", "/abl"};
  const auto flashable = RecoveryVolumes("flashimg");
  std::vector<Volume> result;
  for (const auto &path : allowed) {
    const auto found = std::find_if(
        flashable.begin(), flashable.end(), [&](const Volume &volume) {
          return volume.path == path;
        });
    TWPartition *partition = PartitionManager.Find_Partition_By_Path(path);
    if (found != flashable.end() && partition && partition->Is_SlotSelect())
      result.push_back(*found);
  }
  return result;
}

std::string RecoveryStorage() { return DataManager::GetCurrentStoragePath(); }
std::string RecoveryBackupRoot() { return DataManager::GetStrValue(TW_BACKUPS_FOLDER_VAR); }
std::string RecoverySlot() { return PartitionManager.Get_Active_Slot_Display(); }
bool RecoverySetActiveSlot(const std::string &slot) {
  if (slot != "A" && slot != "B") return false;
  if (RecoverySlot() == slot) return true;

  // Match OrangeFox's setbootslot action: /vendor may hold resources from the
  // current slot and must be detached before PartitionManager updates it.
  if (PartitionManager.Find_Partition_By_Path("/vendor") &&
      !PartitionManager.UnMount_By_Path("/vendor", false)) {
    LOGINFO("AERA: /vendor did not unmount normally; using lazy unmount.\n");
    umount2("/vendor", MNT_DETACH);
  }
  PartitionManager.Set_Active_Slot(slot);
  return RecoverySlot() == slot;
}
bool RecoveryDataLocked() {
  return DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0 &&
         DataManager::GetIntValue(TW_IS_DECRYPTED) == 0;
}

bool RecoverySetStorage(const std::string &path) {
  const auto volumes = RecoveryVolumes("storage");
  const auto found = std::find_if(volumes.begin(), volumes.end(),
      [&](const Volume &volume) { return volume.path == path; });
  if (found == volumes.end() || !PartitionManager.Mount_By_Path(path, true)) return false;
  DataManager::SetValue("tw_storage_path", path);
  return true;
}

std::vector<Volume> RecoveryRestoreVolumes(const std::string &folder) {
  DataManager::SetValue("tw_restore_list", "");
  DataManager::SetValue("tw_restore_selected", "");
  PartitionManager.Set_Restore_Files(folder);
  if (DataManager::GetIntValue("tw_restore_encrypted") != 0) return {};
  return RecoveryVolumes("restore");
}

int RecoveryRunJob(const JobRequest &request) {
  if (request.job == Job::kFormatData && !FormatDataAuthorized(request)) return 1;
  DataManager::SetValue("ui_progress", 0);
  DataManager::SetValue("ui_portion_start", 0.0f);
  DataManager::SetValue("ui_portion_size", request.job == Job::kInstall ? 0.0f : 1.0f);
  DataManager::SetValue("ui_progress_portion", 0);
  DataManager::SetValue("ui_progress_frames", 0);
  DataManager::SetValue("tw_operation", request.title);
  DataManager::SetValue("tw_partition", "");
  DataManager::SetValue("tw_size_progress", "");
  DataManager::SetValue("tw_file_progress", "");
  if (request.job == Job::kInstall) return recovery_ui2_install_package(request.path.c_str());
  if (request.job == Job::kFlashImage) {
    if (request.partitions.size() != 1 || request.path.empty() ||
        request.path.find_first_of("'\r\n") != std::string::npos) return 1;
    struct stat image_info {};
    if (stat(request.path.c_str(), &image_info) != 0 ||
        !S_ISREG(image_info.st_mode) || image_info.st_size <= 0) return 1;
    std::string lower_path = request.path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(),
                   [](unsigned char value) {
                     return static_cast<char>(std::tolower(value));
                   });
    if (lower_path.size() < 4 ||
        lower_path.compare(lower_path.size() - 4, 4, ".img") != 0) return 1;

    const auto targets = RecoveryImageVolumes();
    const std::string &target = request.partitions.front();
    if (target.find(';') != std::string::npos ||
        std::none_of(targets.begin(), targets.end(),
                     [&](const Volume &volume) {
                       return volume.path == target;
                     })) return 1;
    const size_t slash = request.path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= request.path.size()) return 1;
    std::string directory = slash == 0 ? "/" : request.path.substr(0, slash);
    std::string filename = request.path.substr(slash + 1);
    TWPartition *flash_partition = PartitionManager.Find_Partition_By_Path(target);
    if (!flash_partition || !flash_partition->Is_SlotSelect()) return 1;
    DataManager::SetValue("tw_flash_partition", target + ";");
    DataManager::SetValue("tw_flash_both_slots", request.both_slots ? 1 : 0);
    DataManager::SetValue("tw_partition", target);
    TWFunc::SetPerformanceMode(true);
    int result = 1;
    if (request.both_slots) {
      const std::string original_slot = PartitionManager.Get_Active_Slot_Display();
      const bool first_slot_ok = PartitionManager.Flash_Image(directory, filename);
      bool second_slot_ok = false;
      if (first_slot_ok) {
        PartitionManager.Override_Active_Slot(original_slot == "A" ? "B" : "A");
        second_slot_ok = PartitionManager.Flash_Image(directory, filename);
      }
      PartitionManager.Override_Active_Slot(original_slot);
      result = first_slot_ok && second_slot_ok ? 0 : 1;
    } else {
      result = PartitionManager.Flash_Image(directory, filename) ? 0 : 1;
    }
    DataManager::SetValue("tw_flash_both_slots", 0);
    PartitionManager.Update_System_Details();
    TWFunc::SetPerformanceMode(false);
    return result;
  }
  if (request.job == Job::kFormatData) {
    DataManager::SetValue("tw_partition", "/data");
    char fastboot_mode[PROPERTY_VALUE_MAX] = {};
    property_get(TW_FASTBOOT_MODE_PROP, fastboot_mode, "0");
    const bool restore_fastboot = fastboot_mode[0] == '1' &&
                                  fastboot_mode[1] == '\0';
    // Formatting must never race a host-side fastboot write. Detach the USB
    // function and stop fastbootd for the duration, then publish it again once
    // the partition manager has finished updating storage state.
    if (restore_fastboot) {
      property_set("sys.usb.config", "none");
      usleep(300000);
    }
    TWFunc::SetPerformanceMode(true);
    const int result = PartitionManager.Format_Data() ? 0 : 1;
    PartitionManager.Update_System_Details();
    TWFunc::SetPerformanceMode(false);
    if (restore_fastboot) property_set("sys.usb.config", "fastboot");
    return result;
  }
  if (request.job == Job::kMount || request.job == Job::kUnmount) {
    const auto mounts = RecoveryVolumes("mount");
    const auto found = std::find_if(mounts.begin(), mounts.end(),
        [&](const Volume &volume) { return volume.path == request.path; });
    if (found == mounts.end()) return 1;
    return (request.job == Job::kMount
        ? PartitionManager.Mount_By_Path(request.path, true)
        : PartitionManager.UnMount_By_Path(request.path, true)) ? 0 : 1;
  }
  if (request.partitions.empty()) return 1;
  std::vector<Volume> allowed = request.job == Job::kRestore
      ? RecoveryRestoreVolumes(request.path)
      : RecoveryVolumes(request.job == Job::kBackup ? "backup" : "wipe");
  std::string selections;
  for (const auto &path : request.partitions) {
    if (std::none_of(allowed.begin(), allowed.end(),
        [&](const Volume &volume) { return volume.path == path; }) ||
        path.find(';') != std::string::npos) return 1;
    selections += path + ";";
  }
  int result = 1;
  TWFunc::SetPerformanceMode(true);
  if (request.job == Job::kBackup) {
    if (RecoverySetStorage(request.path)) {
      char name[80];
      time_t now = time(nullptr);
      struct tm local = {};
      localtime_r(&now, &local);
      strftime(name, sizeof(name), "AERA-%Y-%m-%d-%H-%M-%S", &local);
      DataManager::SetValue(TW_BACKUP_NAME, name);
      DataManager::SetValue("tw_backup_list", selections);
      DataManager::SetValue(TW_USE_COMPRESSION_VAR, request.compression ? 1 : 0);
      DataManager::SetValue(TW_SKIP_DIGEST_GENERATE_VAR, 0);
      DataManager::SetValue("tw_encrypt_backup", 0);
      if (PartitionManager.Check_Backup_Name(name, true, true) == 0)
        result = PartitionManager.Run_Backup(false) ? 0 : 1;
    }
  } else if (request.job == Job::kRestore) {
    DataManager::SetValue("tw_restore", request.path);
    DataManager::SetValue("tw_restore_selected", selections);
    DataManager::SetValue(TW_SKIP_DIGEST_CHECK_VAR, 1);
    result = PartitionManager.Run_Restore(request.path) ? 0 : 1;
  } else if (request.job == Job::kWipe) {
    result = 0;
    for (const auto &path : request.partitions) {
      int ok;
      if (path == "DALVIK") ok = PartitionManager.Wipe_Dalvik_Cache();
      else if (path == "INTERNAL") ok = PartitionManager.Wipe_Media_From_Data();
      else if (path == "/and-sec") ok = PartitionManager.Wipe_Android_Secure();
      else ok = PartitionManager.Wipe_By_Path(path);
      if (!ok) { result = 1; break; }
    }
  }
  PartitionManager.Update_System_Details();
  TWFunc::SetPerformanceMode(false);
  return result;
}

int RecoveryProgress() {
  return std::max(0, std::min(100, atoi(DataManager::GetStrValue("ui_progress").c_str())));
}
std::string RecoveryOperationDetail() {
  std::string text = DataManager::GetStrValue("tw_operation");
  const auto partition = DataManager::GetStrValue("tw_partition");
  if (!partition.empty()) text += " / " + partition;
  const auto size = DataManager::GetStrValue("tw_size_progress");
  if (!size.empty()) text += "\n" + size;
  const auto files = DataManager::GetStrValue("tw_file_progress");
  if (!files.empty()) text += "\n" + files;
  return text;
}
int RecoveryBrightness() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("tw_brightness_pct");
}
void RecoverySetBrightness(int percent) {
  percent = std::max(10, std::min(100, percent));
  const int maximum = DataManager::GetIntValue("tw_brightness_max");
  if (maximum <= 0) return;
  DataManager::SetValue("tw_brightness_pct", percent);
  DataManager::SetValue("tw_brightness", maximum * percent / 100);
  TWFunc::Set_Brightness(DataManager::GetStrValue("tw_brightness"));
}

bool RecoveryFlashlightSupported() {
  return DataManager::GetIntValue(OF_FLASHLIGHT_ENABLE_STR) == 1;
}
bool RecoveryFlashlightEnabled() {
  return RecoveryFlashlightSupported() &&
         DataManager::GetIntValue("of_flash_on") == 1;
}
bool RecoverySetFlashlight(bool enabled) {
  if (!RecoveryFlashlightSupported()) return false;
  if (RecoveryFlashlightEnabled() != enabled)
    GUIAction::flashlightImpl("");
  return RecoveryFlashlightEnabled() == enabled;
}
bool RecoveryMtpEnabled() {
#ifdef TW_HAS_MTP
  return PartitionManager.is_MTP_Enabled();
#else
  return false;
#endif
}
bool RecoverySetMtp(bool enabled) {
  const bool result = enabled ? PartitionManager.Enable_MTP() : PartitionManager.Disable_MTP();
  if (result) DataManager::SetValue("tw_mtp_enabled", enabled ? 1 : 0);
  return result;
}
static const char *PreferenceVariable(Preference preference) {
  switch (preference) {
    case Preference::kClock24: return "tw_military_time";
    case Preference::kHiddenFiles: return "tw_hidden_files";
    case Preference::kCompression: return TW_USE_COMPRESSION_VAR;
    case Preference::kSha256: return TW_USE_SHA2;
    case Preference::kVerifyZip: return TW_SIGNED_ZIP_VERIFY_VAR;
  }
  return nullptr;
}
bool RecoveryPreference(Preference preference) {
  LoadAeraPreferencesIfAvailable();
  const char *variable = PreferenceVariable(preference);
  return variable && DataManager::GetIntValue(variable) != 0;
}
bool RecoverySha256Available() { return DataManager::GetIntValue(TW_NO_SHA2) == 0; }
bool RecoverySetPreference(Preference preference, bool enabled) {
  const char *variable = PreferenceVariable(preference);
  if (!variable || (preference == Preference::kSha256 && !RecoverySha256Available())) return false;
  return DataManager::SetValue(variable, enabled ? 1 : 0) == 0;
}
int RecoveryUtcOffset() {
  LoadAeraPreferencesIfAvailable();
  time_t now = time(nullptr);
  struct tm local = {};
  return localtime_r(&now, &local) ? static_cast<int>(local.tm_gmtoff / 60) : 0;
}
bool RecoverySetUtcOffset(int minutes) {
  if (minutes < -720 || minutes > 840 || minutes % 15 != 0) return false;
  char zone[32];
  const int magnitude = std::abs(minutes);
  snprintf(zone, sizeof(zone), "UTC%c%d:%02d", minutes < 0 ? '+' : '-',
           magnitude / 60, magnitude % 60);
  if (DataManager::SetValue(TW_TIME_ZONE_VAR, zone) != 0) return false;
  DataManager::update_tz_environment_variables();
  return true;
}
uint32_t RecoveryAccentColor() {
  LoadAeraPreferencesIfAvailable();
  constexpr uint32_t kDefault = 0x16c8ff;
  const std::string value = DataManager::GetStrValue("aera_theme_accent");
  if (value.empty()) return kDefault;
  char *end = nullptr;
  const unsigned long parsed = strtoul(value.c_str(), &end, 16);
  if (end == value.c_str() || *end != '\0' || parsed == 0 ||
      parsed > 0x00ffffffUL) return kDefault;
  return static_cast<uint32_t>(parsed);
}
bool RecoverySetAccentColor(uint32_t rgb) {
  rgb &= 0x00ffffffu;
  if (rgb == 0) return false;
  char value[7];
  snprintf(value, sizeof(value), "%06x", rgb);
  // Mark the value persistent immediately; RecoverySavePreferences() controls
  // when the in-memory settings map is flushed to storage.
  return DataManager::SetValue("aera_theme_accent", value, 1) == 0;
}
bool RecoveryLightMode() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetStrValue("aera_theme_mode") == "light";
}
bool RecoverySetLightMode(bool enabled) {
  return DataManager::SetValue("aera_theme_mode", enabled ? "light" : "graphite", 1) == 0;
}
InterfaceSize RecoveryInterfaceSize() {
  LoadAeraPreferencesIfAvailable();
  const int value = std::clamp(
      DataManager::GetIntValue("aera_interface_size"), 0, 2);
  // DataManager returns zero for an unset integer. Preserve today's UI as the
  // default by storing human-readable names and treating empty as Normal.
  const std::string stored = DataManager::GetStrValue("aera_interface_size");
  return stored.empty() ? InterfaceSize::kNormal
                        : static_cast<InterfaceSize>(value);
}
bool RecoverySetInterfaceSize(InterfaceSize size) {
  const int value = static_cast<int>(size);
  return value >= 0 && value <= 2 &&
      DataManager::SetValue("aera_interface_size", value, 1) == 0;
}
KeyboardLayout RecoveryKeyboardLayout() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetStrValue("aera_keyboard_layout") == "qwertz"
      ? KeyboardLayout::kQwertz : KeyboardLayout::kQwerty;
}
bool RecoverySetKeyboardLayout(KeyboardLayout layout) {
  if (layout != KeyboardLayout::kQwerty &&
      layout != KeyboardLayout::kQwertz) return false;
  return DataManager::SetValue(
      "aera_keyboard_layout",
      layout == KeyboardLayout::kQwertz ? "qwertz" : "qwerty", 1) == 0;
}
int RecoveryHomeGridColumns() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_home_grid_columns");
  if (stored.empty()) return 3;
  return atoi(stored.c_str()) == 2 ? 2 : 3;
}
bool RecoverySetHomeGridColumns(int columns) {
  if (columns != 2 && columns != 3) return false;
  return DataManager::SetValue("aera_home_grid_columns", columns, 1) == 0;
}
DockLayout RecoveryDockLayout() {
  LoadAeraPreferencesIfAvailable();
  const int value = std::clamp(DataManager::GetIntValue("aera_dock_layout"), 0, 2);
  return static_cast<DockLayout>(value);
}
bool RecoverySetDockLayout(DockLayout layout) {
  const int value = static_cast<int>(layout);
  return value >= 0 && value <= 2 &&
      DataManager::SetValue("aera_dock_layout", value, 1) == 0;
}
int RecoveryDockTransparency() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_dock_transparency");
  return stored.empty() ? 60 : std::clamp(atoi(stored.c_str()), 0, 100);
}
bool RecoverySetDockTransparency(int percent) {
  return DataManager::SetValue("aera_dock_transparency",
                               std::clamp(percent, 0, 100), 1) == 0;
}
int RecoveryDockBlur() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_dock_blur");
  return stored.empty() ? 24 : std::clamp(atoi(stored.c_str()), 0, 100);
}
bool RecoverySetDockBlur(int percent) {
  return DataManager::SetValue("aera_dock_blur",
                               std::clamp(percent, 0, 100), 1) == 0;
}
bool RecoveryDockHideInApps() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("aera_dock_hide_apps") != 0;
}
bool RecoverySetDockHideInApps(bool enabled) {
  return DataManager::SetValue("aera_dock_hide_apps", enabled ? 1 : 0, 1) == 0;
}
bool RecoverySavePreferences() { return SaveAeraPreferences(); }

namespace {
const char *HapticVariable(Haptic haptic) {
  switch (haptic) {
    case Haptic::kTouch: return "tw_button_vibrate";
    case Haptic::kKeyboard: return "tw_keyboard_vibrate";
    case Haptic::kAction: return "tw_action_vibrate";
  }
  return nullptr;
}
}

bool RecoveryHapticsAvailable() {
  return DataManager::GetIntValue("tw_disable_haptics") == 0;
}

int RecoveryHapticDuration(Haptic haptic) {
  LoadAeraPreferencesIfAvailable();
  const char *variable = HapticVariable(haptic);
  return variable ? std::max(0, DataManager::GetIntValue(variable)) : 0;
}

bool RecoverySetHapticDuration(Haptic haptic, int milliseconds) {
  if (!RecoveryHapticsAvailable()) return false;
  const char *variable = HapticVariable(haptic);
  if (variable == nullptr) return false;
  const int maximum = haptic == Haptic::kAction ? 500 : 300;
  milliseconds = std::clamp(milliseconds, 0, maximum);
  return DataManager::SetValue(variable, milliseconds) == 0;
}

void RecoveryVibrate(Haptic haptic) {
  if (!RecoveryHapticsAvailable()) return;
  const char *variable = HapticVariable(haptic);
  if (variable != nullptr) DataManager::Vibrate(variable);
}

namespace {
constexpr const char *kAeraPreferencesPath =
    "/data/media/0/AERA/preferences.conf";
constexpr const char *kNasProfilePath =
    "/data/media/0/AERA/network_storage.conf";

void LoadAeraPreferencesIfAvailable() {
  static bool loaded = false;
  if (loaded) return;
  std::ifstream input(kAeraPreferencesPath);
  // Before decryption /data/media/0 is intentionally unavailable. Leave the
  // flag clear so the first post-decryption scene retries automatically.
  if (!input) return;

  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (key == "clock24") DataManager::SetValue("tw_military_time", value);
    else if (key == "hidden") DataManager::SetValue("tw_hidden_files", value);
    else if (key == "compression") DataManager::SetValue(TW_USE_COMPRESSION_VAR, value);
    else if (key == "sha256") DataManager::SetValue(TW_USE_SHA2, value);
    else if (key == "verify_zip") DataManager::SetValue(TW_SIGNED_ZIP_VERIFY_VAR, value);
    else if (key == "timezone") DataManager::SetValue(TW_TIME_ZONE_VAR, value);
    else if (key == "brightness") DataManager::SetValue("tw_brightness_pct", value);
    else if (key == "accent") DataManager::SetValue("aera_theme_accent", value);
    else if (key == "theme") DataManager::SetValue("aera_theme_mode", value);
    else if (key == "interface_size") DataManager::SetValue("aera_interface_size", value);
    else if (key == "keyboard_layout") DataManager::SetValue("aera_keyboard_layout", value);
    else if (key == "home_grid_columns") DataManager::SetValue("aera_home_grid_columns", value);
    else if (key == "dock_layout") DataManager::SetValue("aera_dock_layout", value);
    else if (key == "dock_transparency") DataManager::SetValue("aera_dock_transparency", value);
    else if (key == "dock_blur") DataManager::SetValue("aera_dock_blur", value);
    else if (key == "dock_hide_apps") DataManager::SetValue("aera_dock_hide_apps", value);
    else if (key == "haptic_touch") DataManager::SetValue("tw_button_vibrate", value);
    else if (key == "haptic_keyboard") DataManager::SetValue("tw_keyboard_vibrate", value);
    else if (key == "haptic_action") DataManager::SetValue("tw_action_vibrate", value);
    else if (key == "wifi_auto_enable") DataManager::SetValue("of_wlan_auto_enable", value);
    else if (key == "wifi_auto_connect") DataManager::SetValue("of_wlan_auto_connect", value);
    else if (key == "wifi_last_ssid") DataManager::SetValue("of_wlan_last_ssid", value);
  }
  loaded = true;
  DataManager::update_tz_environment_variables();
  const int percent = std::clamp(
      DataManager::GetIntValue("tw_brightness_pct"), 10, 100);
  const int maximum = DataManager::GetIntValue("tw_brightness_max");
  if (maximum > 0) {
    DataManager::SetValue("tw_brightness", maximum * percent / 100);
    TWFunc::Set_Brightness(DataManager::GetStrValue("tw_brightness"));
  }
  LOGINFO("AERA: restored preferences from shared storage.\n");
}

bool SaveAeraPreferences() {
  constexpr const char *directory = "/data/media/0/AERA";
  if (!TWFunc::Recursive_Mkdir(directory, false)) return false;
  const std::string temporary = std::string(kAeraPreferencesPath) + ".tmp";
  std::ofstream output(temporary, std::ios::out | std::ios::trunc);
  if (!output) return false;
  output << "clock24=" << DataManager::GetIntValue("tw_military_time") << '\n'
         << "hidden=" << DataManager::GetIntValue("tw_hidden_files") << '\n'
         << "compression=" << DataManager::GetIntValue(TW_USE_COMPRESSION_VAR) << '\n'
         << "sha256=" << DataManager::GetIntValue(TW_USE_SHA2) << '\n'
         << "verify_zip=" << DataManager::GetIntValue(TW_SIGNED_ZIP_VERIFY_VAR) << '\n'
         << "timezone=" << DataManager::GetStrValue(TW_TIME_ZONE_VAR) << '\n'
         << "brightness=" << DataManager::GetIntValue("tw_brightness_pct") << '\n'
         << "accent=" << DataManager::GetStrValue("aera_theme_accent") << '\n'
         << "theme=" << DataManager::GetStrValue("aera_theme_mode") << '\n'
         << "interface_size=" << static_cast<int>(RecoveryInterfaceSize()) << '\n'
         << "keyboard_layout="
         << (RecoveryKeyboardLayout() == KeyboardLayout::kQwertz
                 ? "qwertz" : "qwerty") << '\n'
         << "home_grid_columns=" << RecoveryHomeGridColumns() << '\n'
         << "dock_layout=" << DataManager::GetIntValue("aera_dock_layout") << '\n'
         << "dock_transparency=" << RecoveryDockTransparency() << '\n'
         << "dock_blur=" << RecoveryDockBlur() << '\n'
         << "dock_hide_apps=" << (RecoveryDockHideInApps() ? 1 : 0) << '\n'
         << "haptic_touch=" << DataManager::GetIntValue("tw_button_vibrate") << '\n'
         << "haptic_keyboard=" << DataManager::GetIntValue("tw_keyboard_vibrate") << '\n'
         << "haptic_action=" << DataManager::GetIntValue("tw_action_vibrate") << '\n'
         << "wifi_auto_enable=" << DataManager::GetIntValue("of_wlan_auto_enable") << '\n'
         << "wifi_auto_connect=" << DataManager::GetIntValue("of_wlan_auto_connect") << '\n'
         << "wifi_last_ssid=" << DataManager::GetStrValue("of_wlan_last_ssid") << '\n';
  output.flush();
  if (!output) return false;
  output.close();
  if (rename(temporary.c_str(), kAeraPreferencesPath) != 0) return false;
  chmod(kAeraPreferencesPath, 0600);
  LOGINFO("AERA: saved preferences to shared storage.\n");
  return true;
}

bool LoadNasProfile(NasConfig *config) {
  if (config == nullptr) return false;
  std::ifstream input(kNasProfilePath);
  if (!input) return false;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "type") config->type = value;
    else if (key == "host") config->host = value;
    else if (key == "port") config->port = value;
    else if (key == "share") config->share = value;
    else if (key == "path") config->path = value;
    else if (key == "user") config->user = value;
    else if (key == "domain") config->domain = value;
    else if (key == "cache") config->cache_mode = value;
  }
  return !config->host.empty();
}

bool SaveNasProfile(const NasConfig &config) {
  constexpr const char *directory = "/data/media/0/AERA";
  // OrangeFox's Recursive_Mkdir returns 1 on success and 0 on failure.
  if (!TWFunc::Recursive_Mkdir(directory, false)) return false;
  const std::string temporary = std::string(kNasProfilePath) + ".tmp";
  std::ofstream output(temporary, std::ios::out | std::ios::trunc);
  if (!output) return false;
  output << "type=" << config.type << '\n'
         << "host=" << config.host << '\n'
         << "port=" << config.port << '\n'
         << "share=" << config.share << '\n'
         << "path=" << config.path << '\n'
         << "user=" << config.user << '\n'
         << "domain=" << config.domain << '\n'
         << "cache=" << config.cache_mode << '\n';
  output.flush();
  if (!output) return false;
  output.close();
  if (rename(temporary.c_str(), kNasProfilePath) != 0) return false;
  chmod(kNasProfilePath, 0600);
  return true;
}

std::string ReadText(const std::string &path) {
  std::ifstream input(path);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}
std::vector<std::string> Lines(const std::string &text) {
  std::vector<std::string> result;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) result.push_back(line);
  }
  return result;
}
}  // namespace

void RecoveryWifiInitialize() {
#ifdef OF_ENABLE_WLAN
  LoadAeraPreferencesIfAvailable();
  Wlan::Init();
#endif
}

WifiConnection RecoveryWifiConnection() {
  WifiConnection connection;
#ifdef OF_ENABLE_WLAN
  connection.connected = DataManager::GetIntValue("tw_wlan_connected") == 1;
  connection.ssid = DataManager::GetStrValue("wlan_connected_name");
#endif
  return connection;
}

WifiStatus RecoveryWifiStatus() {
  WifiStatus status;
#ifdef OF_ENABLE_WLAN
  status.supported = true;
  status.enabled = Wlan::IsEnabled();
  status.connected = DataManager::GetIntValue("tw_wlan_connected") == 1;
  status.state = DataManager::GetStrValue("wlan_state");
  status.ssid = DataManager::GetStrValue("wlan_connected_name");
  status.ip_address = DataManager::GetStrValue("wlan_info_ip");
  status.busy = status.state == "enabling" || status.state == "disabling" ||
                status.state == "disconnecting" ||
                status.state == "scanning" || status.state == "connecting";
  std::set<std::string> saved;
  for (const auto &ssid : Lines(ReadText("/tmp/wlan/saved.txt"))) saved.insert(ssid);
  for (const auto &ssid : Lines(ReadText("/tmp/wlan/list.txt"))) {
    WifiNetwork network;
    network.ssid = ssid;
    network.security = Lines(ReadText("/tmp/wlan/list/" + ssid)).empty()
        ? "UNKNOWN" : Lines(ReadText("/tmp/wlan/list/" + ssid)).front();
    network.saved = saved.count(ssid) != 0;
    network.connected = status.connected && status.ssid == ssid;
    status.networks.push_back(std::move(network));
  }
  for (const auto &ssid : saved) {
    if (std::none_of(status.networks.begin(), status.networks.end(),
        [&](const WifiNetwork &network) { return network.ssid == ssid; }))
      status.networks.push_back({ssid, "SAVED", true, status.connected && status.ssid == ssid});
  }
  std::stable_sort(status.networks.begin(), status.networks.end(),
      [](const WifiNetwork &a, const WifiNetwork &b) {
        if (a.connected != b.connected) return a.connected;
        if (a.saved != b.saved) return a.saved;
        return strcasecmp(a.ssid.c_str(), b.ssid.c_str()) < 0;
      });
  for (int i = 1; i <= 5; ++i) {
    const std::string line = DataManager::GetStrValue("wlan_test_line" + std::to_string(i));
    if (!line.empty()) status.test_result += (status.test_result.empty() ? "" : "\n") + line;
  }
#endif
  return status;
}

int RecoveryRunWifi(const WifiRequest &request) {
#ifndef OF_ENABLE_WLAN
  (void)request;
  return 1;
#else
  bool result = false;
  switch (request.operation) {
    case WifiOperation::kEnable:
      DataManager::SetValue("wlan_state", "enabling");
      result = Wlan::Enable();
      if (result) {
        DataManager::SetValue("wlan_state", "scanning");
        Wlan::Scan();
      }
      break;
    case WifiOperation::kDisable:
      DataManager::SetValue("wlan_state", "disabling");
      result = Wlan::Disable();
      break;
    case WifiOperation::kDisconnect:
      DataManager::SetValue("wlan_state", "disconnecting");
      result = Wlan::Disconnect();
      break;
    case WifiOperation::kScan:
      DataManager::SetValue("wlan_state", "scanning");
      result = Wlan::Scan();
      break;
    case WifiOperation::kConnect:
      DataManager::SetValue("wlan_state", "connecting");
      DataManager::SetValue("wlanselectedid", request.ssid);
      DataManager::SetValue("wlan_password", request.password);
      result = request.use_saved_credentials ? Wlan::ConnectSaved() : Wlan::Connect();
      DataManager::SetValue("wlan_password", "");
      if (result) SaveAeraPreferences();
      break;
    case WifiOperation::kForget:
      DataManager::SetValue("wlanselectedid", request.ssid);
      result = Wlan::ForgetSaved();
      break;
    case WifiOperation::kTest:
      result = Wlan::TestConnection();
      if (result) {
        result = DataManager::GetStrValue("wlan_test_line3") == "Internet: OK" &&
                 DataManager::GetStrValue("wlan_test_line4") == "DNS: OK";
      }
      break;
  }
  const bool enabled = Wlan::IsEnabled();
  const bool connected = DataManager::GetIntValue("tw_wlan_connected") == 1;
  DataManager::SetValue("wlan_state", !enabled ? "disabled" :
                        connected ? "connected" : result ? "enabled" : "error");
  return result ? 0 : 1;
#endif
}
bool RecoveryWifiAutoEnable() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("of_wlan_auto_enable") == 1;
}
bool RecoveryWifiAutoConnect() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("of_wlan_auto_connect") == 1;
}
bool RecoverySetWifiAutoEnable(bool enabled) {
  LoadAeraPreferencesIfAvailable();
  if (DataManager::SetValue("of_wlan_auto_enable", enabled ? 1 : 0) != 0)
    return false;
  return SaveAeraPreferences();
}
bool RecoverySetWifiAutoConnect(bool enabled) {
  LoadAeraPreferencesIfAvailable();
  if (DataManager::SetValue("of_wlan_auto_connect", enabled ? 1 : 0) != 0)
    return false;
  return SaveAeraPreferences();
}

NasStatus RecoveryNasStatus() {
  NasStatus status;
#ifdef OF_ENABLE_WLAN
  status.supported = true;
  NasManager::RefreshStatus();
  // Stock NAS variables are session-scoped. Restore AERA's last profile only
  // when this fresh recovery process has no host configured yet.
  if (DataManager::GetStrValue(TW_NAS_HOST).empty()) {
    NasConfig remembered;
    if (LoadNasProfile(&remembered)) {
      DataManager::SetValue(TW_NAS_TYPE, remembered.type);
      DataManager::SetValue(TW_NAS_HOST, remembered.host);
      DataManager::SetValue(TW_NAS_PORT, remembered.port);
      DataManager::SetValue(TW_NAS_SHARE, remembered.share);
      DataManager::SetValue(TW_NAS_PATH, remembered.path);
      DataManager::SetValue(TW_NAS_USER, remembered.user);
      DataManager::SetValue(TW_NAS_DOMAIN, remembered.domain);
      DataManager::SetValue(TW_NAS_CACHE_MODE, remembered.cache_mode);
    }
  }
  status.mounted = NasManager::IsMounted();
  status.selected = status.mounted &&
      DataManager::GetStrValue("tw_storage_path") == TW_NAS_MOUNT_POINT;
  status.status = DataManager::GetStrValue(TW_NAS_STATUS_TEXT);
  status.error = NasManager::GetLastError();
  status.config.type = DataManager::GetStrValue(TW_NAS_TYPE);
  status.config.host = DataManager::GetStrValue(TW_NAS_HOST);
  status.config.port = DataManager::GetStrValue(TW_NAS_PORT);
  status.config.share = DataManager::GetStrValue(TW_NAS_SHARE);
  status.config.path = DataManager::GetStrValue(TW_NAS_PATH);
  status.config.user = DataManager::GetStrValue(TW_NAS_USER);
  status.config.password = DataManager::GetStrValue(TW_NAS_PASS);
  if (status.config.password.empty()) {
    std::string saved_password;
      status.config.password = saved_password;
      // NasManager consumes the session value; the persistent copy remains in
      // the encrypted secret store rather than the plain settings map.
      DataManager::SetValue(TW_NAS_PASS, saved_password);
    }
  }
  status.config.domain = DataManager::GetStrValue(TW_NAS_DOMAIN);
  status.config.cache_mode = DataManager::GetStrValue(TW_NAS_CACHE_MODE);
  if (status.config.type != "smb") status.config.type = "sftp";
  if (status.config.port.empty()) status.config.port = "22";
  if (status.config.share.empty()) status.config.share = "Backups";
  if (status.config.domain.empty()) status.config.domain = "WORKGROUP";
  if (status.config.cache_mode != "data") status.config.cache_mode = "off";
#endif
  return status;
}

bool RecoverySetNasConfig(const NasConfig &config, std::string *error) {
#ifndef OF_ENABLE_WLAN
  if (error) *error = "This recovery was built without network storage support.";
  return false;
#else
  const auto invalid = [](const std::string &value, size_t maximum) {
    return value.size() > maximum || value.find('\n') != std::string::npos ||
           value.find('\r') != std::string::npos;
  };
  if (config.type != "sftp" && config.type != "smb") {
    if (error) *error = "Choose either SFTP or SMB.";
    return false;
  }
  if (invalid(config.host, 128) || invalid(config.user, 128) ||
      invalid(config.password, 254) || invalid(config.share, 128) ||
      invalid(config.path, 256) || invalid(config.domain, 128)) {
    if (error) *error = "A field is too long or contains a line break.";
    return false;
  }
  if (config.port.empty() || config.port.size() > 5 ||
      !std::all_of(config.port.begin(), config.port.end(),
                   [](unsigned char character) {
                     return std::isdigit(character) != 0;
                   })) {
    if (error) *error = "Port must be a number from 1 to 65535.";
    return false;
  }
  const long port = strtol(config.port.c_str(), nullptr, 10);
  if (port < 1 || port > 65535) {
    if (error) *error = "Port must be a number from 1 to 65535.";
    return false;
  }
  if (config.cache_mode != "off" && config.cache_mode != "data") {
    if (error) *error = "Choose either no cache or /data write cache.";
    return false;
  }
  bool saved = true;
  saved &= DataManager::SetValue(TW_NAS_TYPE, config.type) == 0;
  saved &= DataManager::SetValue(TW_NAS_HOST, config.host) == 0;
  saved &= DataManager::SetValue(TW_NAS_PORT, config.port) == 0;
  saved &= DataManager::SetValue(TW_NAS_SHARE, config.share) == 0;
  saved &= DataManager::SetValue(TW_NAS_PATH, config.path) == 0;
  saved &= DataManager::SetValue(TW_NAS_USER, config.user) == 0;
  saved &= DataManager::SetValue(TW_NAS_DOMAIN, config.domain) == 0;
  saved &= DataManager::SetValue(TW_NAS_CACHE_MODE, config.cache_mode) == 0;
  // Keep the live value for NasManager, but never mark it persistent here.
  saved &= DataManager::SetValue(TW_NAS_PASS, config.password) == 0;
  const bool secret_saved = config.password.empty()
  if (!secret_saved) {
    if (error) *error = "Could not save the password to AERA's encrypted credential store.";
    return false;
  }
  if (!saved || !SaveNasProfile(config)) {
    if (error) *error = "Could not write the connection settings to recovery storage.";
    return false;
  }
  return true;
#endif
}

int RecoveryRunNas(const NasRequest &request) {
#ifndef OF_ENABLE_WLAN
  (void)request;
  return 1;
#else
  bool result = false;
  switch (request.operation) {
    case NasOperation::kMountAndUse:
      result = NasManager::Mount() && NasManager::SelectAsStorage();
      break;
    case NasOperation::kUse:
      result = NasManager::SelectAsStorage();
      break;
    case NasOperation::kUnmount:
      result = NasManager::Unmount();
      break;
  }
  NasManager::RefreshStatus();
  return result ? 0 : 1;
#endif
}
}  // namespace recovery_ui2
