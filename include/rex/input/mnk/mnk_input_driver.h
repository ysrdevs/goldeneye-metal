/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;
  void OnWindowUnavailable() override;
  void OnInputActiveChanged(bool active) override;
  void SetMouseMotionMode(MouseMotionMode mode) override;
  bool ConsumeApplicationMouseMotion(uint32_t user_index,
                                     MouseMotionDelta* out_delta) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

 private:
  uint32_t UserIndex() const;
  bool IsEnabled() const;
  bool IsMouseEnabled() const;
  void CenterCursor(rex::ui::Window* window);
  void UpdateMouseCapture(bool input_active, uint64_t observed_activity_generation);
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);

  std::atomic<rex::ui::Window*> attached_window_{nullptr};
  std::atomic<bool> closing_{false};
  std::atomic<uint64_t> capture_request_generation_{0};
  std::atomic<bool> host_input_active_{true};
  std::atomic<uint64_t> input_activity_generation_{0};

  std::mutex state_mutex_;
  bool key_down_[256] = {};

  // Mouse delta tracking
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  MouseMotionMode mouse_motion_mode_ = MouseMotionMode::kRightStick;
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  // Read and written only on the window UI thread. All common Window capture
  // state is changed there too, so it stays serialized with ImGui capture.
  bool mouse_captured_ = false;
  std::atomic<bool> mouse_capture_applied_{false};
  std::atomic<bool> has_focus_{true};

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
