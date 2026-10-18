/* SPDX-License-Identifier: Apache-2.0 */
#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aeraui::artillery {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kStep = 1.0f / 120.0f;
constexpr float kCrater = 32.0f;
float Distance(Point a, Point b) {
  return std::hypot(a.x - b.x, a.y - b.y);
}
}

Game::Game(uint32_t seed, Difficulty difficulty)
    : random_(seed ? seed : 1), difficulty_(difficulty) { NewRound(); }

uint32_t Game::Random() {
  random_ ^= random_ << 13;
  random_ ^= random_ >> 17;
  random_ ^= random_ << 5;
  return random_;
}

void Game::NewRound(bool reset_score) {
  const float a = (Random() % 6283) / 1000.0f;
  const float b = (Random() % 6283) / 1000.0f;
  const float c = (Random() % 6283) / 1000.0f;
  for (int i = 0; i < kTerrainSamples; ++i) {
    const float x = i * 2.0f / kWidth;
    terrain_[i] = std::clamp(360.0f + 88 * std::sin(x * 5 + a) +
        57 * std::sin(x * 10 + b) + 26 * std::sin(x * 19 + c), 195.0f, 495.0f);
  }
  for (int side = 0; side < 2; ++side) {
    auto& cannon = cannons_[side];
    cannon.position.x = side ? 840.0f + Random() % 65 : 95.0f + Random() % 65;
    const float ground = Ground(cannon.position.x);
    for (int i = 0; i < kTerrainSamples; ++i)
      if (std::abs(i * 2 - cannon.position.x) < 23) terrain_[i] = ground;
    cannon.position.y = ground - 8;
    cannon.alive = true;
    cannon.angle = side ? 135 : 45;
    cannon.power = 125;
    if (reset_score) cannon.wins = 0;
  }
  wind_ = static_cast<int>(Random() % 61) - 30;
  player_ = static_cast<int>(Random() % 2);
  if (difficulty_ == Difficulty::kEasy) player_ = 0;
  winner_ = -1;
  phase_ = Phase::kAim;
  trail_size_ = steps_ = 0;
  accumulator_ = flight_age_ = impact_age_ = 0;
  has_impact_ = {};
  computer_shots_ = {};
}

float Game::Ground(float x) const {
  const float sample = std::clamp(x / 2, 0.0f, float(kTerrainSamples - 1));
  const int i = std::min(static_cast<int>(sample), kTerrainSamples - 2);
  return terrain_[i] + (terrain_[i + 1] - terrain_[i]) * (sample - i);
}

Point Game::Muzzle(const Cannon& cannon) const {
  const float angle = cannon.angle * kPi / 180;
  return {cannon.position.x + std::cos(angle) * 24,
          cannon.position.y - std::sin(angle) * 24};
}

Point Game::Velocity(int angle, int power) const {
  const float radians = angle * kPi / 180;
  return {std::cos(radians) * power * 2.8f,
          -std::sin(radians) * power * 2.8f};
}

void Game::SetAim(int angle, int power) {
  if (phase_ != Phase::kAim) return;
  cannons_[player_].angle = std::clamp(angle, 0, 180);
  cannons_[player_].power = std::clamp(power, 10, 200);
}

bool Game::Fire() {
  if (phase_ != Phase::kAim) return false;
  projectile_ = Muzzle(cannons_[player_]);
  velocity_ = Velocity(cannons_[player_].angle, cannons_[player_].power);
  phase_ = Phase::kFlight;
  accumulator_ = flight_age_ = 0;
  trail_size_ = steps_ = 0;
  trail_[trail_size_++] = projectile_;
  return true;
}

bool Game::Collides(Point position, int ignore) const {
  for (int side = 0; side < 2; ++side)
    if (side != ignore && cannons_[side].alive &&
        Distance(position, cannons_[side].position) <= 15) return true;
  return position.x >= 0 && position.x <= kWidth && position.y >= Ground(position.x);
}

void Game::Hit(Point position) {
  impact_ = position;
  impact_age_ = 0;
  phase_ = Phase::kImpact;
  last_impacts_[player_] = position;
  has_impact_[player_] = position.x >= 0 && position.x <= kWidth &&
      position.y >= 0 && position.y <= kHeight;
  if (position.x < 0 || position.x > kWidth) return;
  for (int i = 0; i < kTerrainSamples; ++i) {
    const float dx = i * 2 - position.x;
    if (std::abs(dx) < kCrater)
      terrain_[i] = std::min(float(kHeight - 8), std::max(terrain_[i],
          position.y + std::sqrt(kCrater * kCrater - dx * dx)));
  }
  for (auto& cannon : cannons_) {
    if (Distance(position, cannon.position) < kCrater + 8) cannon.alive = false;
    cannon.position.y = Ground(cannon.position.x) - 8;
  }
  if (!cannons_[0].alive || !cannons_[1].alive) {
    winner_ = cannons_[0].alive ? 0 : cannons_[1].alive ? 1 : -1;
    if (winner_ >= 0) ++cannons_[winner_].wins;
  }
}

