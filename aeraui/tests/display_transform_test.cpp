#include "aeraui/display_transform.hpp"
#include "aeraui/runner.hpp"

#include <cassert>

using aeraui::DisplayTransform;

int main() {
  constexpr auto metrics = aeraui::DisplayMetrics::FromThemeMetrics(
      true, 2340, 141, 70, 50);
  static_assert(metrics.adaptive_resolution);
  static_assert(metrics.logical_height == 3120);
  static_assert(metrics.status_bar_height == 141);
  static_assert(metrics.status_indent_left == 70);
  static_assert(metrics.status_indent_right == 50);
  static_assert(metrics.logical_width == 1440);

  // A wider screen: 1485 x 2376 theme units give a 1980 x 3168 canvas.
  constexpr auto wide = aeraui::DisplayMetrics::FromThemeMetrics(
      true, 2376, 165, 54, 54, 1485);
  static_assert(wide.logical_width == 1980);
  static_assert(wide.logical_height == 3168);

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

  // A 1600 x 2560 panel showing a 1980 x 3168 canvas.
  const auto tablet = DisplayTransform::Adaptive(1600, 2560, 3168, 1980);
  assert(tablet.IsValid());
  assert(tablet.IsScaled());
  assert(tablet.logical_width == 1980);
  assert(tablet.logical_height == 3168);
  assert(tablet.ToLogicalX(0) == 0);
  assert(tablet.ToLogicalX(1599) == 1979);
  assert(tablet.ToLogicalY(2559) == 3167);
  assert(tablet.ToLogicalX(800) >= 989 && tablet.ToLogicalX(800) <= 990);
  assert(tablet.ToLogicalY(1280) >= 1583 && tablet.ToLogicalY(1280) <= 1584);

  const auto native = DisplayTransform::Native(1272, 2772);
  assert(!native.IsScaled());
  assert(native.ToLogicalX(635) == 635);
  assert(native.ToLogicalY(1400) == 1400);
  return 0;
}
