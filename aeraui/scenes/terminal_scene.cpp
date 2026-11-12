/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <lvgl.h>

#include "design.hpp"
#include "phone_keyboard.hpp"
#include "ui_components.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

struct TerminalUi {
  lv_obj_t *screen = nullptr;
  lv_obj_t *viewport = nullptr;
  lv_obj_t *output = nullptr;
  lv_obj_t *command = nullptr;
  lv_obj_t *command_bar = nullptr;
  lv_obj_t *controls = nullptr;
  lv_obj_t *keyboard = nullptr;
  lv_timer_t *timer = nullptr;
  int update_counter = -1;
  bool keyboard_visible = false;
  bool landscape = false;
};

void LayoutTerminal(TerminalUi *state) {
  lv_obj_update_layout(state->screen);
  const int width = lv_obj_get_width(state->screen);
  const int height = lv_obj_get_height(state->screen);
  const int top = state->landscape ? 350 : 430;
  const int bottom = height - 64;
  int viewport_width = width - 128;
  int viewport_bottom;
  int controls_x = 64;
  int controls_y;
  int controls_width = width - 128;
  int controls_height = 112;
  int columns = 6;

  if (state->landscape) {
    const int right_width = state->keyboard_visible
        ? width * 55 / 100 : std::min(864, (width - 192) / 3);
    controls_x = width - 64 - right_width;
    controls_width = right_width;
    controls_y = top;
    controls_height = state->keyboard_visible ? 112 : 284;
    columns = state->keyboard_visible ? 6 : 3;
    viewport_width = controls_x - 108;
    viewport_bottom = std::min(1250, bottom);
    const int keyboard_y = top + 150;
    lv_obj_set_pos(state->keyboard, controls_x, keyboard_y);
    lv_obj_set_size(state->keyboard, width - controls_x,
                    std::min(768, bottom - keyboard_y));
  } else {
    const int keyboard_height = std::min(1098, (bottom - top) * 55 / 100);
    const int keyboard_y = std::min(2070, bottom - keyboard_height);
    lv_obj_set_pos(state->keyboard, 0, keyboard_y);
    lv_obj_set_size(state->keyboard, width, keyboard_height);
    controls_y = state->keyboard_visible ? keyboard_y - 160
                                        : std::min(2720, bottom - controls_height);
    viewport_bottom = controls_y - 30;
  }

  lv_obj_set_pos(state->viewport, 64, top);
  lv_obj_set_size(state->viewport, viewport_width, viewport_bottom - top);
  lv_obj_set_pos(state->controls, controls_x, controls_y);
  lv_obj_set_size(state->controls, controls_width, controls_height);
  const int gap = state->landscape ? 20 : 16;
  const int button_width = (controls_width - (columns - 1) * gap) / columns;
  for (int index = 0; index < 6; ++index) {
    auto *button = lv_obj_get_child(state->controls, index);
    lv_obj_set_pos(button, (index % columns) * (button_width + gap),
                    (index / columns) * 132);
    lv_obj_set_size(button, button_width, 112);
  }
}

void LayoutCommandLine(TerminalUi *state) {
  if (state == nullptr || state->viewport == nullptr ||
      state->output == nullptr || state->command_bar == nullptr) return;
  lv_obj_update_layout(state->viewport);
  const int content_width = std::max(320,
      static_cast<int>(lv_obj_get_width(state->viewport)) - 68);
  lv_obj_set_width(state->output, content_width);
  lv_obj_update_layout(state->output);
  const int content_height = std::max(140,
      static_cast<int>(lv_obj_get_height(state->viewport)) - 68);
  const int line_y = std::max(static_cast<int>(lv_obj_get_height(state->output)) + 18,
                              content_height - 116);
  lv_obj_set_pos(state->command_bar, 0, line_y);
  lv_obj_set_size(state->command_bar, content_width, 116);
  lv_obj_set_size(state->command, content_width - 52, 104);
}

void RefreshOutput(TerminalUi *state, bool force = false) {
  if (state == nullptr) return;
  RecoveryTerminalPoll();
  const int update = RecoveryTerminalUpdateCounter();
  if (!force && update == state->update_counter) return;
  state->update_counter = update;

  const auto lines = RecoveryTerminalLines(400);
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    text += lines[i];
    if (i + 1 < lines.size()) text.push_back('\n');
  }
  if (text.empty()) text = "Starting recovery shell…";
  i18n::BindLabel(state->output, text.c_str());
  LayoutCommandLine(state);
  lv_obj_update_layout(state->viewport);
  lv_obj_scroll_to_y(state->viewport, LV_COORD_MAX, LV_ANIM_OFF);
}

void SetKeyboardVisible(TerminalUi *state, bool visible) {
  if (state == nullptr || state->keyboard_visible == visible) return;
  state->keyboard_visible = visible;
  LayoutTerminal(state);
  if (visible) {
    lv_obj_remove_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(state->keyboard);
    lv_keyboard_set_textarea(state->keyboard, state->command);
  } else {
    lv_obj_add_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(state->command, LV_STATE_FOCUSED);
  }
  RefreshOutput(state, true);
}

void SendCommand(TerminalUi *state) {
  if (state == nullptr) return;
  const std::string command = lv_textarea_get_text(state->command);
  if (!command.empty()) RecoveryTerminalWrite(command);
  RecoveryTerminalWrite("\n");
  lv_textarea_set_text(state->command, "");
  SetKeyboardVisible(state, false);
}

void TerminalTick(lv_timer_t *timer) {
  RefreshOutput(static_cast<TerminalUi *>(lv_timer_get_user_data(timer)));
}

