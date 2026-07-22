#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rex::ui::macos {

// AppKit exposes -isARepeat only for key-down events. Modifier transitions are
// delivered as FlagsChanged events, so their previous state must be derived
// without sending that selector to the event object.
constexpr bool GetPreviousKeyState(bool down, bool repeat_capable, bool is_repeat) {
  return down ? (repeat_capable && is_repeat) : true;
}

struct CapsLockTransition {
  bool enabled = false;
  bool emit_key_down = false;
  bool emit_key_up = false;
};

// AppKit reports Caps Lock as a latch transition rather than a dependable
// physical release. Represent each actual toggle as one balanced press/release
// pair so consumers never retain a stuck key state.
constexpr CapsLockTransition ResolveCapsLockTransition(bool was_enabled, bool is_enabled) {
  const bool changed = was_enabled != is_enabled;
  return {is_enabled, changed, changed};
}

// NSEvent may report fractional relative motion, especially for trackpads.
// Preserve the fraction across events instead of rounding every event to zero.
inline int32_t AccumulateRelativeMouseDelta(double delta, double& residual) {
  const double accumulated = delta + residual;
  if (!std::isfinite(accumulated)) {
    residual = 0.0;
    return 0;
  }

  const double quantized = std::round(accumulated);
  if (quantized >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
    residual = 0.0;
    return std::numeric_limits<int32_t>::max();
  }
  if (quantized <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
    residual = 0.0;
    return std::numeric_limits<int32_t>::min();
  }

  residual = accumulated - quantized;
  return static_cast<int32_t>(quantized);
}

struct CursorWarpPoint {
  double x = 0.0;
  double y = 0.0;
  bool valid = false;
};

// AppKit screen coordinates have a bottom-left origin and may be expressed at
// a different scale than Core Graphics display bounds. Convert through a
// normalized position so Retina and multi-display layouts are both handled.
constexpr CursorWarpPoint CalculateCursorWarpPoint(
    double screen_point_x, double screen_point_y, double appkit_screen_x, double appkit_screen_y,
    double appkit_screen_width, double appkit_screen_height, double core_graphics_display_x,
    double core_graphics_display_y, double core_graphics_display_width,
    double core_graphics_display_height) {
  if (appkit_screen_width <= 0.0 || appkit_screen_height <= 0.0 ||
      core_graphics_display_width <= 0.0 || core_graphics_display_height <= 0.0) {
    return {};
  }
  double normalized_x =
      std::clamp((screen_point_x - appkit_screen_x) / appkit_screen_width, 0.0, 1.0);
  double normalized_y = std::clamp(
      (appkit_screen_y + appkit_screen_height - screen_point_y) / appkit_screen_height, 0.0, 1.0);
  return {
      core_graphics_display_x + normalized_x * core_graphics_display_width,
      core_graphics_display_y + normalized_y * core_graphics_display_height,
      true,
  };
}

}  // namespace rex::ui::macos
