#include <rex/cvar.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
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

REXCVAR_DECLARE(double, controller_look_sensitivity);
REXCVAR_DECLARE(double, controller_move_deadzone);
REXCVAR_DECLARE(double, controller_aim_deadzone);
REXCVAR_DECLARE(bool, controller_invert_y);
REXCVAR_DECLARE(bool, controller_rumble_enabled);
REXCVAR_DECLARE(double, controller_rumble_intensity);
REXCVAR_DECLARE(std::string, controller_layout);
REXCVAR_DECLARE(std::string, controller_button_map);

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

  bool Snapshot(uint32_t user_index,
                rex::input::ControllerSnapshot& snapshot) {
    for (int attempt = 0; attempt < 8; ++attempt) {
      SDL_PumpEvents();
      context.ExecutePendingFunctionsFromUIThread();
      if (driver.GetControllerSnapshot(user_index, &snapshot)) {
        return true;
      }
    }
    return driver.GetControllerSnapshot(user_index, &snapshot);
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

TEST_CASE("SDL controller snapshot remains live while guest input is suppressed",
          "[input][sdl][controller]") {
  const double old_sensitivity = REXCVAR_GET(controller_look_sensitivity);
  const double old_move_deadzone = REXCVAR_GET(controller_move_deadzone);
  const double old_aim_deadzone = REXCVAR_GET(controller_aim_deadzone);
  const bool old_invert = REXCVAR_GET(controller_invert_y);
  struct RestoreCvars {
    double sensitivity;
    double move_deadzone;
    double aim_deadzone;
    bool invert;
    ~RestoreCvars() {
      REXCVAR_SET(controller_look_sensitivity, sensitivity);
      REXCVAR_SET(controller_move_deadzone, move_deadzone);
      REXCVAR_SET(controller_aim_deadzone, aim_deadzone);
      REXCVAR_SET(controller_invert_y, invert);
    }
  } restore{old_sensitivity, old_move_deadzone, old_aim_deadzone, old_invert};

  REXCVAR_SET(controller_look_sensitivity, 2.0);
  REXCVAR_SET(controller_move_deadzone, 0.15);
  REXCVAR_SET(controller_aim_deadzone, 0.0);
  REXCVAR_SET(controller_invert_y, true);

  SDLDriverFixture fixture;
  bool active = false;
  fixture.driver.set_is_active_callback([&active] { return active; });
  VirtualGamepad gamepad;
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFTX, 3000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTX, 12000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTY, 8000);
  gamepad.Commit();

  rex::input::ControllerSnapshot snapshot;
  REQUIRE(fixture.Snapshot(0, snapshot));
  CHECK(snapshot.connected);
  CHECK_FALSE(snapshot.input_active);
  CHECK(snapshot.name == "GoldenEye virtual gamepad");
  CHECK((snapshot.raw_gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
  CHECK(snapshot.raw_gamepad.thumb_lx == 3000);
  CHECK(snapshot.gamepad.thumb_lx == 0);
  CHECK(snapshot.gamepad.thumb_rx == 24000);
  CHECK(snapshot.gamepad.thumb_ry > 0);

  X_INPUT_STATE guest_state = {};
  REQUIRE(fixture.Poll(0, guest_state) == X_ERROR_SUCCESS);
  CHECK(guest_state.gamepad.buttons == 0);
  CHECK(guest_state.gamepad.thumb_rx == 0);
}

TEST_CASE("SDL host rumble test respects enable and intensity settings",
          "[input][sdl][controller]") {
  const bool old_enabled = REXCVAR_GET(controller_rumble_enabled);
  const double old_intensity = REXCVAR_GET(controller_rumble_intensity);
  struct RestoreRumbleCvars {
    bool enabled;
    double intensity;
    ~RestoreRumbleCvars() {
      REXCVAR_SET(controller_rumble_enabled, enabled);
      REXCVAR_SET(controller_rumble_intensity, intensity);
    }
  } restore{old_enabled, old_intensity};

  REXCVAR_SET(controller_rumble_enabled, true);
  REXCVAR_SET(controller_rumble_intensity, 0.5);
  SDLDriverFixture fixture;
  RumbleRecord rumble;
  VirtualGamepad gamepad(&rumble);
  rex::input::ControllerSnapshot snapshot;
  REQUIRE(fixture.Snapshot(0, snapshot));
  REQUIRE(snapshot.rumble_supported);

  REQUIRE(fixture.driver.PlayControllerTestRumble(0) == X_ERROR_SUCCESS);
  CHECK(rumble.left == 0x4800);
  CHECK(rumble.right == 0x4800);

  REXCVAR_SET(controller_rumble_enabled, false);
  REQUIRE(fixture.driver.PlayControllerTestRumble(0) ==
          X_ERROR_FUNCTION_FAILED);
}

TEST_CASE("Controller tuning uses independent radial deadzones and safe inversion",
          "[input][sdl][controller]") {
  rex::input::X_INPUT_GAMEPAD source = {};
  source.thumb_lx = 3000;
  source.thumb_ly = 0;
  source.thumb_rx = 12000;
  source.thumb_ry = static_cast<int16_t>(-32768);

  rex::input::ControllerTuning tuning;
  tuning.move_deadzone = 0.15;
  tuning.aim_deadzone = 0.0;
  tuning.look_sensitivity = 2.0;
  tuning.invert_y = true;
  const auto tuned = rex::input::controller::ApplyTuning(source, tuning);

  CHECK(tuned.thumb_lx == 0);
  CHECK(tuned.thumb_ly == 0);
  CHECK(tuned.thumb_rx == 24000);
  // Inverting -32768 must saturate to +32767, not overflow back to -32768.
  CHECK(tuned.thumb_ry == 32767);
}

TEST_CASE("Controller radial deadzone remaps its remaining range", "[input][sdl][controller]") {
  int16_t x = 0;
  int16_t y = 0;
  rex::input::controller::ApplyRadialDeadzone(16384, 0, 0.25, &x, &y);
  CHECK(x == 10923);
  CHECK(y == 0);

  rex::input::controller::ApplyRadialDeadzone(32767, 32767, 0.25, &x, &y);
  CHECK(x == 23170);
  CHECK(y == 23170);
}

TEST_CASE("Controller rumble intensity scales both motors", "[input][sdl][controller]") {
  CHECK(rex::input::controller::ScaleRumble(0xFFFF, 0.0) == 0);
  CHECK(rex::input::controller::ScaleRumble(0xFFFF, 0.5) == 0x8000);
  CHECK(rex::input::controller::ScaleRumble(0xFFFF, 1.0) == 0xFFFF);
  CHECK(rex::input::controller::ScaleRumble(
            0xFFFF, std::numeric_limits<double>::quiet_NaN()) == 0xFFFF);
}

TEST_CASE("Controller layout presets route axes and southpaw stick clicks",
          "[input][sdl][controller][mapping]") {
  using rex::input::controller::ApplyMapping;
  using rex::input::controller::ButtonBindings;
  using rex::input::controller::Layout;

  rex::input::X_INPUT_GAMEPAD source = {};
  source.thumb_lx = 1111;
  source.thumb_ly = 2222;
  source.thumb_rx = 3333;
  source.thumb_ry = 4444;
  source.buttons = rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
  const ButtonBindings bindings;

  const auto modern = ApplyMapping(source, Layout::kModern, bindings);
  CHECK(modern.thumb_lx == 1111);
  CHECK(modern.thumb_ly == 2222);
  CHECK(modern.thumb_rx == 3333);
  CHECK(modern.thumb_ry == 4444);

  const auto classic = ApplyMapping(source, Layout::kClassic, bindings);
  CHECK(classic.thumb_lx == 3333);
  CHECK(classic.thumb_ly == 2222);
  CHECK(classic.thumb_rx == 1111);
  CHECK(classic.thumb_ry == 4444);

  const auto southpaw = ApplyMapping(source, Layout::kSouthpaw, bindings);
  CHECK(southpaw.thumb_lx == 3333);
  CHECK(southpaw.thumb_ly == 4444);
  CHECK(southpaw.thumb_rx == 1111);
  CHECK(southpaw.thumb_ry == 2222);
  CHECK((southpaw.buttons & rex::input::X_INPUT_GAMEPAD_LEFT_THUMB) == 0);
  CHECK((southpaw.buttons & rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB) != 0);
}

TEST_CASE("Controller button map parsing is strict and canonical",
          "[input][sdl][controller][mapping]") {
  rex::input::controller::ButtonBindings bindings;
  REQUIRE(rex::input::controller::ParseButtonBindings(
      " a=b, rt=lb, x=none ", &bindings));
  CHECK(rex::input::controller::SerializeButtonBindings(bindings) ==
        "a=b,x=none,rt=lb");

  rex::input::controller::ButtonBindings round_trip;
  REQUIRE(rex::input::controller::ParseButtonBindings(
      rex::input::controller::SerializeButtonBindings(bindings), &round_trip));
  CHECK(round_trip.sources == bindings.sources);

  CHECK_FALSE(rex::input::controller::ParseButtonBindings(
      "a=b,a=x", &round_trip));
  CHECK_FALSE(rex::input::controller::ParseButtonBindings(
      "guide=a", &round_trip));
  CHECK_FALSE(rex::input::controller::ParseButtonBindings(
      "a=guide", &round_trip));
  CHECK_FALSE(rex::input::controller::ParseButtonBindings(
      "a=b,", &round_trip));
}

TEST_CASE("Controller remapping supports buttons triggers and unbound inputs",
          "[input][sdl][controller][mapping]") {
  rex::input::controller::ButtonBindings bindings;
  REQUIRE(rex::input::controller::ParseButtonBindings(
      "a=b,b=a,x=lt,y=none,lt=rb", &bindings));

  rex::input::X_INPUT_GAMEPAD source = {};
  source.buttons = rex::input::X_INPUT_GAMEPAD_B |
                   rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER |
                   rex::input::X_INPUT_GAMEPAD_GUIDE;
  source.left_trigger = 31;
  const auto mapped = rex::input::controller::ApplyMapping(
      source, rex::input::controller::Layout::kModern, bindings);

  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_B) == 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_X) != 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_Y) == 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_GUIDE) != 0);
  CHECK(mapped.left_trigger == 255);
}

