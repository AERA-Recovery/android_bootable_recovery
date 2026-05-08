#include "wlan.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <regex>
#include <cutils/properties.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <iomanip>
#include <map>

#include "data.hpp"
#include "gui/gui.hpp"
#include "twcommon.h"
#include "twrp-functions.hpp"

#ifndef LOGINFO
#define LOGINFO(...) printf(__VA_ARGS__)
#endif

#ifndef LOGERR
#define LOGERR(...) fprintf(stderr, __VA_ARGS__)
#endif

static const char* WLAN_TMP_DIR        = "/tmp/wlan";
static const char* WLAN_LIST_DIR       = "/tmp/wlan/list";
static const char* WLAN_LIST_FILE      = "/tmp/wlan/list.txt";
static const char* WLAN_SAVED_FILE     = "/tmp/wlan/saved.txt";
static const char* WLAN_CONNECTED_FILE = "/tmp/wlan/connected_name.txt";
static const char* WLAN_INFO_FILE      = "/tmp/wlan/info.txt";

static const char* WLAN_SAVED_SECURE_FILE = "/data/media/0/Fox/wlan/wlan_saved.enc";
static const char* WLAN_SAVED_SECURE_DIR  = "/data/media/0/Fox/wlan";

static const char* DEFAULT_WLAN_IFACE  = "wlan0";
static const char* DEFAULT_CTRL_DIR    = "/tmp/recovery/sockets";
static const char* DEFAULT_SUPP_CONF   = "/vendor/etc/wifi/wpa_supplicant.conf";

static const char* BIN_WPA_CLI         = "/system/bin/wpa_cli";
static const char* BIN_IFCONFIG        = "/system/bin/ifconfig";
static const char* BIN_DHCPTOOL        = "/system/bin/dhcptool";

static const char* WLAN_SUPP_SERVICE   = "wpa_supplicant";
static const char* WLAN_SUPP_SVC_PROP  = "init.svc.wpa_supplicant";


static std::string OF_HexEncode(const unsigned char* data, size_t len)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = data[i];
        out.push_back(hex[(c >> 4) & 0x0F]);
        out.push_back(hex[c & 0x0F]);
    }

    return out;
}

static std::string OF_HexEncodeString(const std::string& in)
{
    return OF_HexEncode(reinterpret_cast<const unsigned char*>(in.data()), in.size());
}

static bool OF_HexDecode(const std::string& hex, std::vector<unsigned char>& out)
{
    if (hex.size() % 2 != 0)
        return false;

    out.clear();
    out.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2) {
        char h = hex[i];
        char l = hex[i + 1];

        int hi = -1;
        int lo = -1;

        if (h >= '0' && h <= '9') hi = h - '0';
        else if (h >= 'a' && h <= 'f') hi = h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') hi = h - 'A' + 10;

        if (l >= '0' && l <= '9') lo = l - '0';
        else if (l >= 'a' && l <= 'f') lo = l - 'a' + 10;
        else if (l >= 'A' && l <= 'F') lo = l - 'A' + 10;

        if (hi < 0 || lo < 0)
            return false;

        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }

    return true;
}

static bool OF_HexDecodeString(const std::string& hex, std::string& out)
{
    std::vector<unsigned char> decoded;

    if (!OF_HexDecode(hex, decoded))
        return false;

    out.assign(reinterpret_cast<const char*>(decoded.data()), decoded.size());
    return true;
}

static bool OF_GetWlanCryptoKey(unsigned char key[32])
{
    /*
     * Practical recovery-side encryption key.
     *
     * This protects against casual viewing of wlan_saved.enc.
     * It is not hardware-backed security, because recovery can still contain
     * the derivation logic.
     */
    char serial[PROPERTY_VALUE_MAX] = {0};
    char device[PROPERTY_VALUE_MAX] = {0};
    char product[PROPERTY_VALUE_MAX] = {0};

    property_get("ro.serialno", serial, "");
    property_get("ro.product.device", device, "");
    property_get("ro.product.name", product, "");

    std::string material;
    material += "OrangeFox-WLAN-Saved-v1|";
    material += serial;
    material += "|";
    material += device;
    material += "|";
    material += product;
    material += "|INFINITI-WLAN";

    SHA256(reinterpret_cast<const unsigned char*>(material.data()), material.size(), key);
    return true;
}

static bool OF_EncryptPassword(const std::string& plain, std::string& iv_hex, std::string& cipher_hex, std::string& tag_hex)
{
    unsigned char key[32];
    OF_GetWlanCryptoKey(key);

    unsigned char iv[12];
    if (RAND_bytes(iv, sizeof(iv)) != 1)
        return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;

    bool ok = false;
    std::vector<unsigned char> cipher;
    cipher.resize(plain.size() + 16);

    int len = 0;
    int cipher_len = 0;
    unsigned char tag[16];

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
        goto done;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), NULL) != 1)
        goto done;

    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1)
        goto done;

    if (!plain.empty()) {
        if (EVP_EncryptUpdate(
                ctx,
                cipher.data(),
                &len,
                reinterpret_cast<const unsigned char*>(plain.data()),
                plain.size()) != 1) {
            goto done;
        }

        cipher_len = len;
    }

    if (EVP_EncryptFinal_ex(ctx, cipher.data() + cipher_len, &len) != 1)
        goto done;

    cipher_len += len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) != 1)
        goto done;

    cipher.resize(cipher_len);

    iv_hex = OF_HexEncode(iv, sizeof(iv));
    cipher_hex = OF_HexEncode(cipher.data(), cipher.size());
    tag_hex = OF_HexEncode(tag, sizeof(tag));

    ok = true;

done:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static bool OF_DecryptPassword(const std::string& iv_hex, const std::string& cipher_hex, const std::string& tag_hex, std::string& plain)
{
    unsigned char key[32];
    OF_GetWlanCryptoKey(key);

    std::vector<unsigned char> iv;
    std::vector<unsigned char> cipher;
    std::vector<unsigned char> tag;

    if (!OF_HexDecode(iv_hex, iv))
        return false;

    if (!OF_HexDecode(cipher_hex, cipher))
        return false;

    if (!OF_HexDecode(tag_hex, tag))
        return false;

    if (iv.size() != 12 || tag.size() != 16)
        return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;

    bool ok = false;
    std::vector<unsigned char> out;
    out.resize(cipher.size() + 16);

    int len = 0;
    int plain_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
        goto done;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), NULL) != 1)
        goto done;

    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv.data()) != 1)
        goto done;

    if (!cipher.empty()) {
        if (EVP_DecryptUpdate(ctx, out.data(), &len, cipher.data(), cipher.size()) != 1)
            goto done;

        plain_len = len;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) != 1)
        goto done;

    if (EVP_DecryptFinal_ex(ctx, out.data() + plain_len, &len) != 1)
        goto done;

    plain_len += len;
    out.resize(plain_len);

    plain.assign(reinterpret_cast<const char*>(out.data()), out.size());
    ok = true;

done:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static std::vector<std::string> OF_SplitString(const std::string& line, char delim)
{
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string part;

    while (std::getline(ss, part, delim))
        out.push_back(part);

    return out;
}

static std::string OF_TrimSavedString(const std::string& s)
{
    size_t start = 0;
    while (start < s.size() &&
           (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) {
        start++;
    }

    size_t end = s.size();
    while (end > start &&
           (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r')) {
        end--;
    }

    return s.substr(start, end - start);
}

static bool OF_ReadSecureSavedLines(std::vector<std::string>& lines)
{
    lines.clear();

    std::ifstream ifs(WLAN_SAVED_SECURE_FILE);
    if (!ifs.is_open())
        return false;

    std::string line;
    while (std::getline(ifs, line)) {
        line = OF_TrimSavedString(line);

        if (!line.empty())
            lines.push_back(line);
    }

    return true;
}

static bool OF_WriteSecureSavedLines(const std::vector<std::string>& lines)
{
    TWFunc::Exec_Cmd(std::string("mkdir -p ") + WLAN_SAVED_SECURE_DIR);

    std::string tmp = std::string(WLAN_SAVED_SECURE_FILE) + ".tmp";

    std::ofstream ofs(tmp.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return false;

    for (size_t i = 0; i < lines.size(); ++i)
        ofs << lines[i] << "\n";

    ofs.close();

    chmod(tmp.c_str(), 0600);

    if (rename(tmp.c_str(), WLAN_SAVED_SECURE_FILE) != 0) {
        unlink(tmp.c_str());
        return false;
    }

    chmod(WLAN_SAVED_SECURE_FILE, 0600);
    return true;
}

static bool OF_SaveEncryptedNetwork(const std::string& ssid, const std::string& password, const std::string& encryption)
{
    if (ssid.empty())
        return false;

    std::string iv_hex;
    std::string cipher_hex;
    std::string tag_hex;

    if (!OF_EncryptPassword(password, iv_hex, cipher_hex, tag_hex))
        return false;

    std::string ssid_hex = OF_HexEncodeString(ssid);
    std::string enc_hex = OF_HexEncodeString(encryption);

    /*
     * Format:
     * ssid_hex|encryption_hex|iv_hex|cipher_hex|tag_hex
     */
    std::string new_line = ssid_hex + "|" + enc_hex + "|" + iv_hex + "|" + cipher_hex + "|" + tag_hex;

    std::vector<std::string> lines;
    OF_ReadSecureSavedLines(lines);

    std::vector<std::string> out;
    bool replaced = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::vector<std::string> parts = OF_SplitString(lines[i], '|');

        if (parts.size() >= 5 && parts[0] == ssid_hex) {
            if (!replaced) {
                out.push_back(new_line);
                replaced = true;
            }
        } else {
            out.push_back(lines[i]);
        }
    }

    if (!replaced)
        out.push_back(new_line);

    return OF_WriteSecureSavedLines(out);
}

static bool OF_LoadEncryptedNetwork(const std::string& ssid, std::string& password, std::string& encryption)
{
    password.clear();
    encryption.clear();

    if (ssid.empty())
        return false;

    std::string ssid_hex = OF_HexEncodeString(ssid);

    std::vector<std::string> lines;
    if (!OF_ReadSecureSavedLines(lines))
        return false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::vector<std::string> parts = OF_SplitString(lines[i], '|');

        if (parts.size() < 5)
            continue;

        if (parts[0] != ssid_hex)
            continue;

        if (!OF_HexDecodeString(parts[1], encryption))
            encryption = "WPA2";

        if (!OF_DecryptPassword(parts[2], parts[3], parts[4], password))
            return false;

        return true;
    }

    return false;
}

static bool OF_DeleteEncryptedNetwork(const std::string& ssid)
{
    if (ssid.empty())
        return false;

    std::string ssid_hex = OF_HexEncodeString(ssid);

    std::vector<std::string> lines;
    if (!OF_ReadSecureSavedLines(lines))
        return true;

    std::vector<std::string> out;
    bool removed = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::vector<std::string> parts = OF_SplitString(lines[i], '|');

        if (parts.size() >= 1 && parts[0] == ssid_hex) {
            removed = true;
            continue;
        }

        out.push_back(lines[i]);
    }

    if (!removed)
        return true;

    if (out.empty()) {
        unlink(WLAN_SAVED_SECURE_FILE);
        return true;
    }

    return OF_WriteSecureSavedLines(out);
}

