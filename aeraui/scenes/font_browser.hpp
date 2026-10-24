/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "fonts/fontsource.hpp"
#include <lvgl.h>
#include <functional>

namespace aeraui {
void OpenFontBrowser(lv_obj_t* screen, std::function<void()> applied,
                     fontsource::Fetch fetch = fontsource::Download);
}
