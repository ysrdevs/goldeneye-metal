/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#if REX_PLATFORM_WIN32
#include <rex/ui/window_win.h>
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_BOOL(mnk_mouse_enabled, true, "Input", "Enable mouse controller input and capture");
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK").range(0, 3);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);

REXCVAR_DEFINE_STRING(keybind_a, "Space", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "Shift", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "R", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "E", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "RMB", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "LMB", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "F", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "MMB", "Input/Keybinds/Controller",
                      "Right stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_up, "", "Input/Keybinds/Controller", "Right stick up");
REXCVAR_DEFINE_STRING(keybind_rstick_down, "", "Input/Keybinds/Controller", "Right stick down");
REXCVAR_DEFINE_STRING(keybind_rstick_left, "", "Input/Keybinds/Controller", "Right stick left");
REXCVAR_DEFINE_STRING(keybind_rstick_right, "", "Input/Keybinds/Controller", "Right stick right");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Right", "Input/Keybinds/Controller", "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button");
#if REX_PLATFORM_MAC
static constexpr char kDefaultStartBinding[] = "Return";
#else
static constexpr char kDefaultStartBinding[] = "Escape";
#endif
REXCVAR_DEFINE_STRING(keybind_start, kDefaultStartBinding, "Input/Keybinds/Controller",
                      "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // InputSystem detaches drivers while their Window is still alive.
  assert_null(attached_window_.load(std::memory_order_acquire));
}

void MnkInputDriver::OnWindowUnavailable() {
  closing_.store(true, std::memory_order_release);
  capture_request_generation_.fetch_add(1, std::memory_order_acq_rel);
  if (auto* window = attached_window_.exchange(nullptr, std::memory_order_acq_rel)) {
    // Window lifecycle callbacks and ReXApp teardown run on the UI thread, the
    // same thread that owns mouse_captured_ and common Window capture state.
    if (mouse_captured_) {
      mouse_captured_ = false;
      mouse_capture_applied_.store(false, std::memory_order_release);
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      window->ReleaseMouse();
    }
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  }
}

X_STATUS MnkInputDriver::Setup() {
  REXLOG_INFO("MnK input driver initialized");
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    closing_.store(false, std::memory_order_release);
    attached_window_.store(window, std::memory_order_release);
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  OnWindowUnavailable();
}

void MnkInputDriver::OnInputActiveChanged(bool active) {
  host_input_active_.store(active, std::memory_order_release);
  const uint64_t activity_generation =
      input_activity_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (!active) {
    std::lock_guard lock(state_mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  }
  // The passed bit is the app-specific modal state. Reconcile against the full
  // callback too (focus and other host overlays) without latching those
  // transient conditions into host_input_active_.
  UpdateMouseCapture(is_active(), activity_generation);
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

bool MnkInputDriver::IsMouseEnabled() const {
  return REXCVAR_GET(mnk_mouse_enabled);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  std::string_view remaining = cvar_val;
  while (!remaining.empty()) {
    const size_t comma = remaining.find(',');
    std::string_view name = remaining.substr(0, comma);
    const size_t first = name.find_first_not_of(" \t");
    if (first != std::string_view::npos) {
      const size_t last = name.find_last_not_of(" \t");
      name = name.substr(first, last - first + 1);
      VirtualKey vk = rex::ui::ParseVirtualKey(name);
      uint16_t idx = static_cast<uint16_t>(vk);
      if (vk != VirtualKey::kNone && idx < 256 && key_down[idx]) {
        return true;
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(comma + 1);
  }
  return false;
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const uint64_t activity_generation = input_activity_generation_.load(std::memory_order_acquire);
  if (!IsEnabled()) {
    UpdateMouseCapture(false, activity_generation);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const bool callback_active = is_active();
  const bool input_active = callback_active && host_input_active_.load(std::memory_order_acquire);
  UpdateMouseCapture(callback_active, activity_generation);

  std::lock_guard lock(state_mutex_);
  if (!input_active || !has_focus_.load(std::memory_order_acquire)) {
    std::memset(key_down_, 0, sizeof(key_down_));
    mouse_dx_ = 0;
    mouse_dy_ = 0;
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  const bool mouse_enabled = IsMouseEnabled();
  if (!mouse_enabled) {
    SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
    SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
    SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  }

  uint16_t buttons = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed(key_down_, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
  uint8_t rt = IsBindPressed(key_down_, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_left)))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_right)))
    lx += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_up)))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_down)))
    ly -= INT16_MAX;

  double sensitivity = REXCVAR_GET(mnk_sensitivity);
  constexpr double kBaseScale = 200.0;
  int32_t rx = mouse_enabled ? static_cast<int32_t>(mouse_dx_ * sensitivity * kBaseScale) : 0;
  int32_t ry = mouse_enabled ? static_cast<int32_t>(-mouse_dy_ * sensitivity * kBaseScale) : 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_left)))
    rx = -INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_right)))
    rx = INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_up)))
    ry = INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_down)))
    ry = -INT16_MAX;
  mouse_dx_ = 0;
  mouse_dy_ = 0;

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    // A host menu notification may have arrived while this guest poll was in
    // flight. Never deliver the state sampled under the older activity epoch.
    if (activity_generation != input_activity_generation_.load(std::memory_order_acquire) ||
        !host_input_active_.load(std::memory_order_acquire)) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
      return X_ERROR_SUCCESS;
    }
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::CenterCursor(rex::ui::Window* window) {
  int32_t cx = static_cast<int32_t>(window->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(window->GetActualLogicalHeight() / 2);
  {
    std::lock_guard lock(state_mutex_);
    prev_mouse_x_ = cx;
    prev_mouse_y_ = cy;
  }
#if REX_PLATFORM_WIN32
  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(window);
  if (win32_window && win32_window->hwnd()) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(win32_window->hwnd(), &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::UpdateMouseCapture(bool input_active, uint64_t observed_activity_generation) {
  auto* window = attached_window_.load(std::memory_order_acquire);
  if (!window || closing_.load(std::memory_order_acquire) ||
      observed_activity_generation != input_activity_generation_.load(std::memory_order_acquire)) {
    return;
  }

  const uint64_t generation =
      capture_request_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  const bool should_capture = IsEnabled() && IsMouseEnabled() &&
                              has_focus_.load(std::memory_order_acquire) && input_active &&
                              host_input_active_.load(std::memory_order_acquire);
  // Common Window state needs a UI-thread transition only when the desired
  // capture state changes. Still increment the request generation above to
  // cancel any older queued transition.
  if (should_capture == mouse_capture_applied_.load(std::memory_order_acquire)) {
#if REX_PLATFORM_WIN32
    if (should_capture) {
      CenterCursor(window);
    }
#endif
    return;
  }

  window->app_context().CallInUIThreadSynchronous(
      [this, window, input_active, generation, observed_activity_generation] {
        // Capture request counts and cursor visibility belong to Window and are not
        // atomic. Recheck after marshaling, then mutate all of them on the UI thread
        // so guest polls cannot race ImGui or close/focus transitions.
        if (generation != capture_request_generation_.load(std::memory_order_acquire) ||
            observed_activity_generation !=
                input_activity_generation_.load(std::memory_order_acquire) ||
            closing_.load(std::memory_order_acquire) ||
            attached_window_.load(std::memory_order_acquire) != window) {
          return;
        }

        bool should_capture = IsEnabled() && IsMouseEnabled() &&
                              has_focus_.load(std::memory_order_acquire) && input_active &&
                              host_input_active_.load(std::memory_order_acquire);
        if (should_capture && !mouse_captured_) {
          {
            std::lock_guard lock(state_mutex_);
            // Reset deltas to avoid a spike on capture start.
            mouse_dx_ = 0;
            mouse_dy_ = 0;
          }
          window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
          window->CaptureMouse();
          mouse_captured_ = true;
          mouse_capture_applied_.store(true, std::memory_order_release);
        } else if (!should_capture && mouse_captured_) {
          mouse_captured_ = false;
          mouse_capture_applied_.store(false, std::memory_order_release);
          window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
          window->ReleaseMouse();
        }

#if REX_PLATFORM_WIN32
        // Re-center cursor each frame while captured to prevent edge clamping.
        if (mouse_captured_) {
          CenterCursor(window);
        }
#endif
      });
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  if (!has_focus_.load(std::memory_order_acquire) ||
      !host_input_active_.load(std::memory_order_acquire) || !is_active())
    return;
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, true);
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !IsMouseEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  if (!has_focus_.load(std::memory_order_acquire) ||
      !host_input_active_.load(std::memory_order_acquire) || !is_active())
    return;
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !IsMouseEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  if (!has_focus_.load(std::memory_order_acquire) ||
      !host_input_active_.load(std::memory_order_acquire) || !is_active())
    return;
  int32_t x = e.x();
  int32_t y = e.y();
  if (e.has_movement_delta()) {
    mouse_dx_ += e.movement_x();
    mouse_dy_ += e.movement_y();
  } else {
    mouse_dx_ += x - prev_mouse_x_;
    mouse_dy_ += y - prev_mouse_y_;
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_.store(false, std::memory_order_release);
  const uint64_t activity_generation =
      input_activity_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  {
    std::lock_guard lock(state_mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  }
  UpdateMouseCapture(false, activity_generation);
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  has_focus_.store(true, std::memory_order_release);
  const uint64_t activity_generation =
      input_activity_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  UpdateMouseCapture(is_active(), activity_generation);
}

}  // namespace rex::input::mnk