void Game::Step() {
  const Point old = projectile_;
  velocity_.x += wind_ * kStep;
  velocity_.y += kGravity * kStep;
  projectile_.x += velocity_.x * kStep;
  projectile_.y += velocity_.y * kStep;
  flight_age_ += kStep;
  // Sweep each step: a fast low shot must not skip a cannon or a thin ridge.
  const int samples = std::max(1, static_cast<int>(Distance(old, projectile_) / 2) + 1);
  for (int i = 1; i <= samples; ++i) {
    const float t = float(i) / samples;
    const Point point{old.x + (projectile_.x - old.x) * t,
                      old.y + (projectile_.y - old.y) * t};
    if (Collides(point, flight_age_ < 0.15f ? player_ : -1)) {
      Hit(point);
      return;
    }
  }
  if (++steps_ % 5 == 0) {
    if (trail_size_ == trail_.size()) {
      for (size_t i = 1; i < trail_.size(); ++i) trail_[i - 1] = trail_[i];
      --trail_size_;
    }
    trail_[trail_size_++] = projectile_;
  }
  if (projectile_.x < -100 || projectile_.x > kWidth + 100 ||
      projectile_.y > kHeight + 100 || flight_age_ > 16) Hit(projectile_);
}

void Game::Advance(float seconds) {
  if (!std::isfinite(seconds) || seconds <= 0) return;
  seconds = std::min(seconds, 0.1f);
  if (phase_ == Phase::kImpact) {
    impact_age_ += seconds;
    if (impact_age_ >= 0.85f) {
      phase_ = (!cannons_[0].alive || !cannons_[1].alive) ? Phase::kWon : Phase::kAim;
      if (phase_ == Phase::kAim) player_ = 1 - player_;
    }
  } else if (phase_ == Phase::kFlight) {
    accumulator_ += seconds;
    while (accumulator_ >= kStep && phase_ == Phase::kFlight) {
      accumulator_ -= kStep;
      Step();
    }
  }
}

Prediction Game::Preview() const {
  Prediction result;
  if (phase_ != Phase::kAim) return result;
  // Run a bounded copy through the same swept collisions as a real shot.
  // Previewing never advances the match, changes scores or consumes randomness.
  Game simulated = *this;
  simulated.Fire();
  result.points[result.count++] = simulated.projectile_;
  for (unsigned step = 0; step < 1922 && simulated.phase_ == Phase::kFlight; ++step) {
    simulated.Step();
    if ((step % 8 == 0 || simulated.phase_ != Phase::kFlight) &&
        result.count < result.points.size())
      result.points[result.count++] = simulated.phase_ == Phase::kImpact
          ? simulated.impact_ : simulated.projectile_;
  }
  result.impact = simulated.impact_;
  result.lands = simulated.phase_ == Phase::kImpact &&
      result.impact.x >= 0 && result.impact.x <= kWidth &&
      result.impact.y >= 0 && result.impact.y <= kHeight;
  return result;
}

void Game::AimComputer() {
  if (phase_ != Phase::kAim) return;
  if (difficulty_ == Difficulty::kEasy) {
    const unsigned shots = computer_shots_[player_]++;
    const int angle = (player_ ? 120 : 60) + static_cast<int>(Random() % 15) - 7;
    Cannon candidate = cannons_[player_];
    candidate.angle = angle;
    const Point start = Muzzle(candidate), target = cannons_[1 - player_].position;
    const float radians = angle * kPi / 180;
    // Easy uses a rough ballistic estimate, not Classic's terrain-aware search.
    // Wind is underestimated, and sizeable power errors leave room to learn.
    const float assumed_wind = wind_ * std::min(0.65f, shots * 0.15f);
    const float time_squared = 2 * ((target.y - start.y) +
        (target.x - start.x) * std::tan(radians)) /
        (kGravity + assumed_wind * std::tan(radians));
    const float time = std::sqrt(std::max(0.1f, time_squared));
    const float speed = ((target.x - start.x) - 0.5f * assumed_wind * time_squared) /
        (std::cos(radians) * time);
    const int spread = shots < 3 ? 24 : 14;
    const int error = static_cast<int>(Random() % (spread * 2 + 1)) - spread;
    SetAim(angle, static_cast<int>(speed / 2.8f) + error);
    return;
  }
  float best = std::numeric_limits<float>::max();
  int best_angle = 135, best_power = 125;
  const auto target = cannons_[1 - player_].position;
  // Search the actual terrain/physics, but do not mutate the live match.
  for (int angle = player_ ? 95 : 15; angle <= (player_ ? 165 : 85); angle += 5) {
    Cannon candidate = cannons_[player_];
    candidate.angle = angle;
    for (int power = 55; power <= 200; power += 4) {
      Point position = Muzzle(candidate), velocity = Velocity(angle, power);
      for (int step = 0; step < 720; ++step) {
        constexpr float dt = 1.0f / 60;
        velocity.x += wind_ * dt;
        velocity.y += kGravity * dt;
        position.x += velocity.x * dt;
        position.y += velocity.y * dt;
        const float distance = Distance(position, target);
        if (distance < best) { best = distance; best_angle = angle; best_power = power; }
        if (position.x < 0 || position.x > kWidth || Collides(position, -1)) break;
      }
    }
  }
  SetAim(best_angle + static_cast<int>(Random() % 5) - 2,
         best_power + static_cast<int>(Random() % 9) - 4);
}

}  // namespace aeraui::artillery
