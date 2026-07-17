#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/ui/ui_event.h>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_DPAD_UP;
using rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_Y;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

constexpr uint16_t kVirtualVendor = 0xCAFE;
constexpr uint16_t kVirtualProduct = 0x0001;

class TestAppContext final : public rex::ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class TestWindow final : public rex::ui::Window {
 public:
  explicit TestWindow(TestAppContext& context) : Window(context, "SDL input test", 1280, 720) {}
  ~TestWindow() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override {
    WindowDestructionReceiver receiver(this);
    OnActualSizeUpdate(1280, 720, receiver);
    if (!receiver.IsWindowDestroyed()) {
      OnFocusUpdate(true, receiver);
    }
    return !receiver.IsWindowDestroyed();
  }

  void RequestCloseImpl() override {
    WindowDestructionReceiver receiver(this);
    OnBeforeClose(receiver);
    if (!receiver.IsWindowDestroyed()) {
      OnAfterClose();
    }
  }

  void ApplyNewMouseCapture() override {}
  void ApplyNewMouseRelease() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};

struct RumbleRecord {
  bool accept = true;
  uint32_t calls = 0;
  uint16_t left = 0;
  uint16_t right = 0;
};

bool SDLCALL RecordRumble(void* userdata, uint16_t left, uint16_t right) {
  auto* record = static_cast<RumbleRecord*>(userdata);
  ++record->calls;
  record->left = left;
  record->right = right;
  return record->accept;
}

class VirtualGamepad {
 public:
  explicit VirtualGamepad(RumbleRecord* rumble = nullptr) {
    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.vendor_id = kVirtualVendor;
    description.product_id = kVirtualProduct;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name = "GoldenEye virtual gamepad";
    description.userdata = rumble;
    description.Rumble = rumble ? RecordRumble : nullptr;

    instance_id_ = SDL_AttachVirtualJoystick(&description);
    if (!instance_id_) {
      throw std::runtime_error(std::string("SDL_AttachVirtualJoystick failed: ") + SDL_GetError());
    }
    joystick_ = SDL_OpenJoystick(instance_id_);
    if (!joystick_) {
      SDL_DetachVirtualJoystick(instance_id_);
      instance_id_ = 0;
      throw std::runtime_error(std::string("SDL_OpenJoystick failed: ") + SDL_GetError());
    }
  }

  ~VirtualGamepad() { Detach(); }

  VirtualGamepad(const VirtualGamepad&) = delete;
  VirtualGamepad& operator=(const VirtualGamepad&) = delete;

  void SetButton(SDL_GamepadButton button, bool down) {
    if (!SDL_SetJoystickVirtualButton(joystick_, static_cast<int>(button), down)) {
      throw std::runtime_error(std::string("SDL_SetJoystickVirtualButton failed: ") +
                               SDL_GetError());
    }
  }

  void SetAxis(SDL_GamepadAxis axis, int16_t value) {
    if (!SDL_SetJoystickVirtualAxis(joystick_, static_cast<int>(axis), value)) {
      throw std::runtime_error(std::string("SDL_SetJoystickVirtualAxis failed: ") + SDL_GetError());
    }
  }

  void Commit() { SDL_UpdateJoysticks(); }

  void Detach() {
    if (joystick_) {
      SDL_CloseJoystick(joystick_);
      joystick_ = nullptr;
    }
    if (instance_id_) {
      SDL_DetachVirtualJoystick(instance_id_);
      instance_id_ = 0;
    }
  }

 private:
  SDL_JoystickID instance_id_ = 0;
  SDL_Joystick* joystick_ = nullptr;
};

class ScopedSDLGamepadSubsystem {
 public:
  ScopedSDLGamepadSubsystem() {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
      throw std::runtime_error(std::string("SDL gamepad initialization failed: ") + SDL_GetError());
    }
  }
  ~ScopedSDLGamepadSubsystem() { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }
};

class SDLDriverFixture {
 public:
  SDLDriverFixture() : window(context), driver(nullptr, 0) {
    if (!SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xCAFE/0x0001")) {
      throw std::runtime_error("Could not isolate SDL virtual gamepads");
    }
    if (!window.Open()) {
      throw std::runtime_error("Could not open the SDL input test window");
    }
    if (driver.Setup() != X_STATUS_SUCCESS) {
      throw std::runtime_error("Could not set up the SDL input driver");
    }
    driver.OnWindowAvailable(&window);
  }

  ~SDLDriverFixture() {
    driver.OnWindowUnavailable();
    SDL_ResetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
  }

  X_RESULT Poll(uint32_t user_index, X_INPUT_STATE& state) {
    for (int attempt = 0; attempt < 8; ++attempt) {
      SDL_PumpEvents();
      context.ExecutePendingFunctionsFromUIThread();
      const X_RESULT result = driver.GetState(user_index, &state);
      if (result == X_ERROR_SUCCESS) {
        return result;
      }
    }
    return driver.GetState(user_index, &state);
  }

