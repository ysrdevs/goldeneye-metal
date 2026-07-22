#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <utility>

using rex::X_RESULT;
using rex::X_STATUS;

namespace {

class KeystrokeDriver final : public rex::input::InputDriver {
 public:
  KeystrokeDriver(rex::X_RESULT result, uint16_t key = 0,
                  rex::input::MouseMotionMode* observed_mouse_mode = nullptr)
      : InputDriver(nullptr, 0),
        result_(result),
        key_(key),
        observed_mouse_mode_(observed_mouse_mode) {}

  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  rex::X_RESULT GetCapabilities(uint32_t, uint32_t, rex::input::X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT GetState(uint32_t, rex::input::X_INPUT_STATE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT SetState(uint32_t, rex::input::X_INPUT_VIBRATION*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT GetKeystroke(uint32_t, uint32_t,
                             rex::input::X_INPUT_KEYSTROKE* keystroke) override {
    if (result_ == X_ERROR_SUCCESS && keystroke) {
      std::memset(keystroke, 0, sizeof(*keystroke));
      keystroke->virtual_key = key_;
    }
    return result_;
  }

  void SetMouseMotionMode(rex::input::MouseMotionMode mode) override {
    if (observed_mouse_mode_) {
      *observed_mouse_mode_ = mode;
    }
  }

  void SetApplicationMouseMotion(rex::input::MouseMotionDelta delta) {
    application_mouse_owned_ = true;
    application_mouse_delta_ = delta;
  }

  void SetControllerSnapshot(rex::input::ControllerSnapshot snapshot) {
    controller_snapshot_ = std::move(snapshot);
    has_controller_snapshot_ = true;
  }

  bool GetControllerSnapshot(
      uint32_t user_index,
      rex::input::ControllerSnapshot* out_snapshot) override {
    if (!has_controller_snapshot_ || user_index != controller_snapshot_.user_index) {
      if (out_snapshot) {
        *out_snapshot = {};
        out_snapshot->user_index = user_index;
      }
      return false;
    }
    if (out_snapshot) {
      *out_snapshot = controller_snapshot_;
    }
    return true;
  }

  void SetTestRumbleResult(rex::X_RESULT result) {
    test_rumble_result_ = result;
  }

  rex::X_RESULT PlayControllerTestRumble(uint32_t) override {
    return test_rumble_result_;
  }

  bool ConsumeApplicationMouseMotion(uint32_t user_index,
                                     rex::input::MouseMotionDelta* out_delta) override {
    if (!application_mouse_owned_ || user_index != 0) {
      if (out_delta) {
        *out_delta = {};
      }
      return false;
    }
    if (out_delta) {
      *out_delta = application_mouse_delta_;
    }
    application_mouse_delta_ = {};
    return true;
  }

 private:
  rex::X_RESULT result_;
  uint16_t key_;
  rex::input::MouseMotionMode* observed_mouse_mode_;
  bool application_mouse_owned_ = false;
  rex::input::MouseMotionDelta application_mouse_delta_ = {};
  bool has_controller_snapshot_ = false;
  rex::input::ControllerSnapshot controller_snapshot_ = {};
  rex::X_RESULT test_rumble_result_ = X_ERROR_DEVICE_NOT_CONNECTED;
};

}  // namespace

TEST_CASE("Input system checks MnK keystrokes after an idle controller driver", "[input]") {
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY));
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_SUCCESS, 0x5810));

  rex::input::X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(input.GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
  CHECK(keystroke.virtual_key == 0x5810);
}

TEST_CASE("Input system forwards application mouse mode to every driver", "[input][mouse]") {
  using rex::input::MouseMotionMode;
  MouseMotionMode first = MouseMotionMode::kRightStick;
  MouseMotionMode second = MouseMotionMode::kRightStick;

  rex::input::InputSystem input(nullptr);
  input.AddDriver(
      std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY, 0, &first));
  input.AddDriver(
      std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY, 0, &second));
  input.SetMouseMotionMode(MouseMotionMode::kApplication);

  CHECK(first == MouseMotionMode::kApplication);
  CHECK(second == MouseMotionMode::kApplication);
}

TEST_CASE("Input system consumes and combines paired application mouse motion", "[input][mouse]") {
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY));

  auto first = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  first->SetApplicationMouseMotion({4, -3});
  input.AddDriver(std::move(first));

  auto second = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  second->SetApplicationMouseMotion({-1, 5});
  input.AddDriver(std::move(second));

  rex::input::MouseMotionDelta delta{99, 99};
  REQUIRE(input.ConsumeApplicationMouseMotion(0, &delta));
  CHECK(delta.x == 3);
  CHECK(delta.y == 2);

  // Drivers still own application input on an idle frame, but the sample is
  // consumed exactly once and the output is always initialized.
  REQUIRE(input.ConsumeApplicationMouseMotion(0, &delta));
  CHECK(delta.x == 0);
  CHECK(delta.y == 0);

  delta = {99, 99};
  CHECK_FALSE(input.ConsumeApplicationMouseMotion(1, &delta));
  CHECK(delta.x == 0);
  CHECK(delta.y == 0);
}

TEST_CASE("Input system exposes the first physical controller snapshot",
          "[input][controller]") {
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY));

  auto controller = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  rex::input::ControllerSnapshot expected;
  expected.connected = true;
  expected.user_index = 0;
  expected.name = "Test pad";
  expected.raw_gamepad.thumb_lx = 1234;
  expected.gamepad.thumb_lx = 1000;
  controller->SetControllerSnapshot(expected);
  input.AddDriver(std::move(controller));

  rex::input::ControllerSnapshot actual;
  REQUIRE(input.GetControllerSnapshot(0, &actual));
  CHECK(actual.connected);
  CHECK(actual.name == "Test pad");
  CHECK(actual.raw_gamepad.thumb_lx == 1234);
  CHECK(actual.gamepad.thumb_lx == 1000);

  actual.name = "stale";
  CHECK_FALSE(input.GetControllerSnapshot(1, &actual));
  CHECK_FALSE(actual.connected);
  CHECK(actual.user_index == 1);
  CHECK(actual.name.empty());
}

TEST_CASE("Input system routes host rumble tests to a connected driver",
          "[input][controller]") {
  rex::input::InputSystem input(nullptr);
  auto disconnected = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  disconnected->SetTestRumbleResult(X_ERROR_DEVICE_NOT_CONNECTED);
  input.AddDriver(std::move(disconnected));
  auto connected = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  connected->SetTestRumbleResult(X_ERROR_SUCCESS);
  input.AddDriver(std::move(connected));
  REQUIRE(input.PlayControllerTestRumble(0) == X_ERROR_SUCCESS);
}
