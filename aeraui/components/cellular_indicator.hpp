/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "design.hpp"
#include "aeraui/backend.hpp"

namespace aeraui::widgets {
constexpr int kCellularWidth=112;
struct CellularIndicator {lv_obj_t *root=nullptr,*rat=nullptr,*bars[4]{};};
inline CellularIndicator CreateCellularIndicator(lv_obj_t *parent) {
  using namespace design;CellularIndicator icon;
  icon.root=lv_obj_create(parent);Clear(icon.root);lv_obj_set_size(icon.root,kCellularWidth,52);
  lv_obj_add_flag(icon.root,LV_OBJ_FLAG_CLICKABLE);
  icon.rat=Label(icon.root,"",&lv_font_montserrat_20,kMutedStrong);
  lv_obj_set_size(icon.rat,48,32);lv_obj_align(icon.rat,LV_ALIGN_LEFT_MID,0,0);
  lv_obj_set_style_text_align(icon.rat,LV_TEXT_ALIGN_RIGHT,0);
  for(int i=0;i<4;++i) {
    icon.bars[i]=lv_obj_create(icon.root);Clear(icon.bars[i]);
    lv_obj_remove_flag(icon.bars[i],LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(icon.bars[i],8,12+i*10);lv_obj_align(icon.bars[i],LV_ALIGN_BOTTOM_LEFT,58+i*13,-4);
    lv_obj_set_style_radius(icon.bars[i],2,0);lv_obj_set_style_bg_color(icon.bars[i],kMutedStrong,0);
  }
  lv_obj_remove_flag(icon.rat,LV_OBJ_FLAG_CLICKABLE);return icon;
}
inline void UpdateCellularIndicator(const CellularIndicator &icon,const MobileStatus &status) {
  using namespace design;
  lv_label_set_text(icon.rat,status.registered?status.technology.c_str():"--");
  for(int i=0;i<4;++i) {
    lv_obj_set_style_bg_color(icon.bars[i],kMutedStrong,0);
    lv_obj_set_style_bg_opa(icon.bars[i],status.signal_valid&&status.bars>i?LV_OPA_COVER:LV_OPA_30,0);
  }
}
}
