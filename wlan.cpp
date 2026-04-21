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

static const char* DEFAULT_WLAN_IFACE  = "wlan0";
static const char* DEFAULT_CTRL_DIR    = "/tmp/recovery/sockets";
static const char* DEFAULT_SUPP_CONF   = "/vendor/etc/wifi/wpa_supplicant.conf";

static const char* BIN_WPA_CLI         = "/system/bin/wpa_cli";
static const char* BIN_IFCONFIG        = "/system/bin/ifconfig";
static const char* BIN_DHCPTOOL        = "/system/bin/dhcptool";

static const char* WLAN_SUPP_SERVICE   = "wpa_supplicant";
static const char* WLAN_SUPP_SVC_PROP  = "init.svc.wpa_supplicant";

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

    RunCommand("rm -rf /tmp/wlan");
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
    if (!IsEnabled() && !Enable())
        return false;

    const std::string iface = GetIface();
    const std::string ctrl = GetCtrlDir();
    const std::string wpacli = GetWpaCliBinary();

    if (wpacli.empty()) {
        gui_print("WLAN: wpa_cli binary not found\n");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string ssid = DataManager::GetStrValue("wlanselectedid");
    std::string pass = DataManager::GetStrValue("wlan_password");

    if (ssid.empty()) {
        gui_print("WLAN: no SSID selected\n");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string enc;
    if (!ReadFile(std::string(WLAN_LIST_DIR) + "/" + ssid, enc)) {
        gui_print("WLAN: missing metadata for selected SSID\n");
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
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string esc_ssid = EscapeDoubleQuotes(ssid);
    gui_print("Add SSID to new config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 ssid '\"" + esc_ssid + "\"'")) {
        gui_print("WLAN: failed setting SSID\n");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Add encryption to new config...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " set_network 0 key_mgmt " + key_mgmt)) {
        gui_print("WLAN: failed setting key_mgmt\n");
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
            DataManager::SetValue("tw_wlan_connected", 0);
            return false;
        }
    }

    gui_print("Enable new config for network...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " enable_network 0")) {
        gui_print("WLAN: enable_network failed\n");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Select new config for network...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " select_network 0")) {
        gui_print("WLAN: select_network failed\n");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Reconnect...\n");
    if (!RunCommand(wpacli + " -i " + iface + " -p " + ctrl + " reconnect")) {
        gui_print("WLAN: reconnect failed\n");
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
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        return false;
    }

    gui_print(" \n");
    gui_print("Checking connection...\n");
    gui_print(" \n");
    gui_print("Getting dhcp ip...\n");
    gui_print(" \n");

    if (!StartDhcp()) {
        gui_print("WLAN: DHCP failed\n");
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
            std::istringstream iss(status);
            std::string line;

            while (std::getline(iss, line)) {
                if (line.rfind("ip_address=", 0) == 0)
                    ip_addr = line.substr(11);
                else if (line.rfind("ssid=", 0) == 0)
                    connected_ssid = line.substr(5);
            }

            if (!ip_addr.empty() && !connected_ssid.empty())
                break;
        }
    }

    UpdateConnectedName();
    RefreshSaved();

    if (ip_addr.empty()) {
        gui_print("WLAN: no IP address assigned\n");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Connected SSID: %s\n", connected_ssid.c_str());
    gui_print("IP address: %s\n", ip_addr.c_str());

    std::string connected;
    ReadFile(WLAN_CONNECTED_FILE, connected);

    if (Trim(connected).empty()) {
        gui_print("WLAN: connected name not updated\n");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    DataManager::SetValue("tw_wlan_connected", 1);
    gui_print("Wlan connect successfully!\n");
    return true;
}

bool Wlan::Info() {
    const std::string iface = GetIface();
    const std::string ifconfigbin = GetIfconfigBinary();

    if (ifconfigbin.empty()) {
        gui_print("WLAN: ifconfig binary not found\n");
        return false;
    }

    std::string info;
    if (!RunCommand(ifconfigbin + " " + iface, info)) {
        RunCommand(ifconfigbin + " -a " + iface, info);
    }

    if (info.empty()) {
        gui_print("Error: Cannot get wlan0 information\n");
        return false;
    }

    std::string mac = "Not found";
    std::string ipv4 = "Not found";
    std::string ipv6 = "Not found";

    {
        std::smatch m;
        std::regex mac_re("([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}");
        if (std::regex_search(info, m, mac_re))
            mac = m.str(0);
    }
    {
        std::smatch m;
        std::regex ipv4_re("inet addr:([0-9.]+)");
        if (std::regex_search(info, m, ipv4_re))
            ipv4 = m.str(1);
    }

    std::ostringstream out;
    out << "==========Network information========\n";
    out << " \n";
    out << "ipv4: " << ipv4 << "\n";
    out << " \n";
    out << "ipv6: " << ipv6 << "\n";
    out << " \n";
    out << "mac : " << mac << "\n";
    out << " \n";
    out << "=====================================\n";
    out << " \n";
    out << " \n";
    out << " \n";

    WriteFile(WLAN_INFO_FILE, out.str());
    gui_print("%s", out.str().c_str());
    return true;
}

bool Wlan::RefreshSaved() {
    return BuildSavedList();
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
    std::string conf;
    if (!ReadFile(GetSupplicantConf(), conf)) {
        unlink(WLAN_SAVED_FILE);
        return false;
    }

    std::istringstream iss(conf);
    std::string line;
    std::ostringstream out;

    while (std::getline(iss, line)) {
        std::string t = Trim(line);
        if (t.rfind("ssid=\"", 0) == 0) {
            size_t first = t.find('"');
            size_t last  = t.rfind('"');
            if (first != std::string::npos && last != std::string::npos && last > first)
                out << t.substr(first + 1, last - first - 1) << "\n";
        }
    }

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

    std::istringstream iss(status);
    std::string line;
    std::string ssid;
    std::string wpa_state;

    while (std::getline(iss, line)) {
        if (line.rfind("ssid=", 0) == 0)
            ssid = line.substr(5);
        else if (line.rfind("wpa_state=", 0) == 0)
            wpa_state = line.substr(10);
    }

    if (wpa_state == "COMPLETED" && !ssid.empty()) {
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
