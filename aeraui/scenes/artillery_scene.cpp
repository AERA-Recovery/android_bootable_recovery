/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "ui_components.hpp"
#include "artillery/game.hpp"
#include "artillery/sound.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unistd.h>

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;
using artillery::Game;
using artillery::Phase;
using artillery::Sound;
constexpr float kPi = 3.14159265358979323846f;

struct Match {
  explicit Match(uint32_t seed) : game(seed) {}
  Game game;
  artillery::Prediction preview;
  Sound sound;
  lv_obj_t *screen = nullptr, *field = nullptr, *status = nullptr;
  lv_obj_t *score = nullptr, *wind = nullptr, *fire = nullptr;
  lv_obj_t *angle = nullptr, *power = nullptr, *angle_text = nullptr, *power_text = nullptr;
  lv_obj_t *mode_cpu = nullptr, *mode_two = nullptr, *sound_button = nullptr;
  lv_obj_t *easy = nullptr, *classic = nullptr;
  lv_obj_t *adjust[4]{};
  lv_timer_t* timer = nullptr;
  uint32_t previous = 0;
  float thinking = 0;
  bool cpu = true, playing_computer = false;
  ~Match() { if (timer) lv_timer_delete(timer); }
};

void ButtonText(lv_obj_t* button, const char* text) {
  lv_label_set_text(lv_obj_get_child(button, 0), text);
  FitButtonLabel(button);
}

void Refresh(Match* match) {
  const auto& game = match->game;
  const auto& cannon = game.cannon(game.player());
  const bool won = game.phase() == Phase::kWon;
  const bool cpu = match->cpu && game.player() == 1;
  const bool aiming = game.phase() == Phase::kAim && !cpu;
  match->preview = aiming && game.difficulty() == artillery::Difficulty::kEasy
      ? game.Preview() : artillery::Prediction{};
  const char* player = game.player() ? (match->cpu ? "Computer" : "Red cannon") : "Blue cannon";
  char text[128];
  if (won) {
    const char* winner = game.winner() == 0 ? "Blue cannon wins!" :
        game.winner() == 1 ? (match->cpu ? "Computer wins!" : "Red cannon wins!") : "Draw!";
    lv_label_set_text(match->status, winner);
  } else {
    snprintf(text, sizeof(text), "%s %s", player,
        game.phase() == Phase::kFlight ? "fired" :
        game.phase() == Phase::kImpact ? "- impact" : cpu ? "is aiming..." : "- your turn");
    lv_label_set_text(match->status, text);
  }
  snprintf(text, sizeof(text), "%u : %u", game.cannon(0).wins, game.cannon(1).wins);
  lv_label_set_text(match->score, text);
  snprintf(text, sizeof(text), "Wind  %s %d", game.wind() < 0 ? LV_SYMBOL_LEFT :
      game.wind() > 0 ? LV_SYMBOL_RIGHT : "", std::abs(game.wind()));
  lv_label_set_text(match->wind, text);
  snprintf(text, sizeof(text), "Angle  %d deg", cannon.angle);
  lv_label_set_text(match->angle_text, text);
  snprintf(text, sizeof(text), "Power  %d", cannon.power);
  lv_label_set_text(match->power_text, text);
  lv_slider_set_value(match->angle, cannon.angle, LV_ANIM_OFF);
  lv_slider_set_value(match->power, cannon.power, LV_ANIM_OFF);
  for (auto* object : {match->angle, match->power, match->adjust[0], match->adjust[1],
                      match->adjust[2], match->adjust[3]}) {
    if (aiming) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
  }
  ButtonText(match->fire, won ? "Next round" : LV_SYMBOL_PLAY "  Fire");
  if (aiming || won) lv_obj_remove_state(match->fire, LV_STATE_DISABLED);
  else lv_obj_add_state(match->fire, LV_STATE_DISABLED);
  for (auto* button : {match->mode_cpu, match->mode_two}) {
    const bool selected = (button == match->mode_cpu) == match->cpu;
    lv_obj_set_style_bg_color(button, selected ? kAccent : kMainPanel, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), selected ? kOnAccent : kText, 0);
  }
  for (auto* button : {match->easy, match->classic}) {
    const bool selected = (button == match->easy) ==
        (game.difficulty() == artillery::Difficulty::kEasy);
    lv_obj_set_style_bg_color(button, selected ? kAccent : kMainPanel, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), selected ? kOnAccent : kText, 0);
  }
}

