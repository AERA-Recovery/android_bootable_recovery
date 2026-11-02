// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <algorithm>

namespace aeraui {
// Consume contacts carried across an unlock, including their final releases.
// New contacts are accepted after every unlocking finger has been lifted.
class UnlockTouchGate {
 public:
  void Observe(int slot,bool pressed) {
    if(slot<0||slot>=static_cast<int>(contacts_.size()))return;
    contacts_[slot]=pressed;
    if(std::none_of(contacts_.begin(),contacts_.end(),[](bool down){return down;}))blocked_=false;
  }
  bool Filter(int slot,bool pressed) {
    const bool consume=blocked_;
    Observe(slot,pressed);
    return consume;
  }
  void Arm() {
    blocked_=std::any_of(contacts_.begin(),contacts_.end(),[](bool down){return down;});
  }
  bool Blocked()const{return blocked_;}
 private:
  std::array<bool,2> contacts_{};
  bool blocked_=false;
};
}
