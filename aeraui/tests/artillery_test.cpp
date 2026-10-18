/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/artillery/game.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace aeraui::artillery;

int main() {
  TapSequence taps;
  for (unsigned i = 0; i < 6; ++i) assert(!taps.Tap(i * 140));
  assert(taps.Tap(840));
  assert(!taps.Tap(950));
  assert(!taps.Tap(2500));
  assert(taps.count == 1);
  taps = {};
  for (unsigned i = 0; i < 7; ++i)
    assert(taps.Tap(UINT32_MAX - 300 + i * 140) == (i == 6));

  unsigned hits = 0, crater_count = 0;
  for (uint32_t seed = 1; seed <= 60; ++seed) {
    Game game(seed, Difficulty::kClassic), identical(seed, Difficulty::kClassic);
    assert(game.phase() == Phase::kAim);
    assert(game.player() == identical.player());
    assert(game.wind() == identical.wind());
    assert(std::abs(game.wind()) <= 30);
    for (int x = -2; x <= kWidth + 2; ++x) {
      assert(std::isfinite(game.Ground(x)));
      assert(game.Ground(x) >= 190 && game.Ground(x) <= 500);
      assert(game.Ground(x) == identical.Ground(x));
    }
    game.SetAim(-50, 300);
    assert(game.cannon(game.player()).angle == 0);
    assert(game.cannon(game.player()).power == 200);
    for (int shot = 0; shot < 24 && game.phase() != Phase::kWon; ++shot) {
      const int player = game.player();
      game.AimComputer();
      const auto aim = game.cannon(player);
      assert(aim.angle >= 0 && aim.angle <= 180);
      assert(aim.power >= 10 && aim.power <= 200);
      float ground[kTerrainSamples];
      for (int i = 0; i < kTerrainSamples; ++i) ground[i] = game.Ground(i * 2);
      assert(game.Fire());
      assert(!game.Fire());
      game.SetAim(0, 10);
      assert(game.cannon(player).angle == aim.angle);
      for (int frame = 0; frame < 2500 && game.phase() == Phase::kFlight; ++frame) {
        game.Advance(1.0f / 60);
        assert(std::isfinite(game.projectile().x) && std::isfinite(game.projectile().y));
        assert(game.trail_size() <= game.trail().size());
      }
      assert(game.phase() == Phase::kImpact);
      for (int i = 0; i < kTerrainSamples; ++i) {
        assert(game.Ground(i * 2) >= ground[i]);
        if (game.Ground(i * 2) > ground[i]) ++crater_count;
      }
      for (int frame = 0; frame < 60; ++frame) game.Advance(1.0f / 60);
      assert(game.phase() == Phase::kAim || game.phase() == Phase::kWon);
      if (game.phase() == Phase::kAim) assert(game.player() == 1 - player);
    }
    if (game.phase() == Phase::kWon) {
      ++hits;
      const unsigned wins = game.cannon(0).wins + game.cannon(1).wins;
      assert(wins <= 1);
      for (int i = 0; i < 100; ++i) game.Advance(0.1f);
      assert(game.cannon(0).wins + game.cannon(1).wins == wins);
      game.NewRound();
      assert(game.cannon(0).wins + game.cannon(1).wins == wins);
      game.NewRound(true);
      assert(game.cannon(0).wins + game.cannon(1).wins == 0);
    }
  }
  assert(hits > 45 && crater_count > 0);
  unsigned first_shot_wins = 0;
  for (uint32_t seed = 1; seed <= 100; ++seed) {
    Game easy(seed);
    assert(easy.difficulty() == Difficulty::kEasy && easy.player() == 0);
    Game unchanged = easy;
    const auto preview = easy.Preview();
    assert(preview.count > 1 && preview.count <= preview.points.size());
    assert(easy.phase() == Phase::kAim && !easy.HasImpact(0));
    assert(easy.Fire());
    for (int i = 0; i < 2000 && easy.phase() == Phase::kFlight; ++i)
      easy.Advance(1.0f / 60);
    assert(easy.phase() == Phase::kImpact);
    assert(std::hypot(easy.impact().x - preview.impact.x,
                      easy.impact().y - preview.impact.y) < 0.01f);
    if (preview.lands) {
      assert(easy.HasImpact(0));
      assert(easy.LastImpact(0).x == easy.impact().x);
    }
    Game opponent = unchanged;
    unchanged.Preview();
    unchanged.Preview();
    unchanged.AimComputer(); opponent.AimComputer();
    assert(unchanged.cannon(0).angle == opponent.cannon(0).angle);
    assert(unchanged.cannon(0).power == opponent.cannon(0).power);
    assert(opponent.Fire());
    for (int i = 0; i < 2000 && opponent.phase() == Phase::kFlight; ++i)
      opponent.Advance(1.0f / 60);
    for (int i = 0; i < 60; ++i) opponent.Advance(1.0f / 60);
    if (opponent.phase() == Phase::kWon && opponent.winner() == 0) ++first_shot_wins;
    easy.NewRound();
    assert(!easy.HasImpact(0) && !easy.HasImpact(1));
  }
  assert(first_shot_wins < 30);
  Game invalid(1);
  invalid.Advance(std::numeric_limits<float>::quiet_NaN());
  invalid.Advance(-1);
  assert(invalid.phase() == Phase::kAim);
  // The same total simulation time produces the same trajectory at 30/60 fps.
  Game slow(57), fast(57);
  slow.SetAim(65, 180); fast.SetAim(65, 180);
  assert(slow.Fire() && fast.Fire());
  for (int i = 0; i < 15; ++i) slow.Advance(1.0f / 30);
  for (int i = 0; i < 30; ++i) fast.Advance(1.0f / 60);
  assert(std::abs(slow.projectile().x - fast.projectile().x) < 0.02f);
  assert(std::abs(slow.projectile().y - fast.projectile().y) < 0.02f);
  printf("Artillery: physics, Classic, exact non-mutating previews and Easy balance passed (%u Classic matches; %u/100 Easy first-shot wins)\n", hits, first_shot_wins);
}
