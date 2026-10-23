/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_wlan_dhcp.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>

int main() {
    using namespace aera::wifi;
    DhcpLease lease;
    assert(ParseDhcpLease("192.168.1.127\n255.255.255.0\n192.168.1.90\n192.168.1.90 1.1.1.1\n", lease));
    assert(lease.address == "192.168.1.127" && lease.prefix == 24);
    assert(lease.gateway == "192.168.1.90" && lease.dns.size() == 2);
    assert(ParseDhcpLease("10.0.0.2\n24\n\n\n", lease));
    assert(lease.gateway.empty() && lease.dns.empty());
    for (const char* invalid : {
        "1.2.3.4;reboot\n24\n\n\n", "0.0.0.0\n24\n\n\n",
        "10.0.0.2\n255.0.255.0\n\n\n", "10.0.0.2\n24\n1.2.3.4;reboot\n\n",
        "10.0.0.2\n24\n\n8.8.8.8;reboot\n", "10.0.0.2\n24\n\n\nextra\n"})
        assert(!ParseDhcpLease(invalid, lease));
    char root[] = "/tmp/aera-dhcp-test-XXXXXX";
    assert(mkdtemp(root));
    const std::string log = std::string(root) + "/log";
    const std::string file = std::string(root) + "/lease";
    assert(RunDhcpProcess({"/bin/sh", "-c", "printf success"}, log, 2000) == 0);
    assert(RunDhcpProcess({"/bin/sh", "-c", "exit 7"}, log, 2000) == 7);
    const auto start = std::chrono::steady_clock::now();
    assert(RunDhcpProcess({"/bin/sh", "-c", "trap '' TERM; sleep 30 & wait"}, log, 150) == 124);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    assert(RunDhcpProcess({"/no/such/program"}, log, 100) == -1);
    { std::ofstream output(file); output << "192.168.1.2\n24\n192.168.1.1\n8.8.8.8\n"; }
    assert(ReadDhcpLease(file, lease));
    unlink(file.c_str());
    assert(symlink(log.c_str(), file.c_str()) == 0);
    assert(!ReadDhcpLease(file, lease));
    unlink(file.c_str()); unlink(log.c_str()); rmdir(root);
    puts("DHCP lease validation, process results and bounded process-group cleanup passed");
}
