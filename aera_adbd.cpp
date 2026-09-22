#include "aera_adbd.hpp"

#ifdef OF_ENABLE_WLAN

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <ctime>
#include <unistd.h>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#include <adb/pairing/pairing_server.h>
#include <openssl/rand.h>
#include <android-base/file.h>
#include <android-base/properties.h>
#include <cutils/properties.h>
#include <json/json.h>

#include "data.hpp"
#include "gui/gui.hpp"
#include "twcommon.h"
#include "twrp-functions.hpp"
#include "wlan.hpp"
#include "aera_secrets/aera_secrets.hpp"

namespace {

constexpr int kDefaultPort = 5555;
constexpr int kMinPairTimeout = 1;
constexpr int kMaxPairTimeout = 300;
constexpr const char* kTlsEnableProp = "persist.adb.tls_server.enable";
constexpr const char* kTlsPortProp = "service.adb.tls.port";
constexpr const char* kTlsRequestedPortProp = "service.adb.tls.port.requested";
constexpr const char* kTcpPortProp = "service.adb.tcp.port";

std::mutex g_pair_mutex;
PairingServerCtx* g_pairing_server = nullptr;
std::string g_pairing_code;
int g_pairing_port = 0;
// Bumped on every StartPairing()/StopPairingLocked(). The timeout and result
// callbacks capture the generation that was current when they were armed and
// only tear down if it still matches, so a stale timer from a previous session
// can never kill a newer one started within its window.
unsigned g_pairing_generation = 0;

bool ValidPort(int port)
{
	return port > 0 && port <= 65535;
}

int ClampPairTimeout(int timeout)
{
	if (timeout < kMinPairTimeout)
		return 60;
	if (timeout > kMaxPairTimeout)
		return kMaxPairTimeout;
	return timeout;
}

bool EnsureWlanConnected(std::string& ip)
{
	Wlan::Info();
	ip = DataManager::GetStrValue("wlan_info_ip");
	return DataManager::GetIntValue("tw_wlan_connected") == 1 && !ip.empty();
}

bool SetProp(const std::string& key, const std::string& value)
{
	return property_set(key.c_str(), value.c_str()) == 0;
}

std::string GetProp(const std::string& key)
{
	char value[PROPERTY_VALUE_MAX] = {};
	property_get(key.c_str(), value, "");
	return value;
}

bool WaitForPropNonEmpty(const std::string& key, std::string& value, int timeout_ms)
{
	const int step_ms = 100;
	int waited = 0;
	while (waited <= timeout_ms) {
		value = GetProp(key);
		if (!value.empty())
			return true;
		usleep(step_ms * 1000);
		waited += step_ms;
	}
	return false;
}

// Cryptographically-strong random digits, used for the ADB pairing code. Fails
// closed (returns "") if the CSPRNG is unavailable rather than falling back to a
// time-seeded, predictable value that would undermine pairing authentication.
// Uses rejection sampling so the digits are unbiased (no modulo bias).
std::string RandomDigits(size_t count)
{
	std::string out;
	out.reserve(count);
	while (out.size() < count) {
		unsigned char b = 0;
		if (RAND_bytes(&b, 1) != 1)
			return std::string();  // no entropy: refuse rather than emit a weak code
		if (b >= 250)
			continue;  // 250 == 25*10; drop the biased tail so b % 10 is uniform
		out.push_back(static_cast<char>('0' + (b % 10)));
	}
	return out;
}

std::string RandomHex(size_t bytes)
{
	static const char* hex = "0123456789abcdef";
	std::vector<unsigned char> buf(bytes);
	if (bytes && RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1)
		return std::string();  // no entropy: fail closed
	std::string out;
	out.reserve(bytes * 2);
	for (size_t i = 0; i < bytes; ++i) {
		out.push_back(hex[buf[i] >> 4]);
		out.push_back(hex[buf[i] & 0x0f]);
	}
	return out;
}

bool MkdirRecursive(const std::string& path, mode_t mode)
{
	if (path.empty())
		return false;
	if (path == "/")
		return true;
	std::string cur;
	size_t start = path[0] == '/' ? 1 : 0;
	if (start == 1)
		cur = "/";
	for (size_t i = start; i <= path.size(); ++i) {
		if (i != path.size() && path[i] != '/')
			continue;
		std::string part = path.substr(start, i - start);
		if (!part.empty()) {
			if (cur.size() > 1)
				cur += "/";
			cur += part;
			if (mkdir(cur.c_str(), mode) != 0 && errno != EEXIST)
				return false;
		}
		start = i + 1;
	}
	return true;
}

bool IsMountPoint(const std::string& mount_point)
{
	FILE* f = fopen("/proc/mounts", "r");
	if (!f)
		return false;
	char dev[256];
	char mnt[256];
	char rest[1024];
	bool found = false;
	while (fscanf(f, "%255s %255s %1023[^\n]\n", dev, mnt, rest) == 3) {
		if (mount_point == mnt) {
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}

bool AppendAdbKey(const std::string& key)
{
	if (key.empty())
		return false;

	std::vector<std::string> paths;
	if (IsMountPoint("/data"))
		paths.push_back("/data/misc/adb/adb_keys");
	paths.push_back("/adb_keys");

	for (const std::string& path : paths) {
		std::string dir(path);
		size_t slash = dir.find_last_of('/');
		if (slash != std::string::npos && slash > 0 && !MkdirRecursive(dir.substr(0, slash), 0700))
			continue;

		std::string existing;
		android::base::ReadFileToString(path.c_str(), &existing, false);
		// Match on the base64 token of a whole line, not a raw substring — a key
		// that merely appears inside another line must not count as present.
		const std::string token = key.substr(0, key.find(' '));
		bool present = false;
		std::istringstream lines(existing);
		std::string line;
		while (std::getline(lines, line)) {
			if (line.substr(0, line.find(' ')) == token) {
				present = true;
				break;
			}
		}
		if (present)
			return true;

		FILE* f = fopen(path.c_str(), "a");
		if (!f)
			continue;
		if (!existing.empty() && existing.back() != '\n')
			fputc('\n', f);
		fputs(key.c_str(), f);
		fputc('\n', f);
		fclose(f);
		chmod(path.c_str(), 0600);
		return true;
	}
	return false;
}

// Remove every adb_keys line whose base64 token matches 'key' (the stored
// pubkey, possibly with a trailing comment). Returns true if any file changed.
bool RemoveAdbKey(const std::string& key)
{
	if (key.empty())
		return false;
	std::string token = key.substr(0, key.find(' '));
	if (token.empty())
		return false;

	std::vector<std::string> paths;
	if (IsMountPoint("/data"))
		paths.push_back("/data/misc/adb/adb_keys");
	paths.push_back("/adb_keys");

	bool changed = false;
	for (const std::string& path : paths) {
		std::string existing;
		if (!android::base::ReadFileToString(path.c_str(), &existing, false))
			continue;
		std::istringstream in(existing);
		std::string line, kept;
		bool removed_here = false;
		while (std::getline(in, line)) {
			std::string line_token = line.substr(0, line.find(' '));
			if (line_token == token) {
				removed_here = true;
				continue;
			}
			if (!line.empty()) {
				kept += line;
				kept += '\n';
			}
		}
		if (!removed_here)
			continue;
		FILE* f = fopen(path.c_str(), "w");
		if (!f)
			continue;
		fputs(kept.c_str(), f);
		fclose(f);
		chmod(path.c_str(), 0600);
		changed = true;
	}
	return changed;
}

PeerInfo MakeDevicePeerInfo()
{
	PeerInfo info = {};
	info.type = ADB_DEVICE_GUID;
	std::string guid = "adb-recovery-" + RandomHex(4);
	snprintf(reinterpret_cast<char*>(info.data), sizeof(info.data), "%s", guid.c_str());
	return info;
}

void StopPairingLocked()
{
	PairingServerCtx* server = g_pairing_server;
	g_pairing_server = nullptr;
	g_pairing_port = 0;
	g_pairing_code.clear();
	// Invalidate any armed timeout/result-callback teardown for this session.
	g_pairing_generation++;
	if (server)
		pairing_server_destroy(server);
}

// Tear down only if the session that armed us is still the active one.
void StopPairingForGeneration(unsigned gen)
{
	std::lock_guard<std::mutex> lock(g_pair_mutex);
	if (gen == g_pairing_generation)
		StopPairingLocked();
}

void PairingResultCallback(const PeerInfo* peer_info, void*)
{
	if (peer_info && peer_info->type == ADB_RSA_PUB_KEY) {
		const char* key = reinterpret_cast<const char*>(peer_info->data);
		std::string key_str(key, strnlen(key, sizeof(peer_info->data)));
		if (AppendAdbKey(key_str)) {
			LOGINFO("ADB WiFi pairing: stored host key\n");
			// Record the paired host in the registry so `web adb list/forget`
			// can present and revoke it later.
			AeraSecrets::AddAdbDevice(key_str);
		} else {
			LOGERR("ADB WiFi pairing: failed to store host key\n");
		}
	}

	unsigned gen;
	{
		std::lock_guard<std::mutex> lock(g_pair_mutex);
		gen = g_pairing_generation;
	}
	std::thread([gen]() {
		usleep(100 * 1000);
		StopPairingForGeneration(gen);
	}).detach();
}

} // namespace

std::string AeraAdbd::WlanIp()
{
	Wlan::Info();
	return DataManager::GetStrValue("wlan_info_ip");
}

bool AeraAdbd::StartSecure(int port)
{
	if (!ValidPort(port))
		port = kDefaultPort;

	std::string ip;
	if (!EnsureWlanConnected(ip)) {
		gui_print("AERA: WLAN must be connected before starting paired ADB\n");
		return false;
	}

	SetProp(kTcpPortProp, "0");
	SetProp(kTlsRequestedPortProp, std::to_string(port));
	SetProp("ctl.start", "adbd");
	if (!SetProp(kTlsEnableProp, "1")) {
		gui_print("AERA: failed to enable paired ADB\n");
		return false;
	}

	std::string actual_port;
	WaitForPropNonEmpty(kTlsPortProp, actual_port, 5000);
	if (actual_port.empty())
		actual_port = std::to_string(port);

	gui_print("Paired ADB enabled\n");
	gui_print("connect=adb connect %s:%s\n", ip.c_str(), actual_port.c_str());
	return true;
}

bool AeraAdbd::StartNoAuth(int port)
{
	if (!ValidPort(port))
		port = kDefaultPort;

	std::string ip;
	if (!EnsureWlanConnected(ip)) {
		gui_print("AERA: WLAN must be connected before starting no-auth ADB\n");
		return false;
	}

	StopPairing();
	SetProp(kTlsEnableProp, "0");
	SetProp(kTlsPortProp, "");
	SetProp(kTlsRequestedPortProp, "");
	SetProp(kTcpPortProp, std::to_string(port));
	SetProp("ctl.restart", "adbd");
	SetProp("ctl.start", "adbd");

	gui_print("No-auth ADB enabled\n");
	gui_print("connect=adb connect %s:%d\n", ip.c_str(), port);
	return true;
}

bool AeraAdbd::StartPairing(int timeout_sec)
{
	std::string ip;
	if (!EnsureWlanConnected(ip)) {
		gui_print("AERA: WLAN must be connected before pairing ADB\n");
		return false;
	}

	if (!StartSecure(kDefaultPort))
		return false;

	std::lock_guard<std::mutex> lock(g_pair_mutex);
	StopPairingLocked();

	g_pairing_code = RandomDigits(6);
	if (g_pairing_code.empty()) {
		gui_print("AERA: no entropy available for ADB pairing code\n");
		return false;
	}
	PeerInfo info = MakeDevicePeerInfo();
	g_pairing_server = pairing_server_new_no_cert(
		reinterpret_cast<const uint8_t*>(g_pairing_code.data()),
		g_pairing_code.size(), &info, 0);
	if (!g_pairing_server) {
		g_pairing_code.clear();
		gui_print("AERA: failed to create ADB pairing server\n");
		return false;
	}

	g_pairing_port = pairing_server_start(g_pairing_server, PairingResultCallback, nullptr);
	if (g_pairing_port <= 0) {
		StopPairingLocked();
		gui_print("AERA: failed to start ADB pairing server\n");
		return false;
	}

	int timeout = ClampPairTimeout(timeout_sec);
	unsigned gen = g_pairing_generation;  // current session (g_pair_mutex held)
	std::thread([timeout, gen]() {
		sleep(timeout);
		StopPairingForGeneration(gen);
	}).detach();

	gui_print("ADB pairing active\n");
	gui_print("pair=adb pair %s:%d\n", ip.c_str(), g_pairing_port);
	gui_print("code=%s\n", g_pairing_code.c_str());
	gui_print("timeout=%d\n", timeout);
	return true;
}

bool AeraAdbd::StopPairing()
{
	std::lock_guard<std::mutex> lock(g_pair_mutex);
	StopPairingLocked();
	return true;
}

bool AeraAdbd::StopAll()
{
	StopPairing();
	SetProp(kTlsEnableProp, "0");
	SetProp(kTlsPortProp, "");
	SetProp(kTlsRequestedPortProp, "");
	SetProp(kTcpPortProp, "0");
	SetProp("ctl.restart", "adbd");
	return true;
}

void AeraAdbd::PrintStatus()
{
	Wlan::Info();
	std::string ip = DataManager::GetStrValue("wlan_info_ip");
	std::string tls_enabled = GetProp(kTlsEnableProp);
	std::string tls_port = GetProp(kTlsPortProp);
	std::string requested = GetProp(kTlsRequestedPortProp);
	std::string tcp_port = GetProp(kTcpPortProp);

	std::lock_guard<std::mutex> lock(g_pair_mutex);
	const bool pairing = g_pairing_server != nullptr;
	std::string mode = "off";
	if (!tcp_port.empty() && tcp_port != "0")
		mode = "no-auth";
	else if (pairing)
		mode = "pairing";
	else if (tls_enabled == "1")
		mode = "secure-paired";

	Json::Value s(Json::objectValue);
	s["mode"] = mode;
	s["wlan_connected"] = DataManager::GetIntValue("tw_wlan_connected") == 1;
	s["ip"] = ip;
	s["secure_enabled"] = tls_enabled == "1";
	s["connect_port"] = tls_port.empty() ? requested : tls_port;
	s["no_auth_port"] = tcp_port;
	s["pairing_active"] = pairing;
	s["pairing_port"] = g_pairing_port;
	s["pairing_code"] = g_pairing_code;
	if (!ip.empty()) {
		std::string port = tls_port.empty() ? requested : tls_port;
		if (port.empty())
			port = std::to_string(kDefaultPort);
		s["connect"] = "adb connect " + ip + ":" + port;
		if (pairing)
			s["pair"] = "adb pair " + ip + ":" + std::to_string(g_pairing_port);
	}
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "";
	gui_print("%s\n", Json::writeString(writer, s).c_str());
}

void AeraAdbd::PrintDevices()
{
	std::vector<AeraSecrets::AdbDevice> devices;
	AeraSecrets::ListAdbDevices(devices);
	Json::Value items(Json::arrayValue);
	for (const auto& dev : devices) {
		Json::Value item(Json::objectValue);
		item["fingerprint"] = dev.fingerprint;
		item["name"] = dev.name;
		item["last_seen"] = static_cast<Json::Int64>(dev.last_seen);
		items.append(item);
	}
	Json::Value payload(Json::objectValue);
	payload["items"] = items;
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "";
	gui_print("%s\n", Json::writeString(writer, payload).c_str());
}

bool AeraAdbd::ForgetDevice(const std::string& id)
{
	if (id.empty()) {
		gui_print("AERA: adb forget requires a device fingerprint or name\n");
		return false;
	}
	std::string removed_key;
	if (!AeraSecrets::DeleteAdbDevice(id, removed_key)) {
		gui_print("AERA: no authorized device matches '%s'\n", id.c_str());
		return false;
	}
	// Best-effort: also strip the key from the adb_keys files so the host can no
	// longer authenticate. The registry removal already succeeded.
	RemoveAdbKey(removed_key);
	gui_print("Forgot adb device '%s'\n", id.c_str());
	return true;
}

#endif // OF_ENABLE_WLAN
