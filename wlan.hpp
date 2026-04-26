#pragma once

#include <string>
#include <vector>
#include <regex>

class Wlan {
public:
    static bool Init();

    static bool Enable();
    static bool Disable();
    static bool IsEnabled();

    static bool Scan();
    static bool Connect();
    static bool ConnectSaved();
    static bool Info();
    static bool RefreshSaved();
    static bool UpdateConnectedName();

private:
    static bool EnsureTmpLayout();
    static bool EnsureSupplicantConf();

    static bool StartSupplicant();
    static bool StopSupplicant();
    static bool WaitForProperty(const std::string& key, const std::string& expected, int timeout_ms);
    static bool StartInitSupplicantService();
    static bool StopInitSupplicantService();
    static bool StartDhcp();
    static bool StopDhcp();

    static bool BuildScanList();
    static bool BuildSavedList();
    static bool BuildConnectedName();

    static bool RunCommand(const std::string& cmd);
    static bool RunCommand(const std::string& cmd, std::string& output);

    static bool WriteFile(const std::string& path, const std::string& content);
    static bool ReadFile(const std::string& path, std::string& out);
    static bool FileExists(const std::string& path);
    static bool MkdirRecursive(const std::string& path);
    static bool IsProcessRunning(const std::string& name);

    static std::string Trim(const std::string& s);
    static std::string GetIface();
    static std::string GetCtrlDir();
    static std::string GetSupplicantConf();
    static std::string EscapeDoubleQuotes(const std::string& s);
    static std::string GetWpaCliBinary();
    static std::string FindBinary(const std::vector<std::string>& paths);
    static std::string GetIfconfigBinary();
};