  X_RESULT PollDisconnected(uint32_t user_index, X_INPUT_STATE& state) {
    X_RESULT result = X_ERROR_SUCCESS;
    for (int attempt = 0; attempt < 8; ++attempt) {
      SDL_PumpEvents();
      context.ExecutePendingFunctionsFromUIThread();
      result = driver.GetState(user_index, &state);
      if (result == X_ERROR_DEVICE_NOT_CONNECTED) {
        break;
      }
    }
    return result;
  }

  TestAppContext context;
  TestWindow window;
  rex::input::sdl::SDLInputDriver driver;
};

}  // namespace

TEST_CASE("SDL virtual gamepad maps modern controller controls and activity", "[input][sdl]") {
  SDLDriverFixture fixture;
  bool active = true;
  fixture.driver.set_is_active_callback([&active] { return active; });

  VirtualGamepad gamepad;
  X_INPUT_STATE state = {};
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);

  gamepad.SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFTX, 12345);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFTY, -20000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTX, -16000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTY, 7000);
  // Virtual joystick triggers use the raw -32768..32767 range; zero maps to
  // the midpoint of SDL's normalized 0..32767 gamepad-trigger range.
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
  gamepad.Commit();

  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_DPAD_UP) != 0);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
  CHECK(state.gamepad.thumb_lx == 12345);
  CHECK(state.gamepad.thumb_ly == 19999);
  CHECK(state.gamepad.thumb_rx == -16000);
  CHECK(state.gamepad.thumb_ry == -7001);
  CHECK(state.gamepad.left_trigger == 127);
  CHECK(state.gamepad.right_trigger == 255);

  gamepad.SetButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
  gamepad.Commit();
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) == 0);

  active = false;
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == 0);
  CHECK(state.gamepad.thumb_lx == 0);
  CHECK(state.gamepad.left_trigger == 0);

  active = true;
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
  CHECK(state.gamepad.thumb_lx == 12345);
}

TEST_CASE("SDL gamepad discovery includes a controller connected before driver startup",
          "[input][sdl]") {
  REQUIRE(SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xCAFE/0x0001"));
  ScopedSDLGamepadSubsystem subsystem;
  VirtualGamepad gamepad;
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepad.Commit();

  SDLDriverFixture fixture;
  X_INPUT_STATE state = {};
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
}

TEST_CASE("SDL gamepad hotplug promotes waiting devices into guest slots", "[input][sdl]") {
  SDLDriverFixture fixture;
  std::array<RumbleRecord, 5> rumble_records = {};
  std::vector<std::unique_ptr<VirtualGamepad>> gamepads;
  gamepads.reserve(rumble_records.size());
  for (auto& record : rumble_records) {
    gamepads.push_back(std::make_unique<VirtualGamepad>(&record));
  }

  gamepads.back()->SetButton(SDL_GAMEPAD_BUTTON_NORTH, true);
  gamepads.back()->Commit();

  X_INPUT_STATE state = {};
  for (uint32_t user = 0; user < 4; ++user) {
    REQUIRE(fixture.Poll(user, state) == X_ERROR_SUCCESS);
  }

  // The fifth controller is connected but initially has no guest slot. Once
  // player 1 disconnects, the remaining slots compact and it fills slot 4.
  gamepads.front()->Detach();
  REQUIRE(fixture.Poll(3, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_Y) != 0);

  for (auto& gamepad : gamepads) {
    gamepad->Detach();
  }
  REQUIRE(fixture.PollDisconnected(0, state) == X_ERROR_DEVICE_NOT_CONNECTED);

  // Reattaching the same driver must clear slot and keystroke state.
  fixture.driver.OnWindowUnavailable();
  fixture.driver.OnWindowAvailable(&fixture.window);

  VirtualGamepad replacement;
  replacement.SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  replacement.Commit();
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
}

TEST_CASE("SDL gamepad rumble follows XInput state and stops when inactive", "[input][sdl]") {
  SDLDriverFixture fixture;
  bool active = true;
  fixture.driver.set_is_active_callback([&active] { return active; });
  RumbleRecord rumble;
  VirtualGamepad gamepad(&rumble);

  X_INPUT_STATE state = {};
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);

  X_INPUT_VIBRATION vibration = {};
  vibration.left_motor_speed = 0x1234;
  vibration.right_motor_speed = 0xABCD;
  REQUIRE(fixture.driver.SetState(0, &vibration) == X_ERROR_SUCCESS);
  CHECK(rumble.calls >= 1);
  CHECK(rumble.left == 0x1234);
  CHECK(rumble.right == 0xABCD);

  active = false;
  fixture.driver.OnInputActiveChanged(active);
  CHECK(rumble.left == 0);
  CHECK(rumble.right == 0);

  REQUIRE(fixture.driver.SetState(0, &vibration) == X_ERROR_SUCCESS);
  CHECK(rumble.left == 0);
  CHECK(rumble.right == 0);

  active = true;
  rumble.accept = false;
  REQUIRE(fixture.driver.SetState(0, &vibration) == X_ERROR_FUNCTION_FAILED);
}
