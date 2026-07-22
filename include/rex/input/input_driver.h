#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstddef>
#include <functional>

#include <rex/input/controller.h>
#include <rex/kernel.h>
#include <rex/ui/window.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem;

class InputDriver {
 public:
  virtual ~InputDriver() = default;

  virtual X_STATUS Setup() = 0;

  virtual X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                                   X_INPUT_CAPABILITIES* out_caps) = 0;
  virtual X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) = 0;
  virtual X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) = 0;
  virtual X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                                X_INPUT_KEYSTROKE* out_keystroke) = 0;

  // Raw and tuned physical-controller state for host settings and diagnostics.
  // This is deliberately separate from GetState so host UI remains usable
  // while guest input is suppressed by a modal overlay.
  virtual bool GetControllerSnapshot(uint32_t /*user_index*/,
                                     ControllerSnapshot* out_snapshot) {
    if (out_snapshot) {
      *out_snapshot = {};
    }
    return false;
  }

  // A short host-initiated pulse used by controller setup UIs. Unlike guest
  // SetState, it is permitted while modal host UI has disabled guest input.
  virtual X_RESULT PlayControllerTestRumble(uint32_t /*user_index*/) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  virtual void OnWindowAvailable(rex::ui::Window* /*window*/) {}
  virtual void OnWindowUnavailable() {}

  // Called when a host UI changes whether input should reach the guest. Most
  // drivers only need the polling callback below; capture-based drivers can
  // use this notification to release platform state immediately.
  virtual void OnInputActiveChanged(bool /*active*/) {}

  // Select whether mouse motion should emulate the guest right stick or be
  // handled directly by the application. Keyboard and mouse buttons are
  // unaffected.
  virtual void SetMouseMotionMode(MouseMotionMode /*mode*/) {}

  // Consume one paired sample when application mouse mode is active. Returns
  // true while this driver owns application mouse input, including frames with
  // no movement, so title hooks may still update their idle camera state.
  virtual bool ConsumeApplicationMouseMotion(uint32_t /*user_index*/,
                                             MouseMotionDelta* out_delta) {
    if (out_delta) {
      *out_delta = {};
    }
    return false;
  }

  void set_is_active_callback(std::function<bool()> is_active_callback) {
    is_active_callback_ = is_active_callback;
  }

 protected:
  explicit InputDriver(rex::ui::Window* window, size_t window_z_order)
      : window_(window), window_z_order_(window_z_order) {}

  rex::ui::Window* window() const { return window_; }
  size_t window_z_order() const { return window_z_order_; }

  bool is_active() const { return !is_active_callback_ || is_active_callback_(); }

 private:
  rex::ui::Window* window_;
  size_t window_z_order_;
  std::function<bool()> is_active_callback_ = nullptr;
};

}  // namespace rex::input
