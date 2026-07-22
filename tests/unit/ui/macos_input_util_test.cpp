#include "ui/macos_input_util.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using rex::ui::macos::CalculateCursorWarpPoint;
using rex::ui::macos::AccumulateRelativeMouseDelta;
using rex::ui::macos::GetPreviousKeyState;
using rex::ui::macos::ResolveCapsLockTransition;

TEST_CASE("macOS modifier events never require key-repeat selectors", "[ui][macos][input]") {
  CHECK_FALSE(GetPreviousKeyState(true, false, false));
  CHECK_FALSE(GetPreviousKeyState(true, false, true));
  CHECK_FALSE(GetPreviousKeyState(true, true, false));
  CHECK(GetPreviousKeyState(true, true, true));
  CHECK(GetPreviousKeyState(false, false, false));
}

TEST_CASE("macOS Caps Lock latch changes emit balanced key events", "[ui][macos][input]") {
  auto enabled = ResolveCapsLockTransition(false, true);
  CHECK(enabled.enabled);
  CHECK(enabled.emit_key_down);
  CHECK(enabled.emit_key_up);

  auto unchanged = ResolveCapsLockTransition(true, true);
  CHECK(unchanged.enabled);
  CHECK_FALSE(unchanged.emit_key_down);
  CHECK_FALSE(unchanged.emit_key_up);

  auto disabled = ResolveCapsLockTransition(true, false);
  CHECK_FALSE(disabled.enabled);
  CHECK(disabled.emit_key_down);
  CHECK(disabled.emit_key_up);
}

TEST_CASE("macOS cursor warp converts AppKit coordinates to Core Graphics displays",
          "[ui][macos][input][mouse]") {
  auto retina_center = CalculateCursorWarpPoint(
      /*screen point=*/720.0, 450.0,
      /*AppKit screen=*/0.0, 0.0, 1440.0, 900.0,
      /*Core Graphics display=*/0.0, 0.0, 2880.0, 1800.0);
  REQUIRE(retina_center.valid);
  CHECK(retina_center.x == 1440.0);
  CHECK(retina_center.y == 900.0);

  auto secondary_top = CalculateCursorWarpPoint(
      /*screen point=*/-960.0, 1080.0,
      /*AppKit screen=*/-1920.0, 0.0, 1920.0, 1080.0,
      /*Core Graphics display=*/-1920.0, 0.0, 1920.0, 1080.0);
  REQUIRE(secondary_top.valid);
  CHECK(secondary_top.x == -960.0);
  CHECK(secondary_top.y == 0.0);

  CHECK_FALSE(
      CalculateCursorWarpPoint(0.0, 0.0, 0.0, 0.0, 0.0, 900.0, 0.0, 0.0, 1440.0, 900.0).valid);
}

TEST_CASE("macOS relative mouse motion preserves fractional trackpad deltas",
          "[ui][macos][input][mouse]") {
  double residual = 0.0;
  CHECK(AccumulateRelativeMouseDelta(0.4, residual) == 0);
  CHECK(AccumulateRelativeMouseDelta(0.4, residual) == 1);
  CHECK(AccumulateRelativeMouseDelta(0.4, residual) == 0);
  CHECK(residual == Catch::Approx(0.2));

  residual = 0.0;
  CHECK(AccumulateRelativeMouseDelta(-0.4, residual) == 0);
  CHECK(AccumulateRelativeMouseDelta(-0.4, residual) == -1);
  CHECK(AccumulateRelativeMouseDelta(-0.4, residual) == 0);
  CHECK(residual == Catch::Approx(-0.2));
}

}  // namespace
