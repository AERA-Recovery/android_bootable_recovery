/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "ui_components.hpp"
#include "phone_keyboard.hpp"
#include "network_tabs.hpp"
#include <array>

namespace aeraui {
namespace {
using namespace widgets;
using namespace design;
struct MobileUi {
  lv_obj_t *screen=nullptr,*title=nullptr,*detail=nullptr,*signal=nullptr,*network=nullptr;
  lv_obj_t *toggle=nullptr,*radio=nullptr,*apn_button=nullptr,*apn_text=nullptr;
  lv_obj_t *auto_radio=nullptr;
  std::array<lv_obj_t *,3> sim{};
  lv_timer_t *timer=nullptr;
  std::string apn,error;
  int subscription=0;
};
void RefreshMobile(MobileUi *state) {
  const auto mobile=RecoveryMobileStatus();
  const bool busy=mobile.enabled&&!mobile.connected;
  i18n::BindLabel(state->title,mobile.connected?"Connected":busy?"Connecting...":"Mobile data");
  lv_obj_set_style_text_color(state->title,mobile.connected?kGreen:kText,0);
  const std::string detail=!state->error.empty()?state->error:mobile.message;
  lv_label_set_text(state->detail,detail.c_str());
  if(mobile.enabled)lv_obj_add_state(state->radio,LV_STATE_CHECKED);
  else lv_obj_remove_state(state->radio,LV_STATE_CHECKED);
  if(RecoveryMobileAutoConnect())lv_obj_add_state(state->auto_radio,LV_STATE_CHECKED);
  else lv_obj_remove_state(state->auto_radio,LV_STATE_CHECKED);
  if(!mobile.available&&!mobile.enabled)lv_obj_add_state(state->toggle,LV_STATE_DISABLED);
  else lv_obj_remove_state(state->toggle,LV_STATE_DISABLED);
  std::string network=mobile.operator_name;
  if(!mobile.technology.empty())network+=(network.empty()?"":" / ")+mobile.technology;
  const auto public_ip=RecoveryMobilePublicIp(mobile);
  if(mobile.connected) {
    const std::string address=!public_ip.address.empty()?public_ip.address:
        public_ip.pending?"Checking...":"Unavailable";
    network+=(network.empty()?"Public IPv4  ":"\nPublic IPv4  ")+address;
  }
  if(network.empty())network="Not connected";
  lv_label_set_text(state->network,network.c_str());
  const std::string strength=mobile.signal_valid?std::to_string(mobile.signal_dbm)+" dBm":"Signal unavailable";
  lv_label_set_text(state->signal,strength.c_str());
  lv_label_set_text(state->apn_text,state->apn.empty()?"Automatic":state->apn.c_str());
  for(size_t i=0;i<state->sim.size();++i) {
    lv_obj_set_style_bg_color(state->sim[i],state->subscription==static_cast<int>(i)?kAccentSoft:kMainPanel,0);
    if(mobile.enabled)lv_obj_add_state(state->sim[i],LV_STATE_DISABLED);
    else lv_obj_remove_state(state->sim[i],LV_STATE_DISABLED);
  }
  if(mobile.enabled)lv_obj_add_state(state->apn_button,LV_STATE_DISABLED);
  else lv_obj_remove_state(state->apn_button,LV_STATE_DISABLED);
}
void EditApn(MobileUi *state) {
  auto *overlay=lv_obj_create(state->screen);Clear(overlay);
  lv_obj_set_user_data(overlay,&kModalMarker);
  lv_obj_set_size(overlay,LV_PCT(100),LV_PCT(100));
  lv_obj_set_style_bg_color(overlay,lv_color_black(),0);lv_obj_set_style_bg_opa(overlay,LV_OPA_60,0);
  lv_obj_update_layout(overlay);
  const int width=std::min(1312,lv_obj_get_width(overlay)-96);
  const int height=std::min(1260,lv_obj_get_height(overlay)-96);
  auto *sheet=lv_obj_create(overlay);Panel(sheet,32,kMainSheet);
  lv_obj_set_size(sheet,width,height);lv_obj_align(sheet,LV_ALIGN_BOTTOM_MID,0,-32);
  lv_obj_set_style_pad_all(sheet,32,0);
  auto *title=Label(sheet,"Access point name",&lv_font_montserrat_40,kText);
  lv_obj_set_pos(title,0,0);
  auto *input=TextArea(sheet);lv_obj_set_pos(input,0,90);lv_obj_set_size(input,width-64,120);
  lv_textarea_set_one_line(input,true);lv_textarea_set_max_length(input,100);
  lv_textarea_set_placeholder_text(input,"Automatic");lv_textarea_set_text(input,state->apn.c_str());
  lv_textarea_set_accepted_chars(input,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_");
  lv_obj_set_style_text_font(input,UiFont(&lv_font_montserrat_32),0);
  lv_obj_set_style_text_color(input,kText,0);lv_obj_set_style_bg_color(input,kMainPanel,0);
  auto *keyboard=lv_keyboard_create(sheet);phone_keyboard::Apply(keyboard);
  lv_obj_set_align(keyboard,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(keyboard,0,240);
  lv_obj_set_size(keyboard,width-64,height-450);lv_keyboard_set_textarea(keyboard,input);
  auto *dismiss=Button(sheet,"Cancel",[overlay]{lv_obj_delete_async(overlay);});
  lv_obj_set_size(dismiss,(width-88)/2,104);lv_obj_align(dismiss,LV_ALIGN_BOTTOM_LEFT,0,0);
  auto *apply=Button(sheet,"Apply",[state,input,overlay]{
    state->error.clear();const std::string apn=lv_textarea_get_text(input);
    if(RecoverySetMobileConfiguration(apn,state->subscription,&state->error))state->apn=apn;
    RefreshMobile(state);lv_obj_delete_async(overlay);},true);
  lv_obj_set_size(apply,(width-88)/2,104);lv_obj_align(apply,LV_ALIGN_BOTTOM_RIGHT,0,0);
}
}
void BuildMobileDataScene(lv_obj_t *screen,ActionCallback callback,void *context) {
  auto *state=new MobileUi;state->screen=screen;
  state->apn=RecoveryMobileApn();state->subscription=RecoveryMobileSubscription();
  Header(screen,ConnectivityTitle(),"Networks & connectivity",callback,context);
  NetworkTabs(screen,true,callback,context);
  auto *content=Scroll(screen,420,std::max(400,lv_obj_get_height(screen)-420-NavigationHeight(screen)-40));
  lv_obj_set_x(content,64);lv_obj_set_width(content,lv_obj_get_width(screen)-128);
  lv_obj_set_flex_flow(content,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(content,28,0);
  auto label=[&](const char *text,const lv_font_t *font,lv_color_t color){
    auto *object=Label(content,text,font,color);lv_obj_set_width(object,LV_PCT(100));
    lv_label_set_long_mode(object,LV_LABEL_LONG_WRAP);return object;};
  state->title=label("Mobile data",&lv_font_montserrat_48,kText);
  state->detail=label("Reading modem status",&lv_font_montserrat_28,kMuted);
  state->toggle=Button(content,"Mobile data",[state]{
    state->error.clear();const auto status=RecoveryMobileStatus();
    if(status.enabled)RecoveryDisconnectMobile(&state->error);
    else RecoveryConnectMobile(state->apn,state->subscription,&state->error);
    RefreshMobile(state);
  });
  lv_obj_set_size(state->toggle,LV_PCT(100),116);
  lv_obj_align(lv_obj_get_child(state->toggle,0),LV_ALIGN_LEFT_MID,28,0);
  state->radio=lv_switch_create(state->toggle);StyleSwitch(state->radio,false);
  lv_obj_align(state->radio,LV_ALIGN_RIGHT_MID,-28,0);
  auto *automatic=Button(content,"Auto-connect on boot",[state]{
    state->error.clear();RecoverySetMobileAutoConnect(!RecoveryMobileAutoConnect(),&state->error);
    RefreshMobile(state);
  });
  lv_obj_set_size(automatic,LV_PCT(100),116);
  lv_obj_align(lv_obj_get_child(automatic,0),LV_ALIGN_LEFT_MID,28,0);
  state->auto_radio=lv_switch_create(automatic);StyleSwitch(state->auto_radio,false);
  lv_obj_align(state->auto_radio,LV_ALIGN_RIGHT_MID,-28,0);
  label("DATA SIM",&lv_font_montserrat_24,kMuted);
  auto *choices=lv_obj_create(content);Clear(choices);lv_obj_set_size(choices,LV_PCT(100),110);
  lv_obj_set_flex_flow(choices,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(choices,16,0);
  const std::array<const char *,3> names{"Default","SIM 1","SIM 2"};
  for(int i=0;i<3;++i) {
    state->sim[i]=Button(choices,names[i],[state,i]{
      state->error.clear();
      if(RecoverySetMobileConfiguration(state->apn,i,&state->error))state->subscription=i;
      RefreshMobile(state);
    });
    lv_obj_set_flex_grow(state->sim[i],1);lv_obj_set_width(state->sim[i],0);lv_obj_set_height(state->sim[i],104);
  }
  label("ACCESS POINT NAME",&lv_font_montserrat_24,kMuted);
  state->apn_button=Button(content,"",[state]{EditApn(state);});
  lv_obj_set_size(state->apn_button,LV_PCT(100),116);
  state->apn_text=lv_obj_get_child(state->apn_button,0);
  state->network=label("Not connected",&lv_font_montserrat_32,kText);
  state->signal=label("Signal unavailable",&lv_font_montserrat_28,kMuted);
  label("Data only. SIM PIN must be disabled. Mobile data disconnects before flashing or restoring.",
        &lv_font_montserrat_24,kMuted);
  Navigation(screen,Action::kSettings,callback,context);
  RecoveryStartMobileService();RefreshMobile(state);
  state->timer=lv_timer_create([](lv_timer_t *timer){RefreshMobile(static_cast<MobileUi *>(lv_timer_get_user_data(timer)));},500,state);
  lv_obj_add_event_cb(screen,[](lv_event_t *event){auto *state=static_cast<MobileUi *>(lv_event_get_user_data(event));
    lv_timer_delete(state->timer);delete state;},LV_EVENT_DELETE,state);
}
}
