// SPDX-License-Identifier: Apache-2.0
#include "lock_credential.hpp"
#include "ui_components.hpp"
#include "phone_keyboard.hpp"
#include "scene.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
namespace aeraui {
namespace {
using namespace design;
using namespace widgets;
struct Prompt {
 lv_obj_t *root{},*keyboard{},*status{},*submit{},*current{},*secret{},*repeat{};
 lv_timer_t* timer{};
 LockCredentialAction action;
 uint64_t generation=0;
 std::function<void()> completed;
 bool busy=false;
 std::atomic<bool> done{false};
 lock_security::Result result;
};
void Wipe(std::string& text) {if(!text.empty()){volatile char* p=text.data();for(size_t i=0;i<text.size();++i)p[i]=0;}text.clear();}
struct PinPrompt : std::enable_shared_from_this<PinPrompt> {
 lv_obj_t* root{};
 DecryptScene page;
 lv_timer_t* timer{};
 uint64_t generation=0;
 std::function<void()> completed;
 bool busy=false;
 std::atomic<bool> done{false};
 lock_security::Result result;
};
lv_obj_t* OpenPinUnlock(lv_obj_t* parent,std::function<void()> completed,uint64_t generation) {
 auto state=std::make_shared<PinPrompt>();state->completed=std::move(completed);state->generation=generation;
 auto* root=lv_obj_create(parent);state->root=root;Clear(root);
 lv_obj_set_size(root,LV_PCT(100),LV_PCT(100));lv_obj_set_pos(root,0,0);lv_obj_update_layout(root);
 lv_obj_set_user_data(root,&kModalMarker);
 state->page=BuildDecryptScene(root,3,true,0,3,[](Action action,void* context){
   auto state=static_cast<PinPrompt*>(context)->shared_from_this();
   if(state->busy)return;
   if(action==Action::kDecryptSkip){lv_obj_delete_async(state->root);return;}
   if(action!=Action::kDecryptSubmit)return;
   auto secret=GetDecryptCredential(state->page);state->busy=true;
   SetDecryptBusy(state->page);lv_textarea_set_text(state->page.input,"");lv_obj_add_state(state->page.input,LV_STATE_DISABLED);
   std::thread([state,secret=std::move(secret)]()mutable {
     state->result=RecoveryVerifyScreenLock(state->generation,secret);Wipe(secret);
     state->done.store(true,std::memory_order_release);
   }).detach();
 },state.get(),{},true);
 lv_obj_send_event(state->page.input,LV_EVENT_FOCUSED,nullptr);
 struct Submit {std::shared_ptr<PinPrompt> state;};
 auto* enter=new Submit{state};
 lv_obj_add_event_cb(state->page.input,[](lv_event_t* event){
   auto state=static_cast<Submit*>(lv_event_get_user_data(event))->state;
   if(!state->busy)lv_obj_send_event(state->page.submit,LV_EVENT_CLICKED,nullptr);
 },LV_EVENT_READY,enter);
 lv_obj_add_event_cb(state->page.input,[](lv_event_t* event){delete static_cast<Submit*>(lv_event_get_user_data(event));},LV_EVENT_DELETE,enter);
 auto* holder=new std::shared_ptr<PinPrompt>(state);
 state->timer=lv_timer_create([](lv_timer_t* timer){
   auto state=*static_cast<std::shared_ptr<PinPrompt>*>(lv_timer_get_user_data(timer));
   if(!state->done.load(std::memory_order_acquire))return;
   state->done.store(false,std::memory_order_relaxed);
   if(state->result.ok){auto callback=state->completed;lv_obj_delete_async(state->root);if(callback)callback();return;}
   state->busy=false;CompleteDecryptAttempt(state->page,false);lv_obj_remove_state(state->page.input,LV_STATE_DISABLED);
   lv_label_set_text(state->page.status,i18n::Translate(state->result.error.c_str()));
 },50,holder);
 lv_obj_add_event_cb(root,[](lv_event_t* event){
   auto* holder=static_cast<std::shared_ptr<PinPrompt>*>(lv_event_get_user_data(event));
   lv_timer_delete((*holder)->timer);(*holder)->completed={};delete holder;
 },LV_EVENT_DELETE,holder);
 lv_obj_move_foreground(root);return root;
}
}
lv_obj_t* OpenLockCredential(lv_obj_t* parent,LockCredentialAction action,std::function<void()> completed,uint64_t generation) {
 auto state=std::make_shared<Prompt>();state->action=action;state->completed=std::move(completed);state->generation=generation;
 const auto existing=RecoveryScreenLockStatus();
 if(action==LockCredentialAction::Verify&&existing.kind==lock_security::Kind::Pin&&lv_obj_get_width(parent)<lv_obj_get_height(parent))
   return OpenPinUnlock(parent,std::move(state->completed),generation);
 auto* root=lv_obj_create(parent);state->root=root;Clear(root);
 lv_obj_set_size(root,LV_PCT(100),LV_PCT(100));lv_obj_set_pos(root,0,0);lv_obj_update_layout(root);
 MainBackground(root);lv_obj_set_user_data(root,&kModalMarker);
 const int w=lv_obj_get_width(parent),h=lv_obj_get_height(parent);const bool wide=w>h;
 const int margin=std::max(20,w/24);
 auto* form=lv_obj_create(root);Clear(form);Panel(form,28,kMainPanel);
 lv_obj_set_pos(form,margin,margin);lv_obj_set_width(form,wide?w*46/100:w-2*margin);
 lv_obj_set_height(form,LV_SIZE_CONTENT);lv_obj_set_style_max_height(form,wide?h-2*margin:h*58/100-margin,0);
 lv_obj_set_style_pad_all(form,28,0);lv_obj_set_flex_flow(form,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(form,18,0);
 lv_obj_add_flag(form,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(form,LV_DIR_VER);
 const bool setting=action==LockCredentialAction::SetPin||action==LockCredentialAction::SetPassword;
 const bool pin=action==LockCredentialAction::SetPin||(!setting&&existing.kind==lock_security::Kind::Pin);
 const char* title=action==LockCredentialAction::Verify?"Unlock AERA":action==LockCredentialAction::SetPin?"Set screen-lock PIN":action==LockCredentialAction::SetPassword?"Set screen-lock password":action==LockCredentialAction::Remove?"Remove screen lock":action==LockCredentialAction::EnableFingerprint?"Enable fingerprint unlock":"Disable fingerprint unlock";
 auto* heading=Label(form,title,&lv_font_montserrat_40,kText);lv_obj_set_width(heading,LV_PCT(100));
 auto* detail=Label(form,setting?"This protects AERA's lock screen. It does not change your Android screen lock.":"Enter your AERA screen-lock credential.",&lv_font_montserrat_28,kMuted);
 lv_obj_set_width(detail,LV_PCT(100));lv_label_set_long_mode(detail,LV_LABEL_LONG_WRAP);
 auto* keyboard=lv_keyboard_create(root);state->keyboard=keyboard;phone_keyboard::Apply(keyboard);
 static const char* const pin_map[]={"1","2","3","\n","4","5","6","\n","7","8","9","\n",LV_SYMBOL_BACKSPACE,"0",LV_SYMBOL_OK,""};
 static const lv_buttonmatrix_ctrl_t pin_controls[]={
   phone_keyboard::Key(),phone_keyboard::Key(),phone_keyboard::Key(),
   phone_keyboard::Key(),phone_keyboard::Key(),phone_keyboard::Key(),
   phone_keyboard::Key(),phone_keyboard::Key(),phone_keyboard::Key(),
   phone_keyboard::Key(),phone_keyboard::Key(),phone_keyboard::Key()};
 lv_keyboard_set_map(keyboard,LV_KEYBOARD_MODE_NUMBER,pin_map,pin_controls);
 if(wide){lv_obj_set_size(keyboard,w*46/100,h-2*margin);lv_obj_align(keyboard,LV_ALIGN_RIGHT_MID,-margin,0);}
 else {lv_obj_set_size(keyboard,w-2*margin,h*38/100);lv_obj_align(keyboard,LV_ALIGN_BOTTOM_MID,0,-margin);}
 auto field=[&](const char* placeholder,bool numeric) {
   auto* input=TextArea(form);lv_obj_set_size(input,LV_PCT(100),128);
   lv_obj_set_style_text_font(input,UiFont(&lv_font_montserrat_36),0);
   lv_obj_set_style_text_color(input,kText,0);lv_obj_set_style_text_color(input,kMuted,LV_PART_TEXTAREA_PLACEHOLDER);
   lv_obj_set_style_bg_color(input,kMainCanvas,0);lv_obj_set_style_bg_opa(input,LV_OPA_COVER,0);
   lv_obj_set_style_border_color(input,kMainLine,0);lv_obj_set_style_border_width(input,1,0);
   lv_obj_set_style_border_color(input,kAccent,LV_STATE_FOCUSED);lv_obj_set_style_border_width(input,2,LV_STATE_FOCUSED);
   lv_obj_set_style_radius(input,20,0);lv_obj_set_style_pad_all(input,24,0);
   lv_textarea_set_one_line(input,true);lv_textarea_set_max_length(input,numeric?16:128);
   lv_textarea_set_placeholder_text(input,placeholder);lv_textarea_set_password_mode(input,true);
   lv_textarea_set_password_show_time(input,0);
   if(numeric)lv_textarea_set_accepted_chars(input,"0123456789");
   struct Focus {lv_obj_t* keyboard;bool numeric;};auto* focus=new Focus{keyboard,numeric};
   lv_obj_add_event_cb(input,[](lv_event_t* e){auto* f=static_cast<Focus*>(lv_event_get_user_data(e));
     lv_keyboard_set_textarea(f->keyboard,lv_event_get_target_obj(e));lv_keyboard_set_mode(f->keyboard,f->numeric?LV_KEYBOARD_MODE_NUMBER:LV_KEYBOARD_MODE_TEXT_LOWER);
   },LV_EVENT_FOCUSED,focus);
   lv_obj_add_event_cb(input,[](lv_event_t* e){delete static_cast<Focus*>(lv_event_get_user_data(e));},LV_EVENT_DELETE,focus);
   return input;
 };
 if(existing.configured||!setting)state->current=field(existing.kind==lock_security::Kind::Pin?"Current AERA PIN":"Current AERA password",existing.kind==lock_security::Kind::Pin);
 if(setting){state->secret=field(pin?"New PIN (4–16 digits)":"New password (6–128 characters)",pin);state->repeat=field(pin?"Confirm PIN":"Confirm password",pin);}
 auto* first=state->current?state->current:state->secret;lv_keyboard_set_textarea(keyboard,first);
 lv_keyboard_set_mode(keyboard,(state->current?existing.kind==lock_security::Kind::Pin:pin)?LV_KEYBOARD_MODE_NUMBER:LV_KEYBOARD_MODE_TEXT_LOWER);
 state->status=Label(form,"",&lv_font_montserrat_28,kMuted);lv_obj_set_width(state->status,LV_PCT(100));lv_label_set_long_mode(state->status,LV_LABEL_LONG_WRAP);
 auto submit=[state] {
   if(state->busy)return;
   std::string current=state->current?lv_textarea_get_text(state->current):"";
   std::string secret=state->secret?lv_textarea_get_text(state->secret):"";
   std::string repeat=state->repeat?lv_textarea_get_text(state->repeat):"";
   state->busy=true;lv_obj_add_state(state->submit,LV_STATE_DISABLED);lv_obj_add_state(state->keyboard,LV_STATE_DISABLED);
   for(auto* field:{state->current,state->secret,state->repeat})if(field){lv_obj_add_state(field,LV_STATE_DISABLED);lv_textarea_set_text(field,"");}
   lv_label_set_text(state->status,i18n::Translate("Checking..."));
   std::thread([state,current=std::move(current),secret=std::move(secret),repeat=std::move(repeat)]()mutable {
     switch(state->action) {
       case LockCredentialAction::Verify:state->result=RecoveryVerifyScreenLock(state->generation,current);break;
       case LockCredentialAction::SetPin:case LockCredentialAction::SetPassword:
         state->result=RecoverySetScreenLock(current,state->action==LockCredentialAction::SetPin?lock_security::Kind::Pin:lock_security::Kind::Password,secret,repeat);break;
       case LockCredentialAction::Remove:state->result=RecoveryRemoveScreenLock(current);break;
       case LockCredentialAction::EnableFingerprint:case LockCredentialAction::DisableFingerprint:
         state->result=RecoverySetScreenFingerprint(current,state->action==LockCredentialAction::EnableFingerprint);break;
     }
     Wipe(current);Wipe(secret);Wipe(repeat);state->done.store(true,std::memory_order_release);
   }).detach();
 };
 auto* ready_handler=new Handler(submit);
 lv_obj_add_event_cb(keyboard,[](lv_event_t* event){(*static_cast<Handler*>(lv_event_get_user_data(event)))();},LV_EVENT_READY,ready_handler);
 lv_obj_add_event_cb(keyboard,[](lv_event_t* event){delete static_cast<Handler*>(lv_event_get_user_data(event));},LV_EVENT_DELETE,ready_handler);
 state->submit=Button(form,action==LockCredentialAction::Verify?"Unlock":"Confirm",submit,true);lv_obj_set_size(state->submit,LV_PCT(100),108);
 auto* cancel=Button(form,"Cancel",[state]{if(!state->busy)lv_obj_delete_async(state->root);});lv_obj_set_size(cancel,LV_PCT(100),100);
 auto* holder=new std::shared_ptr<Prompt>(state);
 state->timer=lv_timer_create([](lv_timer_t* timer){
   auto state=*static_cast<std::shared_ptr<Prompt>*>(lv_timer_get_user_data(timer));
   if(!state->done.load(std::memory_order_acquire))return;
   state->done.store(false,std::memory_order_relaxed);
   if(state->result.ok){auto callback=state->completed;lv_obj_delete_async(state->root);if(callback)callback();return;}
   state->busy=false;lv_obj_remove_state(state->submit,LV_STATE_DISABLED);lv_obj_remove_state(state->keyboard,LV_STATE_DISABLED);
   for(auto* field:{state->current,state->secret,state->repeat})if(field)lv_obj_remove_state(field,LV_STATE_DISABLED);
   lv_label_set_text(state->status,i18n::Translate(state->result.error.c_str()));
 },50,holder);
 lv_obj_add_event_cb(root,[](lv_event_t* e){auto* holder=static_cast<std::shared_ptr<Prompt>*>(lv_event_get_user_data(e));
   lv_timer_delete((*holder)->timer);(*holder)->completed={};delete holder;
 },LV_EVENT_DELETE,holder);
 lv_obj_update_layout(form);
 lv_obj_set_y(form,margin+std::max(0,((wide?h-2*margin:h*58/100-margin)-static_cast<int>(lv_obj_get_height(form)))/2));
 lv_obj_move_foreground(root);return root;
}
}
