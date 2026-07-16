#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>

using rex::X_RESULT;
using rex::X_STATUS;

namespace {

class KeystrokeDriver final : public rex::input::InputDriver {
 public:
  KeystrokeDriver(rex::X_RESULT result, uint16_t key = 0)
      : InputDriver(nullptr, 0), result_(result), key_(key) {}

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

 private:
  rex::X_RESULT result_;
  uint16_t key_;
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