void DeleteTerminalUi(lv_event_t *event) {
  auto *state = static_cast<TerminalUi *>(lv_event_get_user_data(event));
  if (state == nullptr) return;
  if (state->timer != nullptr) lv_timer_delete(state->timer);
  delete state;
}

lv_obj_t *Control(TerminalUi *state, const char *label, TerminalKey key) {
  auto *button = Button(state->controls, label, [key] {
    RecoveryTerminalSendKey(key);
  });
  lv_obj_set_style_radius(button, 30, 0);
  return button;
}

}  // namespace

void BuildTerminalScene(lv_obj_t *screen, ActionCallback callback,
                        void *context) {
  auto *state = new TerminalUi;
  state->screen = screen;
  state->landscape = Landscape(screen);
  lv_obj_add_event_cb(screen, DeleteTerminalUi, LV_EVENT_DELETE, state);

  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  auto *heading = Label(screen, "Terminal", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 80, state->landscape ? 210 : 252);
  auto *subtitle = Label(screen, "AERA shell  /  live recovery session",
                         &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, state->landscape ? 600 : 80,
                 state->landscape ? 220 : 332);
  auto *live = Kicker(screen, "LIVE RECOVERY SHELL", kGreen);
  lv_obj_align(live, LV_ALIGN_TOP_RIGHT, -80,
               state->landscape ? 286 : 342);

  state->viewport = lv_obj_create(screen);
  Panel(state->viewport, 34, Color(0x101216));
  lv_obj_set_style_border_width(state->viewport, 1, 0);
  lv_obj_set_style_border_color(state->viewport, kMainLine, 0);
  lv_obj_set_style_border_opa(state->viewport, LV_OPA_60, 0);
  lv_obj_set_style_pad_all(state->viewport, 34, 0);
  lv_obj_add_flag(state->viewport, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(state->viewport, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->viewport, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(state->viewport, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(state->viewport, 5, LV_PART_SCROLLBAR);

  state->output = Label(state->viewport, "Starting recovery shell…",
                        &lv_font_montserrat_24, Color(0xe9f0f4));
  lv_obj_set_style_text_line_space(state->output, 8, 0);

  state->controls = lv_obj_create(screen);
  Clear(state->controls);
  Control(state, "Esc", TerminalKey::kEscape);
  Control(state, "Tab", TerminalKey::kTab);
  Control(state, LV_SYMBOL_UP, TerminalKey::kUp);
  Control(state, LV_SYMBOL_DOWN, TerminalKey::kDown);
  Control(state, "Ctrl+C", TerminalKey::kInterrupt);
  auto *clear = Button(state->controls, "Clear", [state] {
    RecoveryTerminalClear();
    RefreshOutput(state, true);
  });
  lv_obj_set_style_radius(clear, 30, 0);

  state->command_bar = lv_obj_create(state->viewport);
  Clear(state->command_bar);
  lv_obj_set_style_border_width(state->command_bar, 1, 0);
  lv_obj_set_style_border_side(state->command_bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(state->command_bar, kMainLine, 0);
  lv_obj_set_style_border_opa(state->command_bar, LV_OPA_60, 0);
  auto *prompt = Label(state->command_bar, "$", &lv_font_montserrat_32,
                       kAccent);
  lv_obj_set_pos(prompt, 0, 40);
  state->command = TextArea(state->command_bar);
  lv_obj_set_pos(state->command, 52, 6);
  lv_textarea_set_one_line(state->command, true);
  lv_textarea_set_placeholder_text(state->command, "Type a command…");
  lv_obj_set_style_text_font(state->command, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(state->command, kText, 0);
  lv_obj_set_style_text_color(state->command, kMuted,
                              LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_opa(state->command, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(state->command, 0, 0);
  lv_obj_set_style_pad_all(state->command, 22, 0);

  state->keyboard = lv_keyboard_create(screen);
  phone_keyboard::Apply(state->keyboard);
  lv_obj_set_align(state->keyboard, LV_ALIGN_TOP_LEFT);
  lv_keyboard_set_textarea(state->keyboard, state->command);
  lv_obj_add_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
  LayoutTerminal(state);

  lv_obj_add_event_cb(state->command, [](lv_event_t *event) {
    SetKeyboardVisible(static_cast<TerminalUi *>(lv_event_get_user_data(event)),
                       true);
  }, LV_EVENT_CLICKED, state);
  lv_obj_add_event_cb(state->command, [](lv_event_t *event) {
    SendCommand(static_cast<TerminalUi *>(lv_event_get_user_data(event)));
  }, LV_EVENT_READY, state);
  text_edit::SetControlHandler(state->command, [](lv_obj_t *input, unsigned key, void *) {
    if (key == 3 || key == 4) {
      const char bytes[] = {static_cast<char>(key), 0};
      RecoveryTerminalWrite(bytes);
      if (key == 3) lv_textarea_set_text(input, "");
    }
  }, state);
  lv_obj_add_event_cb(state->keyboard, [](lv_event_t *event) {
    auto *state = static_cast<TerminalUi *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY) SendCommand(state);
    else if (lv_event_get_code(event) == LV_EVENT_CANCEL)
      SetKeyboardVisible(state, false);
  }, LV_EVENT_ALL, state);

  RecoveryTerminalStart(state->landscape ? 112 : 78,
                        state->landscape ? 38 : 42,
                        state->landscape ? 2024 : 1224,
                        state->landscape ? 820 : 1860);
  RefreshOutput(state, true);
  state->timer = lv_timer_create(TerminalTick, 32, state);
  AnimateEnter(state->viewport, 20, 16);
}

}  // namespace aeraui
