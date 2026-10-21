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
  lv_obj_t *status_icon=nullptr,*address=nullptr;
  lv_obj_t *toggle=nullptr,*radio=nullptr,*apn_button=nullptr,*apn_text=nullptr;
  lv_obj_t *auto_radio=nullptr;
  lv_obj_t *pin_button=nullptr,*pin_status=nullptr,*saved_pins=nullptr;
  std::string saved_signature;
  std::array<lv_obj_t *,3> sim{};
  lv_timer_t *timer=nullptr;
  std::string apn,error;
  int subscription=0;
};
lv_obj_t *pin_overlay=nullptr;
std::string prompted_request,open_request;
lv_obj_t *MobileControl(lv_obj_t *parent,const char *text,Handler action) {
  auto *button=lv_button_create(parent);Clear(button);Panel(button,30,kMainPanel);
  Interactive(button,kMainSelected);
  Label(button,text,&lv_font_montserrat_32,kText);
  OnClick(button,std::move(action));return button;
}
void EditPin(lv_obj_t *screen);
void RefreshSavedPins(MobileUi *state) {
  const auto saved=RecoverySavedMobilePins();
  const auto mobile=RecoveryMobileStatus();
  std::string signature;
  for(const auto &pin:saved)signature+=pin.id+std::to_string(pin.slot);
  for(const auto &sim:mobile.sims)signature+=sim.id+std::to_string(sim.slot);
  if(signature==state->saved_signature)return;
  state->saved_signature=signature;lv_obj_clean(state->saved_pins);
  if(saved.empty())lv_obj_add_flag(state->saved_pins,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(state->saved_pins,LV_OBJ_FLAG_HIDDEN);
  for(const auto &pin:saved) {
    int slot=pin.slot;
    for(const auto &sim:mobile.sims)if(sim.id==pin.id)slot=sim.slot;
    const std::string text=i18n::Format("Forget PIN for SIM %d",slot);
    auto *button=Button(state->saved_pins,text.c_str(),[state,id=pin.id]{
      state->error.clear();RecoveryForgetMobilePin(id,&state->error);
    });
    lv_obj_set_size(button,LV_PCT(100),116);
    Panel(button,30,kMainPanel);
  }
}
void RefreshMobile(MobileUi *state) {
  const auto mobile=RecoveryMobileStatus();
  const bool busy=mobile.enabled&&!mobile.connected;
  i18n::BindLabel(state->title,mobile.state=="pin_required"?"SIM PIN required":
      mobile.state=="verifying_pin"?"Verifying SIM PIN":mobile.connected?"Connected":busy?"Connecting...":"Mobile data");
  lv_obj_set_style_text_color(state->title,mobile.connected?kGreen:kText,0);
  for(uint32_t i=0;i<lv_obj_get_child_count(state->status_icon);++i)
    lv_obj_set_style_bg_color(lv_obj_get_child(state->status_icon,i),mobile.connected?kGreen:kAccent,0);
  const std::string detail=!state->error.empty()?state->error:mobile.message;
  lv_label_set_text(state->detail,i18n::Translate(detail.c_str()));
  if(mobile.enabled)lv_obj_add_state(state->radio,LV_STATE_CHECKED);
  else lv_obj_remove_state(state->radio,LV_STATE_CHECKED);
  if(RecoveryMobileAutoConnect())lv_obj_add_state(state->auto_radio,LV_STATE_CHECKED);
  else lv_obj_remove_state(state->auto_radio,LV_STATE_CHECKED);
  if(!mobile.available&&!mobile.enabled)lv_obj_add_state(state->toggle,LV_STATE_DISABLED);
  else lv_obj_remove_state(state->toggle,LV_STATE_DISABLED);
  std::string network=mobile.operator_name;
  if(!mobile.technology.empty())network+=(network.empty()?"":" / ")+mobile.technology;
  const auto public_ip=RecoveryMobilePublicIp(mobile);
  const std::string address=!mobile.connected?"Not connected":!public_ip.address.empty()?public_ip.address:
      public_ip.pending?"Checking...":"Unavailable";
  lv_label_set_text(state->address,i18n::Translate(address.c_str()));
  if(network.empty())network="Not connected";
  lv_label_set_text(state->network,network.c_str());
  const std::string strength=mobile.signal_valid?std::to_string(mobile.signal_dbm)+" dBm":"Signal unavailable";
  lv_label_set_text(state->signal,strength.c_str());
  lv_label_set_text(state->apn_text,state->apn.empty()?"Automatic":state->apn.c_str());
  for(size_t i=0;i<state->sim.size();++i) {
    const bool active=state->subscription==static_cast<int>(i);
    lv_obj_set_style_border_width(state->sim[i],active?3:SurfaceStyleValue()==2?2:1,0);
    lv_obj_set_style_border_color(state->sim[i],active?kAccent:kMainLine,0);
    lv_obj_set_style_border_opa(state->sim[i],active?LV_OPA_COVER:LV_OPA_50,0);
    if(mobile.state=="stopping"||mobile.state=="verifying_pin")lv_obj_add_state(state->sim[i],LV_STATE_DISABLED);
    else lv_obj_remove_state(state->sim[i],LV_STATE_DISABLED);
  }
  if(mobile.enabled)lv_obj_add_state(state->apn_button,LV_STATE_DISABLED);
  else lv_obj_remove_state(state->apn_button,LV_STATE_DISABLED);
  const bool selected=state->subscription==0||state->subscription==mobile.subscription;
  const bool required=mobile.enabled&&mobile.state=="pin_required"&&selected;
  if(required)lv_obj_remove_flag(state->pin_button,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(state->pin_button,LV_OBJ_FLAG_HIDDEN);
  const std::string pin_status=mobile.sim_slot>0&&mobile.enabled?
      i18n::Format("SIM %d",mobile.sim_slot)+" / "+
        (required?i18n::Format("PIN attempts remaining: %d",mobile.pin_retries):
        i18n::Translate(mobile.sim_state=="ready"?"Ready":mobile.sim_state=="puk_required"?"PUK required":
          mobile.sim_state=="blocked"?"SIM is blocked":"Preparing SIM")):"";
  lv_label_set_text(state->pin_status,pin_status.c_str());
  if(pin_status.empty())lv_obj_add_flag(state->pin_status,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(state->pin_status,LV_OBJ_FLAG_HIDDEN);
  RefreshSavedPins(state);
}
void EditPin(lv_obj_t *screen) {
  const auto mobile=RecoveryMobileStatus();
  if(pin_overlay||RecoveryDataLocked()||mobile.state!="pin_required")return;
  prompted_request=mobile.pin_request+":"+std::to_string(mobile.pin_attempt);open_request=mobile.pin_request;
  auto *overlay=lv_obj_create(screen);Clear(overlay);pin_overlay=overlay;
  lv_obj_set_user_data(overlay,&kModalMarker);
  lv_obj_set_size(overlay,LV_PCT(100),LV_PCT(100));
  lv_obj_set_style_bg_color(overlay,kMainCanvas,0);lv_obj_set_style_bg_opa(overlay,LV_OPA_COVER,0);
  lv_obj_add_event_cb(overlay,[](lv_event_t *){pin_overlay=nullptr;},LV_EVENT_DELETE,nullptr);
  lv_obj_update_layout(overlay);
  const int width=lv_obj_get_width(overlay),height=lv_obj_get_height(overlay),inner=width-128;
  auto *title=Label(overlay,i18n::Format("Unlock SIM %d",mobile.sim_slot).c_str(),&lv_font_montserrat_40,kText);
  lv_obj_set_pos(title,64,72);lv_obj_set_width(title,inner);
  auto *attempts=Label(overlay,i18n::Format("PIN attempts remaining: %d",mobile.pin_retries).c_str(),
      &lv_font_montserrat_28,kMuted);lv_obj_set_pos(attempts,64,148);lv_obj_set_width(attempts,inner);
  auto *input=TextArea(overlay);lv_obj_set_pos(input,64,220);lv_obj_set_size(input,inner,124);
  lv_textarea_set_one_line(input,true);lv_textarea_set_password_mode(input,true);
  lv_textarea_set_max_length(input,8);lv_textarea_set_accepted_chars(input,"0123456789");
  lv_obj_set_style_text_font(input,UiFont(&lv_font_montserrat_40),0);
  lv_obj_set_style_text_color(input,kText,0);lv_obj_set_style_bg_color(input,kMainPanel,0);
  lv_obj_set_style_bg_opa(input,LV_OPA_COVER,0);lv_obj_set_style_pad_all(input,24,0);
  lv_obj_set_style_border_color(input,kAccent,0);lv_obj_set_style_border_width(input,2,0);
  auto *remember=lv_checkbox_create(overlay);lv_checkbox_set_text(remember,i18n::Translate("Remember PIN for this SIM"));
  lv_obj_set_pos(remember,64,380);lv_obj_set_size(remember,inner,112);
  lv_obj_set_style_text_font(remember,UiFont(&lv_font_montserrat_32),0);
  lv_obj_set_style_text_color(remember,kText,0);lv_obj_set_style_pad_all(remember,24,0);
  lv_obj_set_style_pad_column(remember,24,0);
  lv_obj_set_style_text_font(remember,UiFont(&lv_font_montserrat_36),LV_PART_INDICATOR);
  lv_obj_set_style_border_width(remember,2,LV_PART_INDICATOR);
  lv_obj_set_style_border_color(remember,kMuted,LV_PART_INDICATOR);
  lv_obj_set_style_radius(remember,8,LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(remember,LV_OPA_COVER,LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(remember,kMainPanel,LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(remember,kAccent,LV_PART_INDICATOR|LV_STATE_CHECKED);
  if(mobile.sim_id.empty())lv_obj_add_state(remember,LV_STATE_DISABLED);
  auto *error=Label(overlay,mobile.pin_result.empty()||mobile.pin_result=="success"?"":
      i18n::Translate(mobile.message.c_str()),&lv_font_montserrat_28,kRed);
  lv_obj_set_pos(error,64,506);lv_obj_set_width(error,inner);lv_label_set_long_mode(error,LV_LABEL_LONG_WRAP);
  auto *keyboard=lv_keyboard_create(overlay);phone_keyboard::Apply(keyboard);
  static const char *const keys[]={"1","2","3","\n","4","5","6","\n","7","8","9","\n",LV_SYMBOL_BACKSPACE,"0",LV_SYMBOL_OK,""};
  constexpr auto key=static_cast<lv_buttonmatrix_ctrl_t>(1|LV_BUTTONMATRIX_CTRL_NO_REPEAT);
  static const lv_buttonmatrix_ctrl_t controls[]={key,key,key,key,key,key,key,key,key,static_cast<lv_buttonmatrix_ctrl_t>(1),key,key};
  lv_keyboard_set_map(keyboard,LV_KEYBOARD_MODE_NUMBER,keys,controls);
  lv_keyboard_set_mode(keyboard,LV_KEYBOARD_MODE_NUMBER);lv_keyboard_set_textarea(keyboard,input);
  const int keyboard_height=std::max(320,std::min(1000,height-820));
  lv_obj_set_size(keyboard,inner,keyboard_height);lv_obj_set_align(keyboard,LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard,64,height-keyboard_height-188);
  auto *cancel=Button(overlay,"Cancel",[overlay]{lv_obj_delete_async(overlay);});
  lv_obj_set_size(cancel,(inner-24)/2,108);lv_obj_align(cancel,LV_ALIGN_BOTTOM_LEFT,64,-48);
  auto *unlock=Button(overlay,"Unlock SIM",[input,remember,error,overlay,request=mobile.pin_request]{
    const auto current=RecoveryMobileStatus();
    if(current.pin_request!=request){lv_obj_delete_async(overlay);return;}
    std::string message,pin=lv_textarea_get_text(input);
    const bool ok=RecoveryUnlockMobileSim(pin,lv_obj_has_state(remember,LV_STATE_CHECKED),&message);
    std::fill(pin.begin(),pin.end(),'\0');lv_textarea_set_text(input,"");
    if(ok)lv_obj_delete_async(overlay);
    else lv_label_set_text(error,i18n::Translate(message.c_str()));
  },true);
  lv_obj_set_size(unlock,(inner-24)/2,108);lv_obj_align(unlock,LV_ALIGN_BOTTOM_RIGHT,-64,-48);
  lv_obj_add_event_cb(keyboard,[](lv_event_t *event){
    lv_obj_send_event(static_cast<lv_obj_t *>(lv_event_get_user_data(event)),LV_EVENT_CLICKED,nullptr);
  },LV_EVENT_READY,unlock);
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
void PollMobilePinPrompt(lv_obj_t *screen,bool allow_prompt) {
  if(!MobileDataEnabled())return;
  static uint32_t last_poll=0;
  if(lv_tick_elaps(last_poll)<250)return;
  last_poll=lv_tick_get();
  const auto mobile=RecoveryMobileStatus();
  const bool required=!RecoveryDataLocked()&&!RecoveryMobileSimSwitching()&&mobile.enabled&&
      mobile.state=="pin_required"&&mobile.pin_retries>0;
  if(pin_overlay&&(!required||open_request!=mobile.pin_request))lv_obj_delete(pin_overlay);
  if(!allow_prompt||!required||pin_overlay||prompted_request==mobile.pin_request+":"+std::to_string(mobile.pin_attempt))return;
  if(!RecoveryMobilePinAutoAvailable(mobile)||mobile.pin_result=="failed"||mobile.pin_result=="unknown")EditPin(screen);
}
void BuildMobileDataScene(lv_obj_t *screen,ActionCallback callback,void *context) {
  auto *state=new MobileUi;state->screen=screen;
  state->apn=RecoveryMobileApn();state->subscription=RecoveryMobileSubscription();
  Header(screen,ConnectivityTitle(),"Networks & connectivity",callback,context);
  NetworkTabs(screen,true,callback,context);
  const int top=NetworkContentTop(screen);
  auto *content=Scroll(screen,top,std::max(200,lv_obj_get_height(screen)-top-NavigationHeight(screen)-40));
  lv_obj_set_x(content,64);lv_obj_set_width(content,lv_obj_get_width(screen)-128);
  lv_obj_set_flex_flow(content,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(content,28,0);
  auto label=[](lv_obj_t *parent,const char *text,const lv_font_t *font,lv_color_t color){
    auto *object=Label(parent,text,font,color);lv_obj_set_width(object,LV_PCT(100));
    lv_label_set_long_mode(object,LV_LABEL_LONG_WRAP);return object;};
  auto column=[](lv_obj_t *parent,int width){
    auto *object=lv_obj_create(parent);Clear(object);
    lv_obj_set_size(object,width,LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(object,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(object,28,0);
    return object;};
  lv_obj_t *connection=content,*configuration=content;
  if(Landscape(screen)) {
    lv_obj_set_flex_flow(content,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(content,32,0);
    const int width=(lv_obj_get_width(screen)-160)/2;
    connection=column(content,width);configuration=column(content,width);
  }
  auto card=[](lv_obj_t *parent){
    auto *object=lv_obj_create(parent);Panel(object,36,kMainSheet);
    lv_obj_set_size(object,LV_PCT(100),LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(object,40,0);lv_obj_set_style_pad_row(object,28,0);
    lv_obj_set_flex_flow(object,LV_FLEX_FLOW_COLUMN);return object;};
  auto *hero=card(connection);
  auto *header=lv_obj_create(hero);Clear(header);lv_obj_set_size(header,LV_PCT(100),124);
  lv_obj_set_flex_flow(header,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(header,32,0);
  lv_obj_set_flex_align(header,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  auto *plate=lv_obj_create(header);Panel(plate,34,kMainPanel);lv_obj_set_size(plate,124,124);
  state->status_icon=NetworkModeIcon(plate,true,72);lv_obj_center(state->status_icon);
  auto *heading=column(header,0);lv_obj_set_flex_grow(heading,1);lv_obj_set_style_pad_row(heading,12,0);
  state->title=label(heading,"Mobile data",&lv_font_montserrat_48,kText);
  lv_label_set_long_mode(state->title,LV_LABEL_LONG_DOT);
  state->network=label(heading,"Not connected",&lv_font_montserrat_24,kMuted);
  lv_label_set_long_mode(state->network,LV_LABEL_LONG_DOT);
  state->toggle=MobileControl(header,"Mobile data",[state]{
    state->error.clear();const auto status=RecoveryMobileStatus();
    if(status.enabled)RecoveryDisconnectMobile(&state->error);
    else RecoveryConnectMobile(state->apn,state->subscription,&state->error);
    RefreshMobile(state);
  });
  Panel(state->toggle,30,kMainPanel);lv_obj_set_size(state->toggle,400,96);
  auto *toggle_text=lv_obj_get_child(state->toggle,0);
  lv_obj_set_style_text_font(toggle_text,UiFont(&lv_font_montserrat_24),0);
  SingleLineLabel(toggle_text,244,&lv_font_montserrat_24);
  lv_obj_align(toggle_text,LV_ALIGN_LEFT_MID,24,0);
  state->radio=lv_switch_create(state->toggle);StyleSwitch(state->radio,false);
  lv_obj_align(state->radio,LV_ALIGN_RIGHT_MID,-20,0);
  state->detail=label(hero,"Reading modem status",&lv_font_montserrat_28,kMuted);
  auto *automatic=MobileControl(connection,"Auto-connect on boot",[state]{
    state->error.clear();RecoverySetMobileAutoConnect(!RecoveryMobileAutoConnect(),&state->error);
    RefreshMobile(state);
  });
  Panel(automatic,36,kMainSheet);lv_obj_set_size(automatic,LV_PCT(100),164);
  auto *automatic_title=lv_obj_get_child(automatic,0);
  lv_obj_update_layout(automatic);
  SingleLineLabel(automatic_title,lv_obj_get_width(automatic)-220,&lv_font_montserrat_32);
  lv_obj_align(automatic_title,LV_ALIGN_TOP_LEFT,40,28);
  auto *automatic_detail=Label(automatic,"Connect when recovery starts",&lv_font_montserrat_24,kMuted);
  lv_obj_set_width(automatic_detail,lv_obj_get_width(automatic)-220);
  lv_label_set_long_mode(automatic_detail,LV_LABEL_LONG_DOT);
  lv_obj_align(automatic_detail,LV_ALIGN_TOP_LEFT,40,88);
  state->auto_radio=lv_switch_create(automatic);StyleSwitch(state->auto_radio,false);
  lv_obj_align(state->auto_radio,LV_ALIGN_RIGHT_MID,-40,0);
  auto *sim_card=card(configuration);
  label(sim_card,"DATA SIM",&lv_font_montserrat_24,kMuted);
  auto *choices=lv_obj_create(sim_card);Clear(choices);lv_obj_set_size(choices,LV_PCT(100),110);
  lv_obj_set_flex_flow(choices,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(choices,16,0);
  const std::array<const char *,3> names{"Default","SIM 1","SIM 2"};
  for(int i=0;i<3;++i) {
    state->sim[i]=Button(choices,names[i],[state,i]{
      if(state->subscription==i)return;
      state->error.clear();
      if(RecoverySwitchMobileSim(state->apn,i,&state->error))state->subscription=i;
      RefreshMobile(state);
    });
    Panel(state->sim[i],30,kMainPanel);
    lv_obj_set_flex_grow(state->sim[i],1);lv_obj_set_width(state->sim[i],0);lv_obj_set_height(state->sim[i],104);
  }
  state->pin_status=label(sim_card,"",&lv_font_montserrat_28,kMuted);
  state->pin_button=Button(sim_card,"Unlock SIM",[state]{EditPin(state->screen);});
  Panel(state->pin_button,30,kMainPanel);
  lv_obj_set_size(state->pin_button,LV_PCT(100),116);
  state->saved_pins=lv_obj_create(sim_card);Clear(state->saved_pins);
  lv_obj_set_width(state->saved_pins,LV_PCT(100));lv_obj_set_height(state->saved_pins,LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(state->saved_pins,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(state->saved_pins,16,0);
  lv_obj_add_flag(state->saved_pins,LV_OBJ_FLAG_HIDDEN);
  auto *apn_card=card(configuration);
  label(apn_card,"ACCESS POINT NAME",&lv_font_montserrat_24,kMuted);
  state->apn_button=MobileControl(apn_card,"",[state]{EditApn(state);});
  Panel(state->apn_button,30,kMainPanel);
  lv_obj_set_size(state->apn_button,LV_PCT(100),116);
  state->apn_text=lv_obj_get_child(state->apn_button,0);
  lv_obj_update_layout(state->apn_button);
  SingleLineLabel(state->apn_text,lv_obj_get_width(state->apn_button)-140,&lv_font_montserrat_32);
  lv_obj_align(state->apn_text,LV_ALIGN_LEFT_MID,28,0);
  auto *arrow=Label(state->apn_button,LV_SYMBOL_RIGHT,&lv_font_montserrat_28,kMuted);
  lv_obj_align(arrow,LV_ALIGN_RIGHT_MID,-28,0);
  auto *information=card(Landscape(screen)?connection:configuration);
  label(information,"CONNECTION",&lv_font_montserrat_24,kMuted);
  auto field=[&](const char *title){
    auto *row=lv_obj_create(information);Clear(row);lv_obj_set_size(row,LV_PCT(100),LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(row,32,0);
    auto *caption=Label(row,title,&lv_font_montserrat_28,kMuted);
    lv_obj_set_width(caption,LV_PCT(38));
    auto *value=Label(row,"",&lv_font_montserrat_28,kText);
    lv_obj_set_width(value,0);lv_obj_set_flex_grow(value,1);
    lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_RIGHT,0);
    lv_label_set_long_mode(value,LV_LABEL_LONG_WRAP);return value;};
  state->address=field("Public IPv4");state->signal=field("Signal");
  label(information,"Data only. Mobile data disconnects before flashing or restoring.",
        &lv_font_montserrat_24,kMuted);
  Navigation(screen,Action::kSettings,callback,context);
  RecoveryStartMobileService();RefreshMobile(state);
  state->timer=lv_timer_create([](lv_timer_t *timer){RefreshMobile(static_cast<MobileUi *>(lv_timer_get_user_data(timer)));},500,state);
  lv_obj_add_event_cb(screen,[](lv_event_t *event){auto *state=static_cast<MobileUi *>(lv_event_get_user_data(event));
    lv_timer_delete(state->timer);delete state;},LV_EVENT_DELETE,state);
}
}
