/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "../scenes/scene.hpp"
#include "ui_components.hpp"

namespace aeraui::widgets {
inline int NetworkTabsTop(lv_obj_t *screen) {
  return Landscape(screen) ? 310 : 430;
}
inline int NetworkContentTop(lv_obj_t *screen) {
  return MobileDataEnabled() ? NetworkTabsTop(screen) + 160
                             : Landscape(screen) ? 350 : 420;
}

inline lv_obj_t *NetworkModeIcon(lv_obj_t *parent, bool mobile, int size) {
  auto *icon = lv_obj_create(parent);
  design::Clear(icon);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  // Icon glyph advance/line height can exceed the nominal font size. Reserve
  // padding for the full glyph, and keep decorative icons out of UI font sizing.
  constexpr int padding = 12;
  lv_obj_set_size(icon, size + 2 * padding, size + 2 * padding);
  if (!mobile) {
    auto *symbol = design::FixedLabel(icon, LV_SYMBOL_WIFI, &lv_font_montserrat_48, design::kAccent);
    lv_obj_center(symbol);
  } else {
    for (int i = 0; i < 4; ++i) {
      auto *bar = lv_obj_create(icon);
      design::Clear(bar);
      lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
      const int height = size * (i + 1) / 4;
      lv_obj_set_pos(bar, padding + size * i / 4, padding + size - height);
      lv_obj_set_size(bar, size / 6, height);
      lv_obj_set_style_radius(bar, 3, 0);
      lv_obj_set_style_bg_color(bar, design::kAccent, 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
  }
  return icon;
}

inline void NetworkTabs(lv_obj_t *screen, bool mobile, ActionCallback callback, void *context) {
  if (!MobileDataEnabled()) return;
  auto *tabs = lv_obj_create(screen);
  design::Clear(tabs);
  const int width = Landscape(screen) ? 1450 : lv_obj_get_width(screen) - 128;
  lv_obj_set_pos(tabs, 64, NetworkTabsTop(screen));
  lv_obj_set_size(tabs, width, 128);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tabs, 24, 0);
  auto tab = [&](const char *title, Action action, bool selected, bool cellular) {
    auto *button = lv_button_create(tabs);
    design::Panel(button, 30, design::kMainPanel);
    design::Interactive(button, design::kMainSelected);
    lv_obj_set_size(button, (width - 24) / 2, 128);
    lv_obj_set_style_border_width(button, selected ? 3 : design::SurfaceStyleValue() == 2 ? 2 : 1, 0);
    lv_obj_set_style_border_color(button, selected ? design::kAccent : design::kMainLine, 0);
    lv_obj_set_style_border_opa(button, selected ? LV_OPA_COVER : LV_OPA_50, 0);
    auto *icon = NetworkModeIcon(button, cellular, 48);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 32, 0);
    auto *heading = Label(button, title, &lv_font_montserrat_32, design::kText);
    SingleLineLabel(heading, (width - 24) / 2 - 144, &lv_font_montserrat_32);
    lv_obj_align(heading, LV_ALIGN_LEFT_MID, 112, 0);
    OnClick(button, [=] { if (!selected && callback) callback(action, context); });
  };
  tab("Wi-Fi", Action::kWifi, !mobile, false);
  tab("Mobile data", Action::kMobileData, mobile, true);
}
}  // namespace aeraui::widgets
