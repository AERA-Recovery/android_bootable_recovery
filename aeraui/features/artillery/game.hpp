/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>
#include <cstdint>

namespace aeraui::artillery {

constexpr int kWidth = 1000;
constexpr int kHeight = 600;
constexpr int kTerrainSamples = kWidth / 2 + 1;
constexpr float kGravity = 115.0f;

struct Point { float x = 0, y = 0; };
struct Cannon {
  Point position;
  int angle = 45;
  int power = 125;
  unsigned wins = 0;
  bool alive = true;
};
enum class Phase { kAim, kFlight, kImpact, kWon };
enum class Difficulty { kEasy, kClassic };

struct Prediction {
  std::array<Point, 257> points{};
  unsigned count = 0;
  Point impact;
  bool lands = false;
};

class Game {
 public:
  explicit Game(uint32_t seed = 1, Difficulty difficulty = Difficulty::kEasy);
  void NewRound(bool reset_score = false);
  void SetAim(int angle, int power);
  bool Fire();
  // Bounded fixed-step simulation keeps trajectories independent of rendering.
  void Advance(float seconds);
  void AimComputer();
  Prediction Preview() const;
  void SetDifficulty(Difficulty difficulty) { difficulty_ = difficulty; }
  Difficulty difficulty() const { return difficulty_; }
  bool HasImpact(int side) const { return has_impact_[side == 1 ? 1 : 0]; }
  Point LastImpact(int side) const { return last_impacts_[side == 1 ? 1 : 0]; }
  float Ground(float x) const;
  Phase phase() const { return phase_; }
  int player() const { return player_; }
  int winner() const { return winner_; }
  int wind() const { return wind_; }
  const Cannon& cannon(int side) const { return cannons_[side == 1 ? 1 : 0]; }
  Point projectile() const { return projectile_; }
  Point impact() const { return impact_; }
  float impact_age() const { return impact_age_; }
  const std::array<Point, 192>& trail() const { return trail_; }
  unsigned trail_size() const { return trail_size_; }

 private:
  uint32_t Random();
  Point Muzzle(const Cannon& cannon) const;
  Point Velocity(int angle, int power) const;
  bool Collides(Point position, int ignore) const;
  void Hit(Point position);
  void Step();
  uint32_t random_;
  Difficulty difficulty_;
  std::array<float, kTerrainSamples> terrain_{};
  std::array<Cannon, 2> cannons_{};
  std::array<Point, 192> trail_{};
  unsigned trail_size_ = 0, steps_ = 0;
  int player_ = 0, winner_ = -1, wind_ = 0;
  Phase phase_ = Phase::kAim;
  Point projectile_, velocity_, impact_;
  std::array<Point, 2> last_impacts_{};
  std::array<bool, 2> has_impact_{};
  std::array<unsigned, 2> computer_shots_{};
  float accumulator_ = 0, flight_age_ = 0, impact_age_ = 0;
};

// The unlock sequence belongs to this About page, never to global UI state.
struct TapSequence {
  unsigned count = 0;
  uint32_t previous = 0;
  bool Tap(uint32_t now) {
    if (!count || now - previous > 900) count = 0;
    previous = now;
    if (++count < 7) return false;
    count = 0;
    return true;
  }
};

}  // namespace aeraui::artillery