TEST_CASE("Controller UI assignment swaps conflicting physical sources",
          "[input][sdl][controller][mapping]") {
  using rex::input::controller::AssignSourceWithSwap;
  using rex::input::controller::ButtonBindings;
  using rex::input::controller::Control;
  using rex::input::controller::Layout;
  using rex::input::controller::ResolveSource;

  ButtonBindings bindings;
  REQUIRE(AssignSourceWithSwap(Layout::kModern, &bindings, Control::kA,
                               Control::kB));
  CHECK(ResolveSource(Layout::kModern, bindings, Control::kA) == Control::kB);
  CHECK(ResolveSource(Layout::kModern, bindings, Control::kB) == Control::kA);
  CHECK(rex::input::controller::SerializeButtonBindings(bindings) ==
        "a=b,b=a");

  rex::input::X_INPUT_GAMEPAD source = {};
  source.buttons = rex::input::X_INPUT_GAMEPAD_B;
  const auto mapped = rex::input::controller::ApplyMapping(
      source, Layout::kModern, bindings);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_B) == 0);
}

TEST_CASE("Controller tuning follows logical axes after layout mapping",
          "[input][sdl][controller][mapping]") {
  rex::input::X_INPUT_GAMEPAD source = {};
  source.thumb_lx = 12000;
  source.thumb_rx = 3000;
  const rex::input::controller::ButtonBindings bindings;
  const auto classic = rex::input::controller::ApplyMapping(
      source, rex::input::controller::Layout::kClassic, bindings);

  rex::input::ControllerTuning tuning;
  tuning.move_deadzone = 0.15;
  tuning.aim_deadzone = 0.0;
  tuning.look_sensitivity = 2.0;
  const auto tuned = rex::input::controller::ApplyTuning(classic, tuning);
  CHECK(tuned.thumb_lx == 0);
  CHECK(tuned.thumb_rx == 24000);
}

