/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "ui_components.hpp"
namespace aeraui::widgets {
// Native, accent-aware artwork. No Microsoft/Winlator logo assets.
inline lv_obj_t *DesktopIcon(lv_obj_t *parent, lv_color_t accent, int size) {
  auto *root=lv_obj_create(parent);
  Clear(root);lv_obj_set_size(root,size,size);
  lv_obj_remove_flag(root,LV_OBJ_FLAG_CLICKABLE);
  auto shape=[&](int x,int y,int w,int h,int radius,bool outline) {
    auto *o=lv_obj_create(root);Clear(o);
    lv_obj_set_pos(o,x*size/100,y*size/100);
    lv_obj_set_size(o,w*size/100,h*size/100);
    lv_obj_set_style_radius(o,radius*size/100,0);
    lv_obj_set_style_bg_color(o,accent,0);
    lv_obj_set_style_bg_opa(o,outline?LV_OPA_TRANSP:LV_OPA_COVER,0);
    lv_obj_set_style_border_color(o,accent,0);
    lv_obj_set_style_border_width(o,outline?std::max(2,size/25):0,0);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);
    return o;
  };
  shape(9,16,82,57,9,true);
  shape(44,72,12,12,1,false);
  shape(29,84,42,5,2,false);
  shape(23,29,22,25,3,false);
  shape(51,29,25,8,2,false);
  shape(51,43,25,11,2,false);
  shape(23,60,53,3,1,false);
  return root;
}
}
