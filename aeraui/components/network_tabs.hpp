/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "../scenes/scene.hpp"
#include "ui_components.hpp"

namespace aeraui::widgets {
inline void NetworkTabs(lv_obj_t *screen, bool mobile, ActionCallback callback, void *context) {
  if (!MobileDataEnabled()) return;
  auto *tabs = lv_obj_create(screen);
  design::Clear(tabs);
  lv_obj_set_pos(tabs, lv_obj_get_width(screen) - 832, Landscape(screen) ? 184 : 232);
  lv_obj_set_size(tabs, 640, 104);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tabs, 16, 0);
  auto tab = [&](const char *title, Action action, bool selected) {
    auto *button = Button(tabs, title, [=] { if (!selected) callback(action, context); });
    lv_obj_set_size(button, 312, 104);
    lv_obj_set_style_bg_color(button, selected ? design::kAccentSoft : design::kMainPanel, 0);
    lv_obj_set_style_text_color(button, selected ? design::kAccent : design::kText, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), selected ? design::kAccent : design::kText, 0);
  };
  tab("Wi-Fi", Action::kWifi, !mobile);
  tab("Mobile data", Action::kMobileData, mobile);
}
}  // namespace aeraui::widgets
