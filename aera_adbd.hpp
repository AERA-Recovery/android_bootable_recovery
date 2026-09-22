#pragma once

#ifdef OF_ENABLE_WLAN

#include <string>

class Fox_Adbd {
public:
	static bool StartSecure(int port);
	static bool StartNoAuth(int port);
	static bool StartPairing(int timeout_sec);
	static bool StopPairing();
	static bool StopAll();
	static void PrintStatus();
	static std::string WlanIp();

	// AERA_ADB_DEVICES_BEGIN/END block; ForgetDevice revokes by fingerprint or
	// name, removing the key from both the registry and the adb_keys files.
	static void PrintDevices();
	static bool ForgetDevice(const std::string& id);
};

#endif // OF_ENABLE_WLAN
