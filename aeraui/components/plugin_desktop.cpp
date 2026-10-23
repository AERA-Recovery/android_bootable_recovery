/* SPDX-License-Identifier: Apache-2.0 */
/* Optional generic API 2 desktop surface; no runtime or game code in host. */
#include "plugin_desktop.hpp"
#include "ui_components.hpp"
#include "play_protocol.h"
#include "phone_keyboard.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <algorithm>
#include <array>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aeraui {
namespace {
struct Contact { bool down=false; int role=0,x=0,y=0,start_x=0,start_y=0; uint32_t started=0; bool dragged=false; };
struct PlayScene {
  lv_obj_t *screen=nullptr,*image=nullptr,*notice=nullptr,*mode_label=nullptr,*keyboard=nullptr;
  lv_timer_t *timer=nullptr;
  lv_image_dsc_t descriptor{};
  int frame=-1,control=-1;
  uint8_t *pixels=nullptr;
  uint32_t sequence=0;
  bool ack=false,menu=true,exit=false,broken=false;
  int left=0,top=0,width=0,height=0;
  unsigned frame_width=960,frame_height=540;
  size_t frame_bytes=960u*540u*4u,shared_bytes=2u*960u*540u*4u;
  std::array<Contact,16> contacts{};
  std::vector<lv_obj_t *> game_controls;
  bool movement[4]{};
  bool mouse_buttons[4]{}, mouse_toolbar=false, mouse_pending=false;
  int mouse_x=0,mouse_y=0,mouse_dx=0,mouse_dy=0;
  std::function<void()> on_close;
  bool Send(uint32_t kind,int x=0,int y=0) {
    if(control<0) return false;
    // Controls use a fixed virtual layout; only guest pointer coordinates scale.
    if(kind==PLAY_POINTER){x=x*static_cast<int>(frame_width)/960;y=y*static_cast<int>(frame_height)/540;}
    PlayMessage m{AERA_PLAY_MAGIC,kind,sequence,x,y};
    if(send(control,&m,sizeof(m),MSG_DONTWAIT|MSG_NOSIGNAL)==sizeof(m)) return true;
    /* Never silently lose a key-up; disconnect tears down the guest. */
    broken=true; return false;
  }
  void FlushMouse() {
    if(!mouse_pending)return;
    if(menu)Send(PLAY_POINTER,mouse_x,mouse_y);
    else {
      while(mouse_dx||mouse_dy) {
        int dx=std::clamp(mouse_dx,-160,160),dy=std::clamp(mouse_dy,-160,160);
        Send(PLAY_LOOK,dx,dy);mouse_dx-=dx;mouse_dy-=dy;
      }
    }
    mouse_pending=false;
  }
  std::vector<unsigned> physical_keys;
  void Release() {
    physical_keys.clear();
    Send(PLAY_RELEASE); contacts={}; for(auto &v:movement)v=false;
    for(auto &v:mouse_buttons)v=false;
    mouse_pending=false;mouse_dx=mouse_dy=0;mouse_toolbar=false;
  }
  ~PlayScene() {
    if(timer)lv_timer_delete(timer);
    Send(PLAY_RELEASE); Send(PLAY_CLOSE);
    if(control>=0)close(control);
    if(image) { lv_image_set_src(image,nullptr); lv_image_cache_drop(&descriptor); }
    if(pixels)munmap(pixels,shared_bytes);
    if(frame>=0)close(frame);
  }
  bool Start(int &output_frame, int &output_input) {
    frame=memfd_create("aera-plugin-desktop",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(frame<0||ftruncate(frame,shared_bytes)||
       fcntl(frame,F_ADD_SEALS,F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL))return false;
    void *map=mmap(nullptr,shared_bytes,PROT_READ,MAP_SHARED,frame,0);
    if(map==MAP_FAILED)return false;
    pixels=static_cast<uint8_t *>(map);
    int pair[2]; if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair))return false;
    control=pair[0];
    int cf=fcntl(frame,F_DUPFD_CLOEXEC,10),cc=fcntl(pair[1],F_DUPFD_CLOEXEC,10);
    if(cf<0||cc<0) { if(cf>=0)close(cf);if(cc>=0)close(cc);close(pair[1]);return false; }
    output_frame=cf;output_input=cc;close(pair[1]);return true;
  }
};
PlayScene *active=nullptr;
void Mark(PlayScene *s,const char *text,int gx,int gy,int w=70,int h=42) {
  auto *b=lv_obj_create(s->screen);
  lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(b,w*s->width/960,h*s->height/540);
  lv_obj_set_pos(b,s->left+(gx-w/2)*s->width/960,
                  s->top+(gy-h/2)*s->height/540);
  lv_obj_set_style_bg_color(b,lv_color_hex(0x15191d),0);
  lv_obj_set_style_bg_opa(b,100,0);lv_obj_set_style_border_width(b,1,0);
  lv_obj_set_style_border_color(b,lv_color_hex(0xc8d4da),0);
  lv_obj_set_style_radius(b,12,0);
  auto *l=lv_label_create(b);lv_label_set_text(l,text);
  lv_obj_set_style_text_color(l,lv_color_white(),0);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_24,0);
  lv_obj_center(l);
  if(gx==130&&gy==32)s->mode_label=l;
  if(gy>65) {s->game_controls.push_back(b);lv_obj_add_flag(b,LV_OBJ_FLAG_HIDDEN);}
}
}

