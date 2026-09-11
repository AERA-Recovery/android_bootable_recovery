#include "recovery_ui2/display_transform.hpp"
#include "recovery_ui2/runner.hpp"

#include <cassert>

using recovery_ui2::DisplayTransform;

int main() {
  constexpr auto metrics = recovery_ui2::DisplayMetrics::FromThemeMetrics(
      true, 2340, 141);
  static_assert(metrics.adaptive_resolution);
  static_assert(metrics.logical_height == 3120);
  static_assert(metrics.status_bar_height == 141);

  const auto dodge = DisplayTransform::Adaptive(1440, 3168);
  assert(dodge.IsValid());
  assert(!dodge.IsScaled());
  assert(dodge.ToLogicalX(0) == 0);
  assert(dodge.ToLogicalX(1439) == 1439);
  assert(dodge.ToLogicalY(3167) == 3167);

  const auto infiniti = DisplayTransform::Adaptive(1272, 2772, 3120);
  assert(infiniti.IsValid());
  assert(infiniti.IsScaled());
  assert(infiniti.logical_width == 1440);
  assert(infiniti.logical_height == 3120);
  assert(infiniti.ToLogicalX(0) == 0);
  assert(infiniti.ToLogicalX(1271) == 1439);
  assert(infiniti.ToLogicalY(2771) == 3119);
  assert(infiniti.ToLogicalX(636) >= 719 &&
         infiniti.ToLogicalX(636) <= 720);
  assert(infiniti.ToLogicalY(1386) >= 1559 &&
         infiniti.ToLogicalY(1386) <= 1560);

  const auto generic = DisplayTransform::Adaptive(1080, 2400);
  assert(generic.ToLogicalX(-20) == 0);
  assert(generic.ToLogicalY(2500) == 3167);

  const auto native = DisplayTransform::Native(1272, 2772);
  assert(!native.IsScaled());
  assert(native.ToLogicalX(635) == 635);
  assert(native.ToLogicalY(1400) == 1400);
  return 0;
}