void Draw(lv_event_t* event) {
  auto* match = static_cast<Match*>(lv_event_get_user_data(event));
  auto* layer = lv_event_get_layer(event);
  lv_area_t bounds;
  lv_obj_get_coords(match->field, &bounds);
  const float width = lv_area_get_width(&bounds), height = lv_area_get_height(&bounds);
  const float scale = std::min(width / artillery::kWidth, height / artillery::kHeight);
  const float left = bounds.x1 + (width - artillery::kWidth * scale) / 2;
  const float top = bounds.y2 + 1 - artillery::kHeight * scale;
  const auto x = [&](float v) { return static_cast<int32_t>(left + v * scale); };
  const auto y = [&](float v) { return static_cast<int32_t>(top + v * scale); };
  const auto point = [&](float ax, float ay) {
    return lv_point_precise_t{static_cast<lv_value_precise_t>(x(ax)),
                              static_cast<lv_value_precise_t>(y(ay))};
  };
  const auto line = [&](float ax, float ay, float bx, float by, lv_color_t color, int size) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1 = point(ax, ay); dsc.p2 = point(bx, by);
    dsc.color = color; dsc.width = std::max(1, static_cast<int>(size * scale));
    dsc.round_start = dsc.round_end = 1;
    lv_draw_line(layer, &dsc);
  };
  const auto box = [&](float ax, float ay, float bw, float bh, lv_color_t color, bool circle = false) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color; dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = circle ? LV_RADIUS_CIRCLE : 0;
    const lv_area_t area{x(ax), y(ay), x(ax + bw), y(ay + bh)};
    lv_draw_rect(layer, &dsc, &area);
  };
  // Terrain is procedural; no bundled textures, framebuffer or image cache.
  if (left > bounds.x1) {
    const int extra = static_cast<int>((left - bounds.x1) / scale) + 1;
    box(-extra, match->game.Ground(0), extra, artillery::kHeight - match->game.Ground(0),
        Color(0x39775b));
    box(artillery::kWidth, match->game.Ground(artillery::kWidth), extra,
        artillery::kHeight - match->game.Ground(artillery::kWidth), Color(0x39775b));
  }
  for (int column = 0; column < artillery::kWidth; column += 4) {
    const float ground = match->game.Ground(column);
    box(column, ground, 4, artillery::kHeight - ground, Color(0x39775b));
    line(column, ground, column + 4, match->game.Ground(column + 4), Color(0x8fd27d), 3);
  }
  for (unsigned i = 1; i < match->game.trail_size(); ++i) {
    const auto a = match->game.trail()[i - 1], b = match->game.trail()[i];
    if (i % 2) line(a.x, a.y, b.x, b.y, Color(0xffffff), 1);
  }
  if (match->game.difficulty() == artillery::Difficulty::kEasy) {
    for (int side = 0; side < 2; ++side) {
      if (!match->game.HasImpact(side)) continue;
      const auto p = match->game.LastImpact(side);
      const auto color = Color(side ? 0xe24654 : 0x167eae);
      line(p.x - 7, p.y - 7, p.x + 7, p.y + 7, Color(0xffffff), 5);
      line(p.x - 7, p.y + 7, p.x + 7, p.y - 7, Color(0xffffff), 5);
      line(p.x - 7, p.y - 7, p.x + 7, p.y + 7, color, 2);
      line(p.x - 7, p.y + 7, p.x + 7, p.y - 7, color, 2);
    }
    for (unsigned i = 1; i < match->preview.count; ++i) {
      if (!(i % 2)) continue;
      const auto a = match->preview.points[i - 1], b = match->preview.points[i];
      line(a.x, a.y, b.x, b.y, Color(0x4d6973), 2);
    }
    if (match->preview.lands) {
      const auto p = match->preview.impact;
      line(p.x - 9, p.y, p.x + 9, p.y, Color(0x26343d), 5);
      line(p.x, p.y - 9, p.x, p.y + 9, Color(0x26343d), 5);
      line(p.x - 9, p.y, p.x + 9, p.y, Color(0xffd268), 2);
      line(p.x, p.y - 9, p.x, p.y + 9, Color(0xffd268), 2);
    }
  }
  for (int side = 0; side < 2; ++side) {
    const auto& cannon = match->game.cannon(side);
    if (!cannon.alive) continue;
    const auto p = cannon.position;
    const auto color = Color(side ? 0xe24654 : 0x167eae);
    const float radians = cannon.angle * kPi / 180;
    if (match->game.player() == side && match->game.phase() == Phase::kAim)
      box(p.x - 25, p.y + 14, 50, 3, Color(0xffd268));
    box(p.x - 17, p.y - 6, 34, 15, color, true);
    line(p.x, p.y - 3, p.x + std::cos(radians) * 25,
         p.y - 3 - std::sin(radians) * 25, Color(0x26343d), 9);
    box(p.x - 14, p.y + 1, 13, 13, Color(0x26343d), true);
    box(p.x + 3, p.y + 1, 13, 13, Color(0x26343d), true);
    line(p.x - 30, p.y + 7, p.x - 30, p.y - 42, Color(0x26343d), 2);
    lv_draw_triangle_dsc_t flag;
    lv_draw_triangle_dsc_init(&flag);
    const int flutter = match->game.wind() < 0 ? -1 : 1;
    flag.p[0] = point(p.x - 30, p.y - 42);
    flag.p[1] = point(p.x - 30 + 24 * flutter, p.y - 34);
    flag.p[2] = point(p.x - 30, p.y - 26);
    flag.color = color;
    lv_draw_triangle(layer, &flag);
  }
  if (match->game.phase() == Phase::kFlight) {
    const auto p = match->game.projectile();
    box(p.x - 3, p.y - 3, 6, 6, Color(0x17272e), true);
    // A high arc can leave the sky temporarily; keep its horizontal position visible.
    if (y(p.y) < bounds.y1 + 12 && p.x >= 0 && p.x <= artillery::kWidth)
      box(p.x - 4, (bounds.y1 + 18 - top) / scale, 8, 8, Color(0xffd268), true);
  }
  if (match->game.phase() == Phase::kImpact && match->game.impact_age() < 0.6f) {
    const auto p = match->game.impact();
    const float radius = 8 + match->game.impact_age() * 60;
    for (int ray = 0; ray < 12; ++ray) {
      const float radians = ray * kPi / 6;
      line(p.x + std::cos(radians) * radius * 0.3f,
           p.y + std::sin(radians) * radius * 0.3f,
           p.x + std::cos(radians) * radius,
           p.y + std::sin(radians) * radius, Color(ray % 2 ? 0xffd268 : 0xf46d42), 3);
    }
  }
}

