/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "aera_wlan_ip.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace aera::wifi {
struct DhcpLease {
    std::string address, gateway;
    int prefix = 0;
    std::vector<std::string> dns;
};

inline bool ParseDhcpLease(const std::string& text, DhcpLease& lease) {
    if (text.size() > 4096 || text.find('\0') != std::string::npos) return false;
    DhcpLease parsed;
    std::istringstream input(text);
    std::string mask, routers, servers, extra;
    if (!std::getline(input, parsed.address) || !std::getline(input, mask) ||
        !std::getline(input, routers) || !std::getline(input, servers) ||
        std::getline(input, extra)) return false;
    if (!ValidHost(parsed.address) || !ParseSubnet(mask, parsed.prefix)) return false;
    std::istringstream router_list(routers), dns_list(servers);
    std::string address;
    while (router_list >> address) {
        if (!ValidHost(address) || address == parsed.address) return false;
        if (parsed.gateway.empty()) parsed.gateway = address;
    }
    while (dns_list >> address) {
        if (!ValidHost(address) || parsed.dns.size() >= 16) return false;
        parsed.dns.push_back(address);
    }
    lease = std::move(parsed);
    return true;
}

inline bool ReadDhcpLease(const std::string& path, DhcpLease& lease) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st {};
    std::string contents;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= 4096;
    char buffer[512];
    while (valid) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { valid = false; break; }
        if (!count) break;
        contents.append(buffer, static_cast<size_t>(count));
        if (contents.size() > 4096) valid = false;
    }
    close(fd);
    return valid && ParseDhcpLease(contents, lease);
}

// A DHCP hook only records the lease. The host validates it before changing IP state.
inline constexpr const char* kDhcpHook =
    "#!/system/bin/sh\n"
    "case \"$1\" in\n"
    "bound|renew) umask 077; printf '%s\\n' \"$ip\" \"$subnet\" \"$router\" \"$dns\" > \"$0.lease\" ;;\n"
    "*) exit 0 ;;\n"
    "esac\n";

inline int RunDhcpProcess(const std::vector<std::string>& args,
                          const std::string& log_path, int timeout_ms) {
    if (args.empty() || timeout_ms <= 0) return -1;
    const int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log < 0) return -1;
    struct stat st {};
    if (fstat(log, &st) != 0 || !S_ISREG(st.st_mode) || ftruncate(log, 0) != 0) {
        close(log);
        return -1;
    }
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, log, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, log, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, log);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    const int result = posix_spawn(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(log);
    if (result != 0) return -1;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    while (true) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (waited < 0 && errno != EINTR) return -1;
        if (std::chrono::steady_clock::now() >= deadline) break;
        usleep(20000);
    }
    // Include the hook in timeout cleanup, not just the DHCP parent process.
    kill(-pid, SIGTERM);
    usleep(100000);
    kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return 124;
}
}