static bool OF_GetEncryptedSavedSsids(std::vector<std::string>& ssids)
{
    ssids.clear();

    std::vector<std::string> lines;
    if (!OF_ReadSecureSavedLines(lines))
        return false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::vector<std::string> parts = OF_SplitString(lines[i], '|');

        if (parts.size() < 5)
            continue;

        std::string ssid;
        if (!OF_HexDecodeString(parts[0], ssid))
            continue;

        if (ssid.empty())
            continue;

        bool duplicate = false;
        for (size_t j = 0; j < ssids.size(); ++j) {
            if (ssids[j] == ssid) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
            ssids.push_back(ssid);
    }

    return !ssids.empty();
}

bool Wlan::Init() {
    DataManager::SetValue("tw_wlan_enabled", 0);
    DataManager::SetValue("tw_wlan_connected", 0);
    DataManager::SetValue("wlan_connected_name", "");
    return EnsureTmpLayout();
}

bool Wlan::Enable() {
    EnsureTmpLayout();

    if (!StartSupplicant()) {
        DataManager::SetValue("tw_wlan_enabled", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        DataManager::SetValue("wlan_connected_name", "");
        gui_print("WLAN: failed to start supplicant service\n");
        return false;
    }

    DataManager::SetValue("tw_wlan_enabled", 1);
    UpdateConnectedName();
    return true;
}

bool Wlan::StopSupplicant() {
    return StopInitSupplicantService();
}

bool Wlan::Disable() {
	StopDhcp();
	StopSupplicant();

	/*
	 * Do not delete /tmp/wlan/saved.txt.
	 * It is our UI-side saved WLAN list for the current recovery session.
	 */
	RunCommand("rm -rf /tmp/wlan/list");
	RunCommand("rm -f /tmp/wlan/list.txt");
	RunCommand("rm -f /tmp/wlan/list_saved.txt");
	RunCommand("rm -f /tmp/wlan/list_unsaved.txt");
	RunCommand("rm -f /tmp/wlan/connected_name.txt");
	RunCommand("rm -f /tmp/wlan/info.txt");

	EnsureTmpLayout();

	DataManager::SetValue("tw_wlan_enabled", 0);
	DataManager::SetValue("tw_wlan_connected", 0);
	DataManager::SetValue("wlan_connected_name", "");
	return true;
}

bool Wlan::IsEnabled() {
    return DataManager::GetIntValue("tw_wlan_enabled") == 1;
}

bool Wlan::Scan() {
    if (!IsEnabled() && !Enable())
        return false;

    const std::string iface = GetIface();
    const std::string ctrl = GetCtrlDir();
    const std::string wpacli = GetWpaCliBinary();

    if (wpacli.empty()) {
        gui_print("WLAN: wpa_cli binary not found\n");
        return false;
    }

    RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " scan");

    for (int i = 0; i < 5; ++i) {
        usleep(1000 * 1000);
        if (BuildScanList()) {
            UpdateConnectedName();
            RefreshSaved();
            return true;
        }
    }

    gui_print("WLAN: scan failed\n");
    return false;
}