void NewRound(Match* match, bool reset) {
  match->game.NewRound(reset);
  match->thinking = 0;
  match->playing_computer = false;
  Refresh(match);
  lv_obj_invalidate(match->field);
}

void Tick(lv_timer_t* timer) {
  auto* match = static_cast<Match*>(lv_timer_get_user_data(timer));
  const uint32_t now = lv_tick_get();
  const float dt = std::min((now - match->previous) / 1000.0f, 0.08f);
  match->previous = now;
  if (lv_screen_active() != match->screen) return;
  const Phase before = match->game.phase();
  match->game.Advance(dt);
  if (before != match->game.phase()) {
    if (match->game.phase() == Phase::kImpact) match->sound.Play('h');
    if (match->game.phase() == Phase::kWon) match->sound.Play('w');
    match->thinking = 0;
    match->playing_computer = false;
    Refresh(match);
  }
  if (match->cpu && match->game.player() == 1 && match->game.phase() == Phase::kAim) {
    match->thinking += dt;
    if (!match->playing_computer && match->thinking > 0.35f) {
      match->game.AimComputer();
      match->playing_computer = true;
      Refresh(match);
    }
    if (match->thinking > 1.0f && match->game.Fire()) {
      match->sound.Play('s');
      Refresh(match);
    }
  }
  if (before == Phase::kFlight || before == Phase::kImpact || before != match->game.phase())
    lv_obj_invalidate(match->field);
}