TEST_CASE("SDL hot-reloads mapped state consistently for state snapshot and keystroke",
          "[input][sdl][controller][mapping]") {
  const std::string old_layout = REXCVAR_GET(controller_layout);
  const std::string old_button_map = REXCVAR_GET(controller_button_map);
  struct RestoreMappingCvars {
    std::string layout;
    std::string button_map;
    ~RestoreMappingCvars() {
      REXCVAR_SET(controller_layout, layout);
      REXCVAR_SET(controller_button_map, button_map);
    }
  } restore{old_layout, old_button_map};

  REXCVAR_SET(controller_layout, std::string("modern"));
  REXCVAR_SET(controller_button_map, std::string());

  SDLDriverFixture fixture;
  VirtualGamepad gamepad;
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_EAST, true);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFTX, 12000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTX, 3000);
  gamepad.Commit();

  X_INPUT_STATE state = {};
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  const uint32_t modern_packet = static_cast<uint32_t>(state.packet_number);
  CHECK((state.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_B) != 0);
  CHECK(state.gamepad.thumb_lx == 12000);

  REXCVAR_SET(controller_layout, std::string("classic"));
  REXCVAR_SET(controller_button_map, std::string("a=b,b=a"));
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(state.packet_number) == modern_packet + 1);
  CHECK((state.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  CHECK((state.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_B) == 0);
  CHECK(state.gamepad.thumb_lx == 3000);
  CHECK(state.gamepad.thumb_rx == 12000);

  rex::input::ControllerSnapshot snapshot;
  REQUIRE(fixture.Snapshot(0, snapshot));
  CHECK((snapshot.raw_gamepad.buttons & rex::input::X_INPUT_GAMEPAD_B) != 0);
  CHECK((snapshot.raw_gamepad.buttons & rex::input::X_INPUT_GAMEPAD_A) == 0);
  CHECK((snapshot.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  CHECK(snapshot.gamepad.thumb_lx == 3000);

  rex::input::X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(fixture.driver.GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
  CHECK(keystroke.virtual_key == static_cast<uint16_t>(
                                      rex::ui::VirtualKey::kXInputPadA));
}