bool PluginDesktopHandlePointer(int slot,int px,int py,bool down) {
  auto *s=active;
  if(!s||lv_screen_active()!=lv_obj_get_screen(s->screen))return false;
  if(s->keyboard&&!lv_obj_has_flag(s->keyboard,LV_OBJ_FLAG_HIDDEN))return false;
  if(slot<0||slot>=16)return true;
  int x=std::clamp((px-s->left)*960/std::max(1,s->width),0,959);
  int y=std::clamp((py-s->top)*540/std::max(1,s->height),0,539);
  auto &c=s->contacts[slot];
  if(down&&!c.down) {
    c.down=true;c.x=x;c.y=y;c.start_x=x;c.start_y=y;c.started=lv_tick_get();
    if(y<65&&x<75) {c.role=10;return true;} // Exit on release.
    if(y<65&&x<190) {c.role=11;return true;}
    if(y<65&&x<290) {c.role=12;s->Send(PLAY_KEY,0xff1b,1);return true;}
    if(y<65&&x<400) {c.role=14;return true;}
    if(y<65&&x<490) {c.role=15;s->Send(PLAY_KEY,0xff0d,1);return true;}
    if(y<65&&x<580) {c.role=16;s->Send(PLAY_KEY,0xff09,1);return true;}
    if(s->menu) {c.role=1;s->Send(PLAY_POINTER,x,y);s->Send(PLAY_BUTTON,1,1);return true;}
    if(x<280&&y>240)c.role=2;
    else if(x>850&&y>440) {c.role=3;s->Send(PLAY_BUTTON,1,1);}
    else if(x>850&&y>350) {c.role=4;s->Send(PLAY_KEY,' ',1);}
    else if(x>850&&y>260) {c.role=5;s->Send(PLAY_KEY,'r',1);}
    else if(x>750&&y>440) {c.role=6;s->Send(PLAY_KEY,'f',1);}
    else if(x>650&&y>440) {c.role=7;s->Send(PLAY_KEY,0xffe3,1);}
    else if(x>550&&y>440) {c.role=8;s->Send(PLAY_BUTTON,3,1);}
    else if(x<400&&y>440) {c.role=9;s->Send(PLAY_KEY,'v',1);}
    else c.role=13;
  }
  if(!down&&c.down) {
    switch(c.role) {
      case 1:s->Send(PLAY_BUTTON,1,0);break;
      case 2: for(int i=0;i<4;i++) {s->Send(PLAY_KEY,"wasd"[i],0);s->movement[i]=false;}break;
      case 3:s->Send(PLAY_BUTTON,1,0);break;
      case 4:s->Send(PLAY_KEY,' ',0);break;
      case 5:s->Send(PLAY_KEY,'r',0);break;
      case 6:s->Send(PLAY_KEY,'f',0);break;
      case 7:s->Send(PLAY_KEY,0xffe3,0);break;
      case 8:s->Send(PLAY_BUTTON,3,0);break;
      case 9:s->Send(PLAY_KEY,'v',0);break;
      case 10:if(y<65&&x<75)s->exit=true;break;
      case 11:if(y<65&&x>=75&&x<190) {
        s->Release();s->menu=!s->menu;lv_label_set_text(s->mode_label,s->menu?"MENU":"PLAY");
        for(auto *b:s->game_controls) {
          if(s->menu)lv_obj_add_flag(b,LV_OBJ_FLAG_HIDDEN);
          else lv_obj_remove_flag(b,LV_OBJ_FLAG_HIDDEN);
        }
      }break;
      case 12:s->Send(PLAY_KEY,0xff1b,0);break;
      case 14:
        if(y<65&&x>=290&&x<400) {
          s->Release();lv_obj_remove_flag(s->keyboard,LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(s->keyboard);
        }
        break;
      case 15:s->Send(PLAY_KEY,0xff0d,0);break;
      case 16:s->Send(PLAY_KEY,0xff09,0);break;
      case 13:
        if(!c.dragged&&lv_tick_elaps(c.started)<350&&
           std::abs(x-c.start_x)<=12&&std::abs(y-c.start_y)<=12) {
          s->Send(PLAY_BUTTON,1,1);s->Send(PLAY_BUTTON,1,0);
        }
        break;
    }
    c={};return true;
  }
  if(down) {
    if(std::abs(x-c.start_x)>12||std::abs(y-c.start_y)>12)c.dragged=true;
    if(c.role==1)s->Send(PLAY_POINTER,x,y);
    if(c.role==2) {
      bool values[]={y-c.y<-22,x-c.x<-22,y-c.y>22,x-c.x>22};
      for(int i=0;i<4;i++)if(values[i]!=s->movement[i]) {
        s->movement[i]=values[i];s->Send(PLAY_KEY,"wasd"[i],values[i]);
      }
    }
    if(c.role==13) {s->Send(PLAY_LOOK,std::clamp(x-c.x,-160,160),std::clamp(y-c.y,-160,160));c.x=x;c.y=y;}
  }
  return true;
}

// Mouse has its own state; hover must not synthesize a touchscreen press.
// kind: 0 motion, 1 button, 2 wheel, 3 release (disconnect/suspend).
void PluginDesktopReleaseKeyboard() {
  auto *s=active;
  if(!s)return;
  for(unsigned key:s->physical_keys)s->Send(PLAY_KEY,key,0);
  s->physical_keys.clear();
}

bool PluginDesktopHandleKey(unsigned symbol,int value) {
  auto *s=active;
  if(!s||lv_screen_active()!=lv_obj_get_screen(s->screen))return false;
  if(value<0||value>2)return true;
  auto it=std::find(s->physical_keys.begin(),s->physical_keys.end(),symbol);
  if(value==0) {
    if(it!=s->physical_keys.end()) {
      s->Send(PLAY_KEY,symbol,0);s->physical_keys.erase(it);
    }
  } else if(value==1&&it==s->physical_keys.end()) {
    s->Send(PLAY_KEY,symbol,1);s->physical_keys.push_back(symbol);
  }
  // X11 owns repeat while held; evdev repeats must not create extra edges.
  return true;
}

bool PluginDesktopHandleMouse(int kind,int px,int py,int button,bool down,int dx,int dy) {
  auto *s=active;
  if(!s||lv_screen_active()!=lv_obj_get_screen(s->screen))return false;
  if(kind==3) {s->Release();return true;}
  if(s->keyboard&&!lv_obj_has_flag(s->keyboard,LV_OBJ_FLAG_HIDDEN))return false;
  int x=std::clamp((px-s->left)*960/std::max(1,s->width),0,959);
  int y=std::clamp((py-s->top)*540/std::max(1,s->height),0,539);
  if(kind==1&&button==1) {
    if(down&&y<65&&x<580&&!s->mouse_buttons[1])s->mouse_toolbar=true;
    if(s->mouse_toolbar) {
      PluginDesktopHandlePointer(15,px,py,down);
      if(!down)s->mouse_toolbar=false;
      return true;
    }
  }
  if(kind==0&&s->mouse_toolbar) {
    PluginDesktopHandlePointer(15,px,py,true);return true;
  }
  if(kind==0) {
    // Coalesce high-polling-rate USB motion once per frame. Button edges
    // flush the latest position first, preserving click/drag ordering.
    s->mouse_x=x;s->mouse_y=y;s->mouse_pending=true;
    s->mouse_dx=std::clamp(s->mouse_dx+dx,-960,960);
    s->mouse_dy=std::clamp(s->mouse_dy+dy,-960,960);
  } else if(kind==1&&button>=1&&button<=3) {
    s->mouse_x=x;s->mouse_y=y;s->mouse_pending=true;s->FlushMouse();
    s->mouse_buttons[button]=down;s->Send(PLAY_BUTTON,button,down);
  } else if(kind==2) {
    s->mouse_x=x;s->mouse_y=y;s->mouse_pending=true;s->FlushMouse();
    s->Send(PLAY_WHEEL,std::clamp(dx,-8,8));
  }
  return true;
}

void ClosePluginDesktop() {
  if (!active) return;
  auto *screen=active->screen;
  active=nullptr;
  lv_obj_delete(screen);
}

bool OpenPluginDesktop(lv_obj_t *parent,int &frame,int &input,std::function<void()> on_close,unsigned width,unsigned height) {
  frame=input=-1;
  if(active||!ValidDesktopResolution(width,height))return false;
  auto *s=new PlayScene;
  s->frame_width=width;s->frame_height=height;
  s->frame_bytes=static_cast<size_t>(width)*height*4;s->shared_bytes=s->frame_bytes*2;
  if(!s->Start(frame,input)){delete s;return false;}
  auto *screen=lv_obj_create(parent);
  widgets::Clear(screen);
  lv_obj_set_size(screen,lv_obj_get_width(parent),lv_obj_get_height(parent));
  lv_obj_set_pos(screen,0,0);
  lv_obj_move_foreground(screen);
  lv_obj_update_layout(screen);
  active=s;s->screen=screen;s->on_close=std::move(on_close);
  lv_obj_add_event_cb(screen,[](lv_event_t *e){auto *s=static_cast<PlayScene *>(lv_event_get_user_data(e));if(active==s)active=nullptr;delete s;},LV_EVENT_DELETE,s);
  lv_obj_set_style_bg_color(screen,lv_color_black(),0);
  lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
  lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
  int sw=lv_obj_get_width(screen),sh=lv_obj_get_height(screen);
  int scale=std::min(sw*256/static_cast<int>(width),sh*256/static_cast<int>(height));
  s->width=width*scale/256;s->height=height*scale/256;
  s->left=(sw-s->width)/2;s->top=(sh-s->height)/2;
  s->image=lv_image_create(screen);lv_image_set_pivot(s->image,0,0);
  lv_image_set_scale(s->image,scale);
  lv_obj_set_pos(s->image,s->left,s->top);
  s->descriptor.header.magic=LV_IMAGE_HEADER_MAGIC;
  s->descriptor.header.cf=LV_COLOR_FORMAT_ARGB8888;
  s->descriptor.header.flags=LV_IMAGE_FLAGS_MODIFIABLE|LV_IMAGE_FLAGS_USER1;
  s->descriptor.header.w=width;s->descriptor.header.h=height;s->descriptor.header.stride=width*4;
  s->descriptor.data_size=s->frame_bytes;
  s->notice=lv_label_create(screen);lv_label_set_text(s->notice,"Waiting for the application...\nMENU: pointer controls. PLAY: game controls.\nEXIT returns to setup.");
  lv_obj_set_style_text_color(s->notice,lv_color_white(),0);
  lv_obj_set_style_text_font(s->notice,&lv_font_montserrat_32,0);lv_obj_set_width(s->notice,sw-80);lv_obj_center(s->notice);
  Mark(s,"EXIT",40,32);Mark(s,"MENU",130,32,100);Mark(s,"ESC",240,32);
  Mark(s,"KEYS",345,32,90);Mark(s,"ENTER",445,32,85);Mark(s,"TAB",535,32,75);
  Mark(s,"MOVE",145,405,180,160);Mark(s,"FIRE",905,485);
  Mark(s,"JUMP",905,395);Mark(s,"LOAD",905,305);
  Mark(s,"USE",800,485);Mark(s,"DUCK",700,485);Mark(s,"AIM",600,485);Mark(s,"SUIT",340,485);
  s->keyboard=lv_keyboard_create(screen);
  phone_keyboard::Apply(s->keyboard);
  lv_obj_set_size(s->keyboard,sw,sh*55/100);
  lv_obj_align(s->keyboard,LV_ALIGN_BOTTOM_MID,0,0);
  lv_obj_add_flag(s->keyboard,LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s->keyboard,[](lv_event_t *event) {
    auto *state=static_cast<PlayScene *>(lv_event_get_user_data(event));
    const auto code=lv_event_get_code(event);
    if(code==LV_EVENT_READY||code==LV_EVENT_CANCEL) {
      lv_obj_add_flag(state->keyboard,LV_OBJ_FLAG_HIDDEN);return;
    }
    if(code!=LV_EVENT_VALUE_CHANGED)return;
    auto id=lv_buttonmatrix_get_selected_button(state->keyboard);
    const char *text=lv_buttonmatrix_get_button_text(state->keyboard,id);
    if(!text)return;
    int key=0;
    if(!strcmp(text,LV_SYMBOL_BACKSPACE))key=0xff08;
    else if(!strcmp(text,LV_SYMBOL_NEW_LINE))key=0xff0d;
    else if(!strcmp(text,LV_SYMBOL_LEFT))key=0xff51;
    else if(!strcmp(text,LV_SYMBOL_RIGHT))key=0xff53;
    else if(strlen(text)==1&&text[0]>=32&&text[0]<=126)key=text[0];
    if(key) {
      const bool shift=(key>='A'&&key<='Z')||(key<127&&strchr("~!@#$%^&*()_+{}|:\"<>?",key));
      if(shift)state->Send(PLAY_KEY,0xffe1,1);
      state->Send(PLAY_KEY,key,1);state->Send(PLAY_KEY,key,0);
      if(shift)state->Send(PLAY_KEY,0xffe1,0);
    }
  },LV_EVENT_ALL,s);
  s->timer=lv_timer_create([](lv_timer_t *t){
    auto *s=static_cast<PlayScene *>(lv_timer_get_user_data(t));
    if(s->exit) {auto cb=s->on_close;ClosePluginDesktop();if(cb)cb();return;}
    if(lv_screen_active()!=lv_obj_get_screen(s->screen))return;
    s->FlushMouse();
    if(s->ack) {s->Send(PLAY_ACK);s->ack=false;}
    if(s->control<0)return;
    PlayMessage m{};ssize_t n=recv(s->control,&m,sizeof(m),MSG_DONTWAIT|MSG_TRUNC);
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&!s->broken)return;
    if(n!=sizeof(m)||m.magic!=AERA_PLAY_MAGIC||m.kind!=PLAY_FRAME||
       m.x!=static_cast<int>(s->frame_width)||m.y!=static_cast<int>(s->frame_height)||m.sequence!=s->sequence+1||s->broken) {
      close(s->control);s->control=-1;
      lv_obj_remove_flag(s->notice,LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s->notice,"Application stopped. Use EXIT to return to setup.");return;
    }
    s->sequence=m.sequence;s->ack=true;
    s->descriptor.data=s->pixels+(m.sequence%2)*s->frame_bytes;
    lv_image_cache_drop(&s->descriptor);lv_image_set_src(s->image,&s->descriptor);
    lv_obj_invalidate(s->image);lv_obj_add_flag(s->notice,LV_OBJ_FLAG_HIDDEN);
  },16,s);
  return true;
}
}
