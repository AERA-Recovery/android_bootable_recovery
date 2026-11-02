// SPDX-License-Identifier: Apache-2.0
#include "unlock_touch_gate.hpp"
#include <cassert>
#include <lvgl.h>
using aeraui::UnlockTouchGate;
static UnlockTouchGate gate;
static bool down=false;
static int clicks=0;
static void Read(lv_indev_t*,lv_indev_data_t* data) {
  data->point={180,250};
  data->state=down&&!gate.Blocked()?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static void Touch(lv_indev_t* input,bool pressed) {
  if(gate.Filter(0,pressed)) {
    down=false;
    if(!gate.Blocked())lv_indev_read(input);
    return;
  }
  down=pressed;lv_tick_inc(20);lv_indev_read(input);
}
int main() {
  lv_init();auto* display=lv_display_create(480,800);
  auto* input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(input,display);lv_indev_set_read_cb(input,Read);
  auto* button=lv_button_create(lv_screen_active());lv_obj_set_pos(button,100,180);lv_obj_set_size(button,200,160);
  lv_obj_add_event_cb(button,[](lv_event_t*){++clicks;},LV_EVENT_CLICKED,nullptr);
  auto* overlay=lv_obj_create(lv_layer_top());lv_obj_set_pos(overlay,0,0);lv_obj_set_size(overlay,480,800);
  lv_obj_update_layout(lv_screen_active());lv_obj_update_layout(lv_layer_top());
  Touch(input,true);gate.Arm();down=false;lv_indev_reset(input,nullptr);lv_obj_delete(overlay);
  for(int i=0;i<5;++i)Touch(input,true);
  Touch(input,false);assert(clicks==0&&!gate.Blocked());
  Touch(input,true);Touch(input,false);assert(clicks==1);
  // Unlocking after lift must not eat the first fresh tap.
  gate.Arm();assert(!gate.Blocked());
  Touch(input,true);Touch(input,false);assert(clicks==2);
  // AOD contacts are observed without being delivered to the underlying page.
  gate.Observe(0,true);gate.Arm();Touch(input,true);Touch(input,false);assert(clicks==2);
  Touch(input,true);Touch(input,false);assert(clicks==3);
  gate.Observe(0,true);gate.Arm();gate.Observe(0,false);assert(!gate.Blocked());
  Touch(input,true);Touch(input,false);assert(clicks==4);
  // Two fingers keep input blocked until both are lifted, including the last up.
  gate.Observe(0,true);gate.Observe(1,true);gate.Arm();
  assert(gate.Filter(0,false)&&gate.Blocked());
  assert(gate.Filter(0,true)&&gate.Blocked());
  assert(gate.Filter(1,false)&&gate.Blocked());
  assert(gate.Filter(0,false)&&!gate.Blocked());
  assert(!gate.Filter(0,true));assert(!gate.Filter(0,false));
  lv_deinit();
}