void AimRow(Match* match, lv_obj_t* parent, int y, int width, bool power) {
  auto*& text = power ? match->power_text : match->angle_text;
  auto*& slider = power ? match->power : match->angle;
  text = Label(parent, "", &lv_font_montserrat_40, kText);
  lv_obj_set_pos(text, 0, y);
  lv_obj_set_width(text, width - 200);
  slider = lv_slider_create(parent);
  lv_obj_set_pos(slider, 24, y + 100);
  lv_obj_set_size(slider, width - 260, 24);
  lv_slider_set_range(slider, power ? 10 : 0, power ? 200 : 180);
  lv_obj_set_style_bg_color(slider, kLine, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, kAccent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, kAccent, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 24, LV_PART_KNOB);
  lv_obj_set_ext_click_area(slider, 32);
  lv_obj_add_event_cb(slider, [](lv_event_t* event) {
    auto* match = static_cast<Match*>(lv_event_get_user_data(event));
    if (match->cpu && match->game.player() == 1) return;
    match->game.SetAim(lv_slider_get_value(match->angle), lv_slider_get_value(match->power));
    Refresh(match);
    lv_obj_invalidate(match->field);
  }, LV_EVENT_VALUE_CHANGED, match);
  for (int i = 0; i < 2; ++i) {
    auto* button = Button(parent, i ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS, [match, power, i] {
      const auto cannon = match->game.cannon(match->game.player());
      const int delta = i ? 1 : -1;
      match->game.SetAim(cannon.angle + (power ? 0 : delta),
                         cannon.power + (power ? delta : 0));
      Refresh(match);
      lv_obj_invalidate(match->field);
    });
    lv_obj_set_pos(button, width - 184 + i * 98, y + 58);
    lv_obj_set_size(button, 86, 94);
    match->adjust[(power ? 2 : 0) + i] = button;
  }
}
}

