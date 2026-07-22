#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/input/input.h>

namespace rex::input {

// Stable names and ranges used by both controller backends and player-facing
// settings UIs. All settings are hot-reloaded.
inline constexpr const char* kControllerLookSensitivityCvar =
    "controller_look_sensitivity";
inline constexpr const char* kControllerMoveDeadzoneCvar =
    "controller_move_deadzone";
inline constexpr const char* kControllerAimDeadzoneCvar =
    "controller_aim_deadzone";
inline constexpr const char* kControllerInvertYCvar = "controller_invert_y";
inline constexpr const char* kControllerRumbleEnabledCvar =
    "controller_rumble_enabled";
inline constexpr const char* kControllerRumbleIntensityCvar =
    "controller_rumble_intensity";
inline constexpr const char* kControllerLayoutCvar = "controller_layout";
inline constexpr const char* kControllerButtonMapCvar =
    "controller_button_map";

inline constexpr double kControllerLookSensitivityMin = 0.25;
inline constexpr double kControllerLookSensitivityMax = 3.0;
inline constexpr double kControllerDeadzoneMin = 0.0;
inline constexpr double kControllerDeadzoneMax = 0.5;
inline constexpr double kControllerRumbleIntensityMin = 0.0;
inline constexpr double kControllerRumbleIntensityMax = 1.0;

struct ControllerTuning {
  double look_sensitivity = 1.0;
  double move_deadzone = 0.0;
  double aim_deadzone = 0.0;
  bool invert_y = false;
};

// A raw and guest-configured controller sample for host UI. Unlike guest
// GetState, this remains live while a modal host menu suppresses game input.
struct ControllerSnapshot {
  bool connected = false;
  bool input_active = false;
  bool rumble_supported = false;
  uint32_t user_index = 0;
  std::string name;
  X_INPUT_GAMEPAD raw_gamepad = {};
  X_INPUT_GAMEPAD gamepad = {};
};

namespace controller {

enum class Layout : uint8_t {
  kModern,
  kClassic,
  kSouthpaw,
};

inline constexpr std::array<std::string_view, 3> kLayoutNames = {
    "modern", "classic", "southpaw"};

inline bool ParseLayout(std::string_view name, Layout* out_layout) {
  if (!out_layout) {
    return false;
  }
  for (size_t i = 0; i < kLayoutNames.size(); ++i) {
    if (name == kLayoutNames[i]) {
      *out_layout = static_cast<Layout>(i);
      return true;
    }
  }
  return false;
}

inline std::string_view LayoutName(Layout layout) {
  const size_t index = static_cast<size_t>(layout);
  return index < kLayoutNames.size() ? kLayoutNames[index]
                                     : kLayoutNames[0];
}

// Logical guest controls and the physical controls that may feed them. The
// two pseudo-controls are valid only as binding sources.
enum class Control : uint8_t {
  kA,
  kB,
  kX,
  kY,
  kLeftShoulder,
  kRightShoulder,
  kLeftTrigger,
  kRightTrigger,
  kLeftThumb,
  kRightThumb,
  kBack,
  kStart,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  kCount,
  kDefault,
  kNone,
};

inline constexpr size_t kControlCount = static_cast<size_t>(Control::kCount);
inline constexpr uint8_t kDigitalControlThreshold = 30;

inline constexpr std::array<std::string_view, kControlCount>
    kControlNames = {"a",         "b",          "x",         "y",
                     "lb",        "rb",         "lt",        "rt",
                     "l3",        "r3",         "back",      "start",
                     "dpad_up",   "dpad_down",  "dpad_left", "dpad_right"};
inline constexpr std::array<std::string_view, kControlCount>
    kControlDisplayNames = {"A",          "B",           "X",          "Y",
                            "LB",         "RB",          "LT",         "RT",
                            "L3",         "R3",          "Back",       "Start",
                            "D-pad Up",   "D-pad Down",  "D-pad Left", "D-pad Right"};

inline std::string_view ControlName(Control control) {
  const size_t index = static_cast<size_t>(control);
  if (index < kControlNames.size()) {
    return kControlNames[index];
  }
  return control == Control::kDefault ? "default" : "none";
}

inline std::string_view ControlDisplayName(Control control) {
  const size_t index = static_cast<size_t>(control);
  if (index < kControlDisplayNames.size()) {
    return kControlDisplayNames[index];
  }
  return control == Control::kDefault ? "Default" : "Unbound";
}

inline bool ParseControl(std::string_view name, bool allow_pseudo,
                         Control* out_control) {
  if (!out_control) {
    return false;
  }
  for (size_t i = 0; i < kControlNames.size(); ++i) {
    if (name == kControlNames[i]) {
      *out_control = static_cast<Control>(i);
      return true;
    }
  }
  if (allow_pseudo && name == "default") {
    *out_control = Control::kDefault;
    return true;
  }
  if (allow_pseudo && name == "none") {
    *out_control = Control::kNone;
    return true;
  }
  return false;
}

struct ButtonBindings {
  ButtonBindings() { sources.fill(Control::kDefault); }
  std::array<Control, kControlCount> sources;
};

inline std::string_view Trim(std::string_view text) {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' ||
          text.front() == '\r' || text.front() == '\n')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' ||
          text.back() == '\r' || text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return text;
}

// Compact persisted format: "a=b,rt=rb". Missing targets retain their preset
// defaults, allowing a layout change without erasing explicit button choices.
inline bool ParseButtonBindings(std::string_view text,
                                ButtonBindings* out_bindings) {
  if (!out_bindings) {
    return false;
  }
  ButtonBindings parsed;
  text = Trim(text);
  if (text.empty()) {
    *out_bindings = parsed;
    return true;
  }

  std::array<bool, kControlCount> seen = {};
  while (!text.empty()) {
    const size_t comma = text.find(',');
    std::string_view item = Trim(text.substr(0, comma));
    if (item.empty()) {
      return false;
    }
    const size_t equals = item.find('=');
    if (equals == std::string_view::npos ||
        item.find('=', equals + 1) != std::string_view::npos) {
      return false;
    }

    Control target;
    Control source;
    if (!ParseControl(Trim(item.substr(0, equals)), false, &target) ||
        !ParseControl(Trim(item.substr(equals + 1)), true, &source)) {
      return false;
    }
    const size_t target_index = static_cast<size_t>(target);
    if (seen[target_index]) {
      return false;
    }
    seen[target_index] = true;
    parsed.sources[target_index] = source;

    if (comma == std::string_view::npos) {
      text = {};
    } else {
      text.remove_prefix(comma + 1);
      if (Trim(text).empty()) {
        return false;
      }
    }
  }
  *out_bindings = parsed;
  return true;
}

inline std::string SerializeButtonBindings(const ButtonBindings& bindings) {
  std::string result;
  for (size_t i = 0; i < bindings.sources.size(); ++i) {
    const Control source = bindings.sources[i];
    if (source == Control::kDefault) {
      continue;
    }
    if (!result.empty()) {
      result += ',';
    }
    result += kControlNames[i];
    result += '=';
    result += ControlName(source);
  }
  return result;
}

inline Control DefaultSource(Layout layout, Control target) {
  if (layout == Layout::kSouthpaw) {
    if (target == Control::kLeftThumb) {
      return Control::kRightThumb;
    }
    if (target == Control::kRightThumb) {
      return Control::kLeftThumb;
    }
  }
  return target;
}

inline Control ResolveSource(Layout layout, const ButtonBindings& bindings,
                             Control target) {
  const size_t index = static_cast<size_t>(target);
  if (index >= bindings.sources.size()) {
    return Control::kNone;
  }
  const Control source = bindings.sources[index];
  return source == Control::kDefault ? DefaultSource(layout, target) : source;
}

// UI-facing assignment helper. Physical sources are one-to-one by default: if
// a newly selected source already feeds another logical control, that control
// receives the target's old source. This makes A -> B behave like a swap
// instead of producing A+B from a single press. Unbound is intentionally
// repeatable.
inline bool AssignSourceWithSwap(Layout layout, ButtonBindings* bindings,
                                 Control target, Control requested_source) {
  if (!bindings || static_cast<size_t>(target) >= kControlCount ||
      requested_source == Control::kCount) {
    return false;
  }
  const size_t target_index = static_cast<size_t>(target);
  if (bindings->sources[target_index] == requested_source) {
    return false;
  }

  const Control old_source = ResolveSource(layout, *bindings, target);
  const Control new_source =
      requested_source == Control::kDefault
          ? DefaultSource(layout, target)
          : requested_source;
  if (new_source != Control::kNone && new_source != old_source) {
    for (size_t i = 0; i < kControlCount; ++i) {
      const Control other_target = static_cast<Control>(i);
      if (other_target != target &&
          ResolveSource(layout, *bindings, other_target) == new_source) {
        bindings->sources[i] = old_source;
        break;
      }
    }
  }
  bindings->sources[target_index] = requested_source;
  return true;
}

inline uint16_t ControlButtonMask(Control control) {
  switch (control) {
    case Control::kA:
      return X_INPUT_GAMEPAD_A;
    case Control::kB:
      return X_INPUT_GAMEPAD_B;
    case Control::kX:
      return X_INPUT_GAMEPAD_X;
    case Control::kY:
      return X_INPUT_GAMEPAD_Y;
    case Control::kLeftShoulder:
      return X_INPUT_GAMEPAD_LEFT_SHOULDER;
    case Control::kRightShoulder:
      return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    case Control::kLeftThumb:
      return X_INPUT_GAMEPAD_LEFT_THUMB;
    case Control::kRightThumb:
      return X_INPUT_GAMEPAD_RIGHT_THUMB;
    case Control::kBack:
      return X_INPUT_GAMEPAD_BACK;
    case Control::kStart:
      return X_INPUT_GAMEPAD_START;
    case Control::kDpadUp:
      return X_INPUT_GAMEPAD_DPAD_UP;
    case Control::kDpadDown:
      return X_INPUT_GAMEPAD_DPAD_DOWN;
    case Control::kDpadLeft:
      return X_INPUT_GAMEPAD_DPAD_LEFT;
    case Control::kDpadRight:
      return X_INPUT_GAMEPAD_DPAD_RIGHT;
    default:
      return 0;
  }
}

inline uint8_t ReadControl(const X_INPUT_GAMEPAD& source, Control control) {
  if (control == Control::kLeftTrigger) {
    return source.left_trigger;
  }
  if (control == Control::kRightTrigger) {
    return source.right_trigger;
  }
  const uint16_t mask = ControlButtonMask(control);
  return mask && (static_cast<uint16_t>(source.buttons) & mask) ? 255 : 0;
}

inline void WriteControl(X_INPUT_GAMEPAD* target, Control control,
                         uint8_t value) {
  if (control == Control::kLeftTrigger) {
    target->left_trigger = value;
    return;
  }
  if (control == Control::kRightTrigger) {
    target->right_trigger = value;
    return;
  }
  const uint16_t mask = ControlButtonMask(control);
  if (mask && value > kDigitalControlThreshold) {
    target->buttons = static_cast<uint16_t>(target->buttons) | mask;
  }
}

// Converts physical input to the logical XInput layout seen by the guest.
// Axis presets are applied before tuning so move/look deadzones keep their
// semantic roles and are never rewritten by selecting a preset.
inline X_INPUT_GAMEPAD ApplyMapping(const X_INPUT_GAMEPAD& source,
                                    Layout layout,
                                    const ButtonBindings& bindings) {
  X_INPUT_GAMEPAD result = {};
  result.buttons = static_cast<uint16_t>(source.buttons) & X_INPUT_GAMEPAD_GUIDE;

  switch (layout) {
    case Layout::kClassic:
      result.thumb_lx = source.thumb_rx;
      result.thumb_ly = source.thumb_ly;
      result.thumb_rx = source.thumb_lx;
      result.thumb_ry = source.thumb_ry;
      break;
    case Layout::kSouthpaw:
      result.thumb_lx = source.thumb_rx;
      result.thumb_ly = source.thumb_ry;
      result.thumb_rx = source.thumb_lx;
      result.thumb_ry = source.thumb_ly;
      break;
    case Layout::kModern:
    default:
      result.thumb_lx = source.thumb_lx;
      result.thumb_ly = source.thumb_ly;
      result.thumb_rx = source.thumb_rx;
      result.thumb_ry = source.thumb_ry;
      break;
  }

  for (size_t i = 0; i < kControlCount; ++i) {
    const Control target = static_cast<Control>(i);
    WriteControl(&result, target,
                 ReadControl(source, ResolveSource(layout, bindings, target)));
  }
  return result;
}

inline float AxisToUnit(int16_t value) {
  return value < 0 ? static_cast<float>(value) / 32768.0f
                   : static_cast<float>(value) / 32767.0f;
}

inline int16_t UnitToAxis(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  value = std::clamp(value, -1.0f, 1.0f);
  const float scale = value < 0.0f ? 32768.0f : 32767.0f;
  return static_cast<int16_t>(std::lround(value * scale));
}

// Radial deadzones avoid the square corners and diagonal speed changes caused
// by applying independent per-axis cutoffs. The remaining range is remapped so
// full stick deflection always remains full deflection.
inline void ApplyRadialDeadzone(int16_t input_x, int16_t input_y,
                                double requested_deadzone, int16_t* output_x,
                                int16_t* output_y) {
  const float x = AxisToUnit(input_x);
  const float y = AxisToUnit(input_y);
  const float magnitude = std::sqrt(x * x + y * y);
  const float deadzone = static_cast<float>(std::isfinite(requested_deadzone)
                                                ? std::clamp(requested_deadzone,
                                                             kControllerDeadzoneMin,
                                                             kControllerDeadzoneMax)
                                                : 0.0);
  // The defaults must be bit-exact pass-through. Besides avoiding needless
  // rounding, this preserves square-range controllers at diagonal extremes.
  if (!(deadzone > 0.0f)) {
    *output_x = input_x;
    *output_y = input_y;
    return;
  }
  if (!(magnitude > deadzone)) {
    *output_x = 0;
    *output_y = 0;
    return;
  }

  const float remapped_magnitude =
      std::min(1.0f, (magnitude - deadzone) / (1.0f - deadzone));
  const float direction_scale = remapped_magnitude / magnitude;
  *output_x = UnitToAxis(x * direction_scale);
  *output_y = UnitToAxis(y * direction_scale);
}

inline X_INPUT_GAMEPAD ApplyTuning(const X_INPUT_GAMEPAD& source,
                                   const ControllerTuning& requested) {
  X_INPUT_GAMEPAD result = source;

  int16_t left_x = 0;
  int16_t left_y = 0;
  ApplyRadialDeadzone(static_cast<int16_t>(source.thumb_lx),
                      static_cast<int16_t>(source.thumb_ly),
                      requested.move_deadzone, &left_x, &left_y);
  result.thumb_lx = left_x;
  result.thumb_ly = left_y;

  int16_t right_x = 0;
  int16_t right_y = 0;
  ApplyRadialDeadzone(static_cast<int16_t>(source.thumb_rx),
                      static_cast<int16_t>(source.thumb_ry),
                      requested.aim_deadzone, &right_x, &right_y);
  const float sensitivity = static_cast<float>(
      std::isfinite(requested.look_sensitivity)
          ? std::clamp(requested.look_sensitivity,
                       kControllerLookSensitivityMin,
                       kControllerLookSensitivityMax)
          : 1.0);
  result.thumb_rx = UnitToAxis(AxisToUnit(right_x) * sensitivity);
  const float tuned_y = AxisToUnit(right_y) * sensitivity *
                        (requested.invert_y ? -1.0f : 1.0f);
  result.thumb_ry = UnitToAxis(tuned_y);
  return result;
}

inline uint16_t ScaleRumble(uint16_t value, double requested_intensity) {
  const double intensity = std::isfinite(requested_intensity)
                               ? std::clamp(requested_intensity,
                                            kControllerRumbleIntensityMin,
                                            kControllerRumbleIntensityMax)
                               : 1.0;
  return static_cast<uint16_t>(
      std::lround(static_cast<double>(value) * intensity));
}

}  // namespace controller
}  // namespace rex::input
