/* SPDX-License-Identifier: Apache-2.0 */
#include "aeraui/backend.hpp"
#include <string_view>
#ifndef EXPECT_MOBILE_DATA
#define EXPECT_MOBILE_DATA 0
#endif
static_assert(aeraui::MobileDataEnabled() == (EXPECT_MOBILE_DATA == 1));
static_assert(std::string_view(aeraui::ConnectivityTitle()) ==
    (EXPECT_MOBILE_DATA == 1 ? "Network" : "Wi-Fi"));
int main() {}