void BuildArtilleryScene(lv_obj_t* screen, ActionCallback callback, void* context) {
  auto* match = new Match(lv_tick_get() ^ static_cast<uint32_t>(getpid() * 7919));
  match->screen = screen;
  lv_obj_add_event_cb(screen, [](lv_event_t* event) {
    delete static_cast<Match*>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, match);
  Header(screen, "Bang! Bang!", "AERA Artillery", callback, context);
  const int w = lv_obj_get_width(screen), h = lv_obj_get_height(screen);
  const bool landscape = w > h;
  const int bottom = h - NavigationHeight(screen) - (landscape ? 45 : 80);
  const int controls_x = landscape ? w - 820 : 80;
  const int controls_y = landscape ? 440 : bottom - 730;
  const int controls_w = landscape ? 740 : w - 160;
  const int field_top = landscape ? 430 : 540;
  const int field_w = landscape ? w - 990 : w - 160;
  const int field_h = landscape ? bottom - field_top : controls_y - field_top - 65;

  match->status = Label(screen, "", &lv_font_montserrat_40, kText);
  lv_obj_set_pos(match->status, 80, landscape ? 340 : 434);
  lv_obj_set_width(match->status, landscape ? w - 1800 : 920);
  lv_label_set_long_mode(match->status, LV_LABEL_LONG_DOT);
  match->score = Label(screen, "", &lv_font_montserrat_40, kAccent);
  lv_obj_set_pos(match->score, landscape ? w - 1570 : w - 360, landscape ? 340 : 434);
  lv_obj_set_width(match->score, 280);
  lv_obj_set_style_text_align(match->score, LV_TEXT_ALIGN_RIGHT, 0);
  match->wind = Label(screen, "", &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_pos(match->wind, controls_x, landscape ? 350 : controls_y - 2);
  // On portrait the wind sits in the sky rather than sharing the first aim row.
  if (!landscape) lv_obj_set_pos(match->wind, 112, field_top + 28);

  match->field = lv_obj_create(screen);
  Clear(match->field);
  lv_obj_set_pos(match->field, 80, field_top);
  lv_obj_set_size(match->field, field_w, field_h);
  lv_obj_set_style_bg_color(match->field, Color(0xb9e0e5), 0);
  lv_obj_set_style_bg_opa(match->field, LV_OPA_COVER, 0);
  lv_obj_remove_flag(match->field, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(match->field, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(match->field, Draw, LV_EVENT_DRAW_MAIN, match);
  if (!landscape) {
    lv_obj_move_foreground(match->wind);
    lv_obj_set_style_text_color(match->wind, Color(0x26343d), 0);
  }

  auto* controls = lv_obj_create(screen);
  Clear(controls);
  lv_obj_set_pos(controls, controls_x, controls_y);
  lv_obj_set_size(controls, controls_w, bottom - controls_y);
  AimRow(match, controls, 0, controls_w, false);
  AimRow(match, controls, 190, controls_w, true);
  match->fire = Button(controls, LV_SYMBOL_PLAY "  Fire", [match] {
    if (match->game.phase() == Phase::kWon) { NewRound(match, false); return; }
    if (match->game.Fire()) { match->sound.Play('s'); Refresh(match); }
  }, true);
  lv_obj_set_pos(match->fire, 0, 390);
  lv_obj_set_size(match->fire, controls_w - 248, 120);
  match->sound_button = Button(controls, LV_SYMBOL_VOLUME_MAX, [match] {
    ButtonText(match->sound_button, match->sound.Toggle() ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
  });
  lv_obj_set_pos(match->sound_button, controls_w - 224, 390);
  lv_obj_set_size(match->sound_button, 100, 120);
  auto* reset = Button(controls, LV_SYMBOL_REFRESH, [match] { NewRound(match, true); });
  lv_obj_set_pos(reset, controls_w - 100, 390);
  lv_obj_set_size(reset, 100, 120);
  const int mode_width = (controls_w - 24) / 2;
  match->easy = Button(controls, "Easy", [match] {
    if (match->game.difficulty() != artillery::Difficulty::kEasy) {
      match->game.SetDifficulty(artillery::Difficulty::kEasy);
      NewRound(match, true);
    }
  });
  match->classic = Button(controls, "Classic", [match] {
    if (match->game.difficulty() != artillery::Difficulty::kClassic) {
      match->game.SetDifficulty(artillery::Difficulty::kClassic);
      NewRound(match, true);
    }
  });
  lv_obj_set_pos(match->easy, 0, 540);
  lv_obj_set_size(match->easy, mode_width, 80);
  lv_obj_set_pos(match->classic, mode_width + 24, 540);
  lv_obj_set_size(match->classic, mode_width, 80);
  match->mode_cpu = Button(controls, "vs CPU", [match] {
    if (!match->cpu) { match->cpu = true; NewRound(match, true); }
  });
  match->mode_two = Button(controls, "2 players", [match] {
    if (match->cpu) { match->cpu = false; NewRound(match, true); }
  });
  lv_obj_set_pos(match->mode_cpu, 0, 650);
  lv_obj_set_size(match->mode_cpu, mode_width, 80);
  lv_obj_set_pos(match->mode_two, mode_width + 24, 650);
  lv_obj_set_size(match->mode_two, mode_width, 80);

  Navigation(screen, Action::kNone, callback, context, true);
  match->previous = lv_tick_get();
  match->timer = lv_timer_create(Tick, 33, match);
  Refresh(match);
}

}  // namespace aeraui