bool Wlan::Connect() {
    DataManager::SetValue("wlan_connect_text", "Connecting");
    DataManager::SetValue("wlan_connect_done", 0);

    if (!IsEnabled() && !Enable()) {
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    const std::string iface = GetIface();
    const std::string ctrl = GetCtrlDir();
    const std::string wpacli = GetWpaCliBinary();

    if (wpacli.empty()) {
        gui_print("WLAN: wpa_cli binary not found\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string ssid = DataManager::GetStrValue("wlanselectedid");
    std::string pass = DataManager::GetStrValue("wlan_password");

    if (ssid.empty()) {
        gui_print("WLAN: no SSID selected\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string enc;
    if (!ReadFile(std::string(WLAN_LIST_DIR) + "/" + ssid, enc)) {
        gui_print("WLAN: missing metadata for selected SSID\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }
    enc = Trim(enc);

    std::string key_mgmt = "NONE";
    if (enc == "WPA3") {
        key_mgmt = "SAE";
    } else if (enc == "WPA2" || enc == "WPA") {
        key_mgmt = "WPA-PSK";
    } else if (enc == "OPEN") {
        key_mgmt = "NONE";
    } else {
        key_mgmt = "WPA-PSK";
    }

    if (key_mgmt != "NONE" && pass.empty()) {
        gui_print("WLAN: password required\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Beginning WLAN connection...\n");
    gui_print("Connecting to wlan: %s\n", ssid.c_str());
    gui_print("Encryption: %s (key_mgmt: %s)\n", enc.c_str(), key_mgmt.c_str());
    gui_print(" \n");

    gui_print("Remove old network config...\n");
    std::string list_out;
    if (RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " list_networks", list_out)) {
        bool has_net0 = false;
        std::istringstream lss(list_out);
        std::string lline;
        bool first_line = true;

        while (std::getline(lss, lline)) {
            if (first_line) {
                first_line = false;
                continue;
            }

            lline = Trim(lline);
            if (lline.empty())
                continue;

            if (lline.rfind("0\t", 0) == 0 || lline.rfind("0 ", 0) == 0 || lline == "0") {
                has_net0 = true;
                break;
            }
        }

        if (has_net0) {
            RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " remove_network 0");
        }
    }

    gui_print("Add new network config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " add_network")) {
        gui_print("WLAN: add_network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string esc_ssid = EscapeDoubleQuotes(ssid);

    gui_print("Add SSID to new config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 ssid '\"" + esc_ssid + "\"'")) {
        gui_print("WLAN: failed setting SSID\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Add encryption to new config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 key_mgmt " + key_mgmt)) {
        gui_print("WLAN: failed setting key_mgmt\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    if (key_mgmt != "NONE") {
        std::string esc_pass = EscapeDoubleQuotes(pass);
        gui_print("Add password to new config...\n");

        bool pass_ok = false;
        if (key_mgmt == "SAE") {
            pass_ok = RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 sae_password '\"" + esc_pass + "\"'");
        } else {
            pass_ok = RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 psk '\"" + esc_pass + "\"'");
        }

        if (!pass_ok) {
            gui_print("WLAN: failed setting password\n");
            DataManager::SetValue("wlan_connect_text", "Failed");
            DataManager::SetValue("wlan_connect_done", 0);
            DataManager::SetValue("tw_wlan_connected", 0);
            return false;
        }
    }

    gui_print("Enable new config for network...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " enable_network 0")) {
        gui_print("WLAN: enable_network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Select new config for network...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " select_network 0")) {
        gui_print("WLAN: select_network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Reconnect...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " reconnect")) {
        gui_print("WLAN: reconnect failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print(" \n");
    gui_print("Connect to network with new config...\n");
    gui_print(" \n");

    int tries = 0;
    const int max_tries = 10;
    bool completed = false;

    while (tries < max_tries) {
        usleep(1000 * 1000);

        std::string status;
        if (RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " status", status)) {
            std::istringstream iss(status);
            std::string line;
            std::string wpa_state;
            std::string current_ssid;

            while (std::getline(iss, line)) {
                if (line.rfind("wpa_state=", 0) == 0)
                    wpa_state = line.substr(10);
                else if (line.rfind("ssid=", 0) == 0)
                    current_ssid = line.substr(5);
            }

            gui_print("Connection state: %s (%d/%d)\n", wpa_state.c_str(), tries, max_tries);

            if (wpa_state == "COMPLETED" && current_ssid == ssid) {
                completed = true;
                break;
            }
        }

        tries++;
    }

    if (!completed) {
        gui_print(" \n");
        gui_print("WLAN: association failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        RefreshSaved();
        return false;
    }

    DataManager::SetValue("wlan_connect_text", "Getting IP-Address");
    DataManager::SetValue("wlan_connect_done", 0);

    gui_print(" \n");
    gui_print("Checking connection...\n");
    gui_print(" \n");
    gui_print("Getting dhcp ip...\n");
    gui_print(" \n");

    if (!StartDhcp()) {
        gui_print("WLAN: DHCP failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        RefreshSaved();
        return false;
    }

    std::string ip_addr;
    std::string connected_ssid;

    for (int i = 0; i < 5; ++i) {
        usleep(1000 * 1000);

        std::string status;
        if (RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " status", status)) {
            std::string wpa_state;
            ParseSupplicantStatus(status, wpa_state, connected_ssid, ip_addr);

            if (!ip_addr.empty() && !connected_ssid.empty())
                break;
        }
    }

    UpdateConnectedName();

    if (ip_addr.empty()) {
        gui_print("WLAN: no IP address assigned\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        RefreshSaved();
        return false;
    }

    gui_print("Connected SSID: %s\n", connected_ssid.c_str());
    gui_print("IP address: %s\n", ip_addr.c_str());

    std::string connected;
    ReadFile(WLAN_CONNECTED_FILE, connected);

    if (Trim(connected).empty()) {
        gui_print("WLAN: connected name not updated\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        RefreshSaved();
        return false;
    }

    /*
     * UI-side saved WLAN handling.
     * This does not depend on wpa_supplicant.conf.
     * If the connection worked, add the SSID to /tmp/wlan/saved.txt.
     */
    EnsureTmpLayout();

    std::string saved;
    ReadFile(WLAN_SAVED_FILE, saved);

    bool already_saved = false;
    std::istringstream iss_saved(saved);
    std::string saved_line;

    while (std::getline(iss_saved, saved_line)) {
        if (Trim(saved_line) == ssid) {
            already_saved = true;
            break;
        }
    }

    if (!already_saved) {
        if (!saved.empty() && saved[saved.size() - 1] != '\n')
            saved += "\n";

        saved += ssid + "\n";

        if (WriteFile(WLAN_SAVED_FILE, saved)) {
            gui_print("WLAN: saved network: %s\n", ssid.c_str());
        } else {
            gui_print("WLAN: failed to write saved network file\n");
        }
    } else {
        gui_print("WLAN: network already saved: %s\n", ssid.c_str());
    }

    if (OF_SaveEncryptedNetwork(ssid, pass, enc)) {
        gui_print("WLAN: encrypted credentials saved for: %s\n", ssid.c_str());
    } else {
        gui_print("WLAN: failed to save encrypted credentials for: %s\n", ssid.c_str());
    }

    RefreshSaved();

    std::string saved_debug;
    if (ReadFile(WLAN_SAVED_FILE, saved_debug)) {
        gui_print("WLAN: saved.txt after refresh:\n%s", saved_debug.c_str());
    } else {
        gui_print("WLAN: saved.txt missing after refresh\n");
    }

    DataManager::SetValue("tw_wlan_connected", 1);
    DataManager::SetValue("wlan_connected_name", ssid);

    /*
     * Show the check icon and "Connected" text in the overlay before it closes.
     */
    DataManager::SetValue("wlan_connect_text", "Connected");
    DataManager::SetValue("wlan_connect_done", 1);
    usleep(1000 * 1000);

    gui_print("Wlan connect successfully!\n");
    return true;
}

bool Wlan::ConnectSaved() {
    DataManager::SetValue("wlan_connect_text", "Connecting");
    DataManager::SetValue("wlan_connect_done", 0);

    if (!IsEnabled() && !Enable()) {
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    const std::string iface = GetIface();
    const std::string ctrl = GetCtrlDir();
    const std::string wpacli = GetWpaCliBinary();

    if (wpacli.empty()) {
        gui_print("WLAN: wpa_cli binary not found\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string ssid = DataManager::GetStrValue("wlanselectedid");

    if (ssid.empty()) {
        gui_print("WLAN: no saved SSID selected\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string saved_pass;
    std::string saved_enc;

    if (!OF_LoadEncryptedNetwork(ssid, saved_pass, saved_enc)) {
        gui_print("WLAN: no encrypted saved credentials for: %s\n", ssid.c_str());
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Beginning saved WLAN connection...\n");
    gui_print("Connecting to saved wlan: %s\n", ssid.c_str());
    gui_print("Saved encryption: %s\n", saved_enc.c_str());
    gui_print(" \n");

    std::string key_mgmt = "NONE";

    if (saved_enc == "WPA3") {
        key_mgmt = "SAE";
    } else if (saved_enc == "WPA2" || saved_enc == "WPA") {
        key_mgmt = "WPA-PSK";
    } else if (saved_enc == "OPEN") {
        key_mgmt = "NONE";
    } else {
        key_mgmt = "WPA-PSK";
    }

    gui_print("Disconnect current WLAN state...\n");
    RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " disconnect");
    usleep(500 * 1000);

    gui_print("Remove old network config...\n");
    std::string list_out;

    if (RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " list_networks", list_out)) {
        std::istringstream lss(list_out);
        std::string lline;
        bool first_line = true;

        while (std::getline(lss, lline)) {
            if (first_line) {
                first_line = false;
                continue;
            }

            lline = Trim(lline);
            if (lline.empty())
                continue;

            std::vector<std::string> cols;
            std::stringstream ls(lline);
            std::string col;

            while (std::getline(ls, col, '\t')) {
                cols.push_back(col);
            }

            if (cols.size() >= 1) {
                std::string id = Trim(cols[0]);
                if (!id.empty()) {
                    RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " remove_network " + id);
                }
            }
        }
    }

    gui_print("Add saved network config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " add_network")) {
        gui_print("WLAN: add_network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string esc_ssid = EscapeDoubleQuotes(ssid);

    gui_print("Add saved SSID to config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 ssid '\"" + esc_ssid + "\"'")) {
        gui_print("WLAN: failed setting saved SSID\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Add saved encryption to config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 key_mgmt " + key_mgmt)) {
        gui_print("WLAN: failed setting saved key_mgmt\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    if (key_mgmt != "NONE") {
        std::string esc_pass = EscapeDoubleQuotes(saved_pass);

        gui_print("Add saved password to config...\n");

        bool pass_ok = false;
        if (key_mgmt == "SAE") {
            pass_ok = RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 sae_password '\"" + esc_pass + "\"'");
        } else {
            pass_ok = RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 psk '\"" + esc_pass + "\"'");
        }

        if (!pass_ok) {
            gui_print("WLAN: failed setting saved password\n");
            DataManager::SetValue("wlan_connect_text", "Failed");
            DataManager::SetValue("wlan_connect_done", 0);
            DataManager::SetValue("tw_wlan_connected", 0);
            return false;
        }
    }

    gui_print("Enable saved config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " enable_network 0")) {
        gui_print("WLAN: enable saved network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Select saved config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " select_network 0")) {
        gui_print("WLAN: select saved network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Reconnect saved network...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " reconnect")) {
        gui_print("WLAN: saved reconnect failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    int tries = 0;
    const int max_tries = 10;
    bool completed = false;
    bool saw_transition = false;

    while (tries < max_tries) {
        usleep(1000 * 1000);

        std::string status;
        if (RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " status", status)) {
            std::string wpa_state;
            std::string current_ssid;
            std::string ip_addr;
            ParseSupplicantStatus(status, wpa_state, current_ssid, ip_addr);

            gui_print("Saved connection state: %s (%d/%d)\n", wpa_state.c_str(), tries, max_tries);

            if (wpa_state != "COMPLETED")
                saw_transition = true;

            if (wpa_state == "COMPLETED" && current_ssid == ssid) {
                completed = true;
                break;
            }
        }

        tries++;
    }

    if (!completed) {
        gui_print("WLAN: saved association failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        return false;
    }

    if (!saw_transition) {
        gui_print("WLAN: saved association was already completed; forcing reassociation before DHCP\n");
        RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " reassociate");

        completed = false;

        for (int i = 0; i < max_tries; ++i) {
            usleep(1000 * 1000);

            std::string status;
            if (RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " status", status)) {
                std::string wpa_state;
                std::string current_ssid;
                std::string ip_addr;
                ParseSupplicantStatus(status, wpa_state, current_ssid, ip_addr);

                gui_print("Saved reassociation state: %s (%d/%d)\n", wpa_state.c_str(), i, max_tries);

                if (wpa_state == "COMPLETED" && current_ssid == ssid) {
                    completed = true;
                    break;
                }
            }
        }

        if (!completed) {
            gui_print("WLAN: saved reassociation failed\n");
            DataManager::SetValue("wlan_connect_text", "Failed");
            DataManager::SetValue("wlan_connect_done", 0);
            DataManager::SetValue("tw_wlan_connected", 0);
            UpdateConnectedName();
            return false;
        }
    }

    DataManager::SetValue("wlan_connect_text", "Getting IP-Address");
    DataManager::SetValue("wlan_connect_done", 0);

    gui_print("Getting dhcp ip for saved network...\n");

    if (!StartDhcp()) {
        gui_print("WLAN: DHCP failed on saved network\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        return false;
    }

    std::string ip_addr;
    std::string connected_ssid;

    for (int i = 0; i < 5; ++i) {
        usleep(1000 * 1000);

        std::string status;
        if (RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " status", status)) {
            std::string wpa_state;
            ParseSupplicantStatus(status, wpa_state, connected_ssid, ip_addr);

            if (!ip_addr.empty() && !connected_ssid.empty())
                break;
        }
    }

    UpdateConnectedName();
    RefreshSaved();

    if (ip_addr.empty()) {
        gui_print("WLAN: no IP address assigned on saved network\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    DataManager::SetValue("tw_wlan_connected", 1);
    DataManager::SetValue("wlan_connected_name", ssid);

    /*
     * Show the check icon and "Connected" text in the overlay before it closes.
     */
    DataManager::SetValue("wlan_connect_text", "Connected");
    DataManager::SetValue("wlan_connect_done", 1);
    usleep(1000 * 1000);

    gui_print("Connected saved SSID: %s\n", connected_ssid.c_str());
    gui_print("IP address: %s\n", ip_addr.c_str());
    gui_print("WLAN: saved network connected successfully!\n");

    return true;
}

bool Wlan::Info()
{
    const std::string iface = GetIface();
    const std::string ctrl = GetCtrlDir();
    const std::string wpacli = GetWpaCliBinary();

    /*
     * Do not clear cached UI state before checking status.
     * A temporary wpa_cli failure would otherwise remove the accent row
     * and connected info even if WLAN is still connected.
     */
    if (wpacli.empty()) {
        gui_print("WLAN: wpa_cli binary not found for info, keeping cached WLAN info\n");
        return true;
    }

    std::string status;
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " status", status)) {
        gui_print("WLAN: failed to read status for info, keeping cached WLAN info\n");
        return true;
    }

    std::string wpa_state;
    std::string ssid;
    std::string ip_addr;
    ParseSupplicantStatus(status, wpa_state, ssid, ip_addr);

    DataManager::SetValue("wlan_info_state", wpa_state);

    if (wpa_state == "COMPLETED" && !ssid.empty() && !ip_addr.empty()) {
        DataManager::SetValue("wlan_info_connected", "1");
        DataManager::SetValue("wlan_info_ssid", ssid);
        DataManager::SetValue("wlan_info_ip", ip_addr);

        DataManager::SetValue("tw_wlan_connected", 1);
        DataManager::SetValue("wlan_connected_name", ssid);
        DataManager::SetValue("wlan_info_text", "Connected: " + ssid + "  IP: " + ip_addr);

        return true;
    }

    /*
     * Only clear connected UI state after a valid status response that says
     * WLAN is not actually completed/connected.
     */
    DataManager::SetValue("wlan_info_connected", "0");
    DataManager::SetValue("wlan_info_ssid", "");
    DataManager::SetValue("wlan_info_ip", "");
    DataManager::SetValue("wlan_info_text", "");

    DataManager::SetValue("tw_wlan_connected", 0);
    DataManager::SetValue("wlan_connected_name", "");

    return true;
}

bool Wlan::RefreshSaved() {
    return BuildSavedList();
}

bool Wlan::ForgetSaved()
{
    std::string ssid = Trim(DataManager::GetStrValue("wlanselectedid"));

    if (ssid.empty()) {
        gui_print("WLAN: no saved network selected for delete\n");
        return false;
    }

    gui_print("WLAN: removing saved network: %s\n", ssid.c_str());

    /*
     * Remove from the plain runtime saved list.
     */
    std::string saved;
    std::ostringstream out;

    if (ReadFile(WLAN_SAVED_FILE, saved)) {
        std::istringstream iss(saved);
        std::string line;

        while (std::getline(iss, line)) {
            std::string saved_ssid = Trim(line);

            if (saved_ssid.empty())
                continue;

            if (saved_ssid == ssid)
                continue;

            out << saved_ssid << "\n";
        }

        std::string new_saved = out.str();

        if (new_saved.empty()) {
            unlink(WLAN_SAVED_FILE);
        } else {
            WriteFile(WLAN_SAVED_FILE, new_saved);
        }
    }

    /*
     * Remove encrypted credentials too.
     */
    if (OF_DeleteEncryptedNetwork(ssid)) {
        gui_print("WLAN: encrypted credentials removed for: %s\n", ssid.c_str());
    } else {
        gui_print("WLAN: failed removing encrypted credentials for: %s\n", ssid.c_str());
    }

    DataManager::SetValue("wlanselectedid", "");

    RefreshSaved();

    return true;
}

bool Wlan::UpdateConnectedName() {
    return BuildConnectedName();
}

bool Wlan::EnsureTmpLayout() {
    if (!MkdirRecursive(WLAN_TMP_DIR)) return false;
    if (!MkdirRecursive(WLAN_LIST_DIR)) return false;
    return true;
}

bool Wlan::EnsureSupplicantConf() {
    // Path for vendor config
    const std::string conf = "/vendor/etc/wifi/wpa_supplicant.conf";

    if (FileExists(conf)) {
        return true;
    }

    // If the file doesn't exist, try to create a basic one if needed
    std::ostringstream ss;
    ss << "ctrl_interface=/data/misc/wifi/sockets\n";
    ss << "update_config=1\n";
    ss << "country=US\n";

    return WriteFile(conf, ss.str());
}

bool Wlan::StartSupplicant() {
    EnsureSupplicantConf();
    MkdirRecursive(GetCtrlDir());
    return StartInitSupplicantService();
}

bool Wlan::StartDhcp() {
    const std::string iface = GetIface();
    const std::string dhcptool = "/system/bin/dhcptool";

    if (!FileExists(dhcptool)) {
        gui_print("WLAN: dhcptool binary not found\n");
        return false;
    }

    return RunCommand(dhcptool + " " + iface + " >/tmp/dhcptool.log 2>&1");
}

bool Wlan::StopDhcp() {
    return true;
}

bool Wlan::BuildScanList() {
    const std::string iface = GetIface();
    const std::string ctrl = GetCtrlDir();
    const std::string wpacli = GetWpaCliBinary();

    if (wpacli.empty()) {
        gui_print("WLAN: wpa_cli binary not found\n");
        return false;
    }

    RunCommand("mkdir -p /tmp/wlan/list");
    RunCommand("rm -rf /tmp/wlan/list/*");

    std::string output;
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " scan_results", output)) {
        gui_print("WLAN: failed to get scan_results\n");
        return false;
    }

    std::istringstream iss(output);
    std::string line;
    bool first_line = true;
    std::vector<std::pair<std::string, std::string> > networks;

    while (std::getline(iss, line)) {
        if (first_line) {
            first_line = false;
            continue;
        }

        if (Trim(line).empty())
            continue;

        std::vector<std::string> cols;
        std::stringstream ls(line);
        std::string col;
        while (std::getline(ls, col, '\t')) {
            cols.push_back(col);
        }

        if (cols.size() < 5)
            continue;

        std::string flags = cols[3];
        std::string ssid = Trim(cols[4]);

        if (ssid.empty())
            continue;

        std::string encryption = "OPEN";
        if (flags.find("SAE") != std::string::npos) {
            encryption = "WPA3";
        } else if (flags.find("WPA2") != std::string::npos || flags.find("RSN") != std::string::npos) {
            encryption = "WPA2";
        } else if (flags.find("WPA-PSK") != std::string::npos || flags.find("WPA") != std::string::npos) {
            encryption = "WPA";
        }

        std::string safe = ssid;
        for (size_t i = 0; i < safe.size(); ++i) {
            char& c = safe[i];
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }

        safe = Trim(safe);
        if (safe.empty())
            continue;

        bool exists = false;
        for (size_t i = 0; i < networks.size(); ++i) {
            if (networks[i].first == safe) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            networks.push_back(std::make_pair(safe, encryption));
        }
    }

    if (networks.empty()) {
        unlink(WLAN_LIST_FILE);
        return false;
    }

    std::ostringstream list;
    for (size_t i = 0; i < networks.size(); ++i) {
        list << networks[i].first << "\n";
        WriteFile(std::string(WLAN_LIST_DIR) + "/" + networks[i].first, networks[i].second);
    }

    return WriteFile(WLAN_LIST_FILE, list.str());
}

bool Wlan::BuildSavedList() {
    EnsureTmpLayout();

    std::vector<std::string> unique;

    /*
     * First read encrypted persistent saved networks.
     */
    std::vector<std::string> encrypted_ssids;
    if (OF_GetEncryptedSavedSsids(encrypted_ssids)) {
        for (size_t i = 0; i < encrypted_ssids.size(); ++i) {
            if (encrypted_ssids[i].empty())
                continue;

            bool duplicate = false;
            for (size_t j = 0; j < unique.size(); ++j) {
                if (unique[j] == encrypted_ssids[i]) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate)
                unique.push_back(encrypted_ssids[i]);
        }
    }

    /*
     * Also merge current-session /tmp/wlan/saved.txt.
     */
    std::string saved;
    if (ReadFile(WLAN_SAVED_FILE, saved)) {
        std::istringstream iss(saved);
        std::string line;

        while (std::getline(iss, line)) {
            std::string ssid = Trim(line);

            if (ssid.empty())
                continue;

            bool duplicate = false;
            for (size_t i = 0; i < unique.size(); ++i) {
                if (unique[i] == ssid) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate)
                unique.push_back(ssid);
        }
    }

    if (unique.empty()) {
        unlink(WLAN_SAVED_FILE);
        return false;
    }

    std::ostringstream out;
    for (size_t i = 0; i < unique.size(); ++i)
        out << unique[i] << "\n";

    return WriteFile(WLAN_SAVED_FILE, out.str());
}

bool Wlan::BuildConnectedName() {
    const std::string iface = GetIface();
    const std::string ctrl  = GetCtrlDir();
    const std::string wpacli = GetWpaCliBinary();
    std::string status;

    if (wpacli.empty()) {
        unlink(WLAN_CONNECTED_FILE);
        DataManager::SetValue("wlan_connected_name", "");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " status", status)) {
        unlink(WLAN_CONNECTED_FILE);
        DataManager::SetValue("wlan_connected_name", "");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string ssid;
    std::string wpa_state;
    std::string ip_addr;
    ParseSupplicantStatus(status, wpa_state, ssid, ip_addr);

    if (wpa_state == "COMPLETED" && !ssid.empty() && !ip_addr.empty()) {
        WriteFile(WLAN_CONNECTED_FILE, ssid + "\n");
        DataManager::SetValue("wlan_connected_name", ssid);
        DataManager::SetValue("tw_wlan_connected", 1);
        return true;
    }

    unlink(WLAN_CONNECTED_FILE);
    DataManager::SetValue("wlan_connected_name", "");
    DataManager::SetValue("tw_wlan_connected", 0);
    return false;
}

bool Wlan::ParseSupplicantStatus(const std::string& status, std::string& wpa_state, std::string& ssid, std::string& ip_addr) {
    wpa_state.clear();
    ssid.clear();
    ip_addr.clear();

    std::istringstream iss(status);
    std::string line;

    while (std::getline(iss, line)) {
        line = Trim(line);

        if (line.rfind("wpa_state=", 0) == 0)
            wpa_state = line.substr(10);
        else if (line.rfind("ssid=", 0) == 0)
            ssid = line.substr(5);
        else if (line.rfind("ip_address=", 0) == 0)
            ip_addr = line.substr(11);
    }

    return !wpa_state.empty();
}

bool Wlan::RunCommand(const std::string& cmd) {
    LOGINFO("WLAN CMD: %s\n", cmd.c_str());
    return TWFunc::Exec_Cmd(cmd) == 0;
}

bool Wlan::RunCommand(const std::string& cmd, std::string& output) {
    LOGINFO("WLAN CMD: %s\n", cmd.c_str());

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        LOGERR("popen failed for %s: %s\n", cmd.c_str(), strerror(errno));
        return false;
    }

    char buf[512];
    output.clear();

    while (fgets(buf, sizeof(buf), fp) != NULL)
        output += buf;

    int rc = pclose(fp);
    return rc == 0;
}

bool Wlan::WriteFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return false;
    ofs << content;
    return true;
}

bool Wlan::ReadFile(const std::string& path, std::string& out) {
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open())
        return false;

    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

bool Wlan::FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool Wlan::MkdirRecursive(const std::string& path) {
    if (path.empty())
        return false;
    if (FileExists(path))
        return true;

    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] == '/' && current != "/")
            mkdir(current.c_str(), 0755);
    }

    mkdir(path.c_str(), 0755);
    return FileExists(path);
}

std::string Wlan::Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start]))
        ++start;

    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1]))
        --end;

    return s.substr(start, end - start);
}

std::string Wlan::GetIface() {
    std::string iface = DataManager::GetStrValue("tw_wlan_iface");
    return iface.empty() ? DEFAULT_WLAN_IFACE : iface;
}

std::string Wlan::GetCtrlDir() {
    std::string ctrl = DataManager::GetStrValue("tw_wlan_ctrl_dir");
    return ctrl.empty() ? DEFAULT_CTRL_DIR : ctrl;
}

std::string Wlan::GetSupplicantConf() {
    std::string conf = DataManager::GetStrValue("tw_wlan_conf");
    return conf.empty() ? DEFAULT_SUPP_CONF : conf;
}

std::string Wlan::EscapeDoubleQuotes(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"')
            out += "\\\"";
        else
            out += s[i];
    }
    return out;
}

std::string Wlan::GetWpaCliBinary() {
    return FindBinary({
        "/system/bin/wpa_cli",
        "/vendor/bin/wpa_cli",
        "/vendor/bin/hw/wpa_cli",
        "/system_ext/bin/wpa_cli",
        "/sbin/wpa_cli"
    });
}

std::string Wlan::FindBinary(const std::vector<std::string>& paths) {
    for (size_t i = 0; i < paths.size(); ++i) {
        if (FileExists(paths[i])) {
            return paths[i];
        }
    }
    return "";
}

std::string Wlan::GetIfconfigBinary() {
    return FindBinary({
        "/system/bin/ifconfig",
        "/vendor/bin/ifconfig",
        "/system_ext/bin/ifconfig",
        "/sbin/ifconfig",
        "/bin/ifconfig"
    });
}

bool Wlan::WaitForProperty(const std::string& key, const std::string& expected, int timeout_ms) {
    const int step_ms = 100;
    int waited = 0;
    char value[PROPERTY_VALUE_MAX] = {0};

    while (waited < timeout_ms) {
        property_get(key.c_str(), value, "");
        if (expected == value)
            return true;
        usleep(step_ms * 1000);
        waited += step_ms;
    }
    return false;
}

bool Wlan::StartInitSupplicantService() {
    char value[PROPERTY_VALUE_MAX] = {0};
    property_get(WLAN_SUPP_SVC_PROP, value, "");

    std::string state = value;
    if (state == "running" || state == "restarting") {
        LOGINFO("WLAN: supplicant service already active (%s)\n", state.c_str());
        return true;
    }

    LOGINFO("WLAN: starting init service %s\n", WLAN_SUPP_SERVICE);
    property_set("ctl.start", WLAN_SUPP_SERVICE);

    if (!WaitForProperty(WLAN_SUPP_SVC_PROP, "running", 5000)) {
        LOGERR("WLAN: init service %s failed to enter running state\n", WLAN_SUPP_SERVICE);
        return false;
    }

    return true;
}

bool Wlan::StopInitSupplicantService() {
    char value[PROPERTY_VALUE_MAX] = {0};
    property_get(WLAN_SUPP_SVC_PROP, value, "");

    if (std::string(value) == "stopped") {
        LOGINFO("WLAN: supplicant service already stopped\n");
        return true;
    }

    LOGINFO("WLAN: stopping init service %s\n", WLAN_SUPP_SERVICE);
    property_set("ctl.stop", WLAN_SUPP_SERVICE);

    if (!WaitForProperty(WLAN_SUPP_SVC_PROP, "stopped", 5000)) {
        LOGERR("WLAN: init service %s failed to enter stopped state\n", WLAN_SUPP_SERVICE);
        return false;
    }

    return true;
}

bool Wlan::IsProcessRunning(const std::string& name) {
    return TWFunc::Exec_Cmd("pidof " + name + " >/dev/null 2>&1") == 0;
}
