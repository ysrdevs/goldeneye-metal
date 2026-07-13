#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse_enabled);
REXCVAR_DECLARE(double, mnk_sensitivity);
REXCVAR_DECLARE(std::string, keybind_start);
REXCVAR_DECLARE(std::string, keybind_lstick_up);
REXCVAR_DECLARE(std::string, keybind_rstick_up);

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_GAMEPAD_START;
using rex::input::X_INPUT_STATE;

namespace {

template <typename T>
class ScopedValue {
 public:
  ScopedValue(T& storage, T value) : storage_(storage), old_value_(storage) {
    storage_ = std::move(value);
  }

  ~ScopedValue() { storage_ = std::move(old_value_); }

 private:
  T& storage_;
  T old_value_;
};

rex::ui::KeyEvent Key(rex::ui::VirtualKey key) {
  return rex::ui::KeyEvent(nullptr, key, 1, false, false, false, false, false);
}

class TestAppContext final : public rex::ui::WindowedAppContext {
 public:
  TestAppContext() = default;

 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class TestWindow final : public rex::ui::Window {
 public:
  explicit TestWindow(TestAppContext& context) : Window(context, "MnK test", 1280, 720) {}
  ~TestWindow() override { EnterDestructor(); }

  uint32_t capture_calls() const { return capture_calls_; }
  uint32_t release_calls() const { return release_calls_; }
  bool capture_applied_on_ui_thread() const { return capture_applied_on_ui_thread_; }

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

  void ApplyNewMouseCapture() override {
    ++capture_calls_;
    capture_applied_on_ui_thread_ &= app_context().IsInUIThread();
  }

  void ApplyNewMouseRelease() override {
    ++release_calls_;
    capture_applied_on_ui_thread_ &= app_context().IsInUIThread();
  }

  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(
      rex::ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}

 private:
  uint32_t capture_calls_ = 0;
  uint32_t release_calls_ = 0;
  bool capture_applied_on_ui_thread_ = true;
};

void PumpUntilDone(TestAppContext& context, const std::atomic<bool>& done) {
  while (!done.load(std::memory_order_acquire)) {
    context.ExecutePendingFunctionsFromUIThread();
    std::this_thread::yield();
  }
  context.ExecutePendingFunctionsFromUIThread();
}

}  // namespace

TEST_CASE("MnK driver maps native keyboard and mouse events to controller state",
          "[input][mnk]") {
  ScopedValue<bool> enabled(REXCVAR_GET(mnk_mode), true);
  ScopedValue<double> sensitivity(REXCVAR_GET(mnk_sensitivity), 1.0);
  ScopedValue<std::string> start(REXCVAR_GET(keybind_start), "Return");

  rex::input::mnk::MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  auto w = Key(rex::ui::VirtualKey::kW);
  auto shift = Key(rex::ui::VirtualKey::kShift);
  auto enter = Key(rex::ui::VirtualKey::kReturn);
  driver.OnKeyDown(w);
  driver.OnKeyDown(shift);
  driver.OnKeyDown(enter);

  rex::ui::MouseEvent move(nullptr, rex::ui::MouseEvent::Button::kNone, 0, 0, 0, 0, {-4, 3});
  rex::ui::MouseEvent left_down(nullptr, rex::ui::MouseEvent::Button::kLeft, 0, 0);
  driver.OnMouseMove(move);
  driver.OnMouseDown(left_down);

  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ly == INT16_MAX);
  CHECK(state.gamepad.thumb_rx == -800);
  CHECK(state.gamepad.thumb_ry == -600);
  CHECK(state.gamepad.right_trigger == 0xFF);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_B) != 0);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_START) != 0);

  driver.OnKeyUp(w);
  driver.OnKeyUp(shift);
  driver.OnKeyUp(enter);
  rex::ui::MouseEvent left_up(nullptr, rex::ui::MouseEvent::Button::kLeft, 0, 0);
  driver.OnMouseUp(left_up);
}

TEST_CASE("MnK driver supports keyboard right-stick binds and modal suppression",
          "[input][mnk]") {
  ScopedValue<bool> enabled(REXCVAR_GET(mnk_mode), true);
  ScopedValue<std::string> look_up(REXCVAR_GET(keybind_rstick_up), "I");

  rex::input::mnk::MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  bool active = true;
  driver.set_is_active_callback([&active] { return active; });

  auto look = Key(rex::ui::VirtualKey::kI);
  driver.OnKeyDown(look);

  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ry == INT16_MAX);

  active = false;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ry == 0);

  // Keys pressed by a modal host UI never leak into gameplay when it closes.
  auto w = Key(rex::ui::VirtualKey::kW);
  driver.OnKeyDown(w);
  active = true;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ly == 0);

  driver.OnKeyUp(look);
  driver.OnKeyUp(w);
}

TEST_CASE("MnK driver supports comma-separated alternative bindings", "[input][mnk]") {
  ScopedValue<bool> enabled(REXCVAR_GET(mnk_mode), true);
  ScopedValue<std::string> move_up(REXCVAR_GET(keybind_lstick_up), "Up, W");

  rex::input::mnk::MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  auto w = Key(rex::ui::VirtualKey::kW);
  driver.OnKeyDown(w);

  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ly == INT16_MAX);

  driver.OnKeyUp(w);
}

TEST_CASE("MnK driver host activity notification clears held input", "[input][mnk]") {
  ScopedValue<bool> enabled(REXCVAR_GET(mnk_mode), true);

  rex::input::mnk::MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  driver.set_is_active_callback([] { return true; });

  auto w = Key(rex::ui::VirtualKey::kW);
  driver.OnKeyDown(w);
  driver.OnInputActiveChanged(false);

  // Even an independently true polling callback cannot override an explicit
  // host-menu suppression notification.
  driver.OnKeyDown(w);
  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ly == 0);

  driver.OnInputActiveChanged(true);
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ly == 0);
  driver.OnKeyUp(w);
}

TEST_CASE("MnK driver serializes native capture and ignores a stale guest poll",
          "[input][mnk][window]") {
  ScopedValue<bool> enabled(REXCVAR_GET(mnk_mode), true);
  ScopedValue<bool> mouse_enabled(REXCVAR_GET(mnk_mouse_enabled), true);

  TestAppContext context;
  TestWindow window(context);
  REQUIRE(window.Open());

  rex::input::mnk::MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  driver.OnWindowAvailable(&window);
  driver.set_is_active_callback([] { return true; });

  X_RESULT guest_result = X_ERROR_DEVICE_NOT_CONNECTED;
  std::atomic<bool> guest_done{false};
  std::thread guest_poll([&] {
    X_INPUT_STATE state = {};
    guest_result = driver.GetState(0, &state);
    guest_done.store(true, std::memory_order_release);
  });
  PumpUntilDone(context, guest_done);
  guest_poll.join();

  REQUIRE(guest_result == X_ERROR_SUCCESS);
  CHECK(window.IsMouseCaptureRequested());
  CHECK(window.capture_calls() == 1);
  CHECK(window.capture_applied_on_ui_thread());

  // A steady-state poll must not enqueue another common Window transition.
  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(window.capture_calls() == 1);

  std::atomic<bool> callback_sampled{false};
  std::atomic<bool> finish_callback{false};
  driver.set_is_active_callback([&] {
    if (!context.IsInUIThread()) {
      callback_sampled.store(true, std::memory_order_release);
      while (!finish_callback.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
    return true;
  });

  guest_done.store(false, std::memory_order_release);
  std::thread stale_guest_poll([&] {
    X_INPUT_STATE stale_state = {};
    guest_result = driver.GetState(0, &stale_state);
    guest_done.store(true, std::memory_order_release);
  });
  while (!callback_sampled.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  driver.OnInputActiveChanged(false);
  CHECK_FALSE(window.IsMouseCaptureRequested());
  CHECK(window.release_calls() == 1);
  finish_callback.store(true, std::memory_order_release);
  PumpUntilDone(context, guest_done);
  stale_guest_poll.join();

  CHECK_FALSE(window.IsMouseCaptureRequested());
  CHECK(window.capture_calls() == 1);
  CHECK(window.release_calls() == 1);
  CHECK(window.capture_applied_on_ui_thread());

  driver.set_is_active_callback([] { return true; });
  driver.OnInputActiveChanged(true);
  CHECK(window.IsMouseCaptureRequested());
  driver.OnWindowUnavailable();
  CHECK_FALSE(window.IsMouseCaptureRequested());
  CHECK(window.capture_calls() == 2);
  CHECK(window.release_calls() == 2);
}

TEST_CASE("MnK driver clears held state on focus loss", "[input][mnk]") {
  ScopedValue<bool> enabled(REXCVAR_GET(mnk_mode), true);

  rex::input::mnk::MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  auto w = Key(rex::ui::VirtualKey::kW);
  driver.OnKeyDown(w);

  rex::ui::UISetupEvent focus_event;
  driver.OnLostFocus(focus_event);

  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ly == 0);

  driver.OnGotFocus(focus_event);
  driver.OnKeyUp(w);
}

TEST_CASE("MnK driver can release mouse capture without disabling keyboard", "[input][mnk]") {
  ScopedValue<bool> enabled(REXCVAR_GET(mnk_mode), true);
  ScopedValue<bool> mouse_enabled(REXCVAR_GET(mnk_mouse_enabled), false);

  rex::input::mnk::MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  auto w = Key(rex::ui::VirtualKey::kW);
  driver.OnKeyDown(w);
  rex::ui::MouseEvent move(nullptr, rex::ui::MouseEvent::Button::kNone, 0, 0, 0, 0, {8, 4});
  rex::ui::MouseEvent left_down(nullptr, rex::ui::MouseEvent::Button::kLeft, 0, 0);
  driver.OnMouseMove(move);
  driver.OnMouseDown(left_down);

  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_ly == INT16_MAX);
  CHECK(state.gamepad.thumb_rx == 0);
  CHECK(state.gamepad.thumb_ry == 0);
  CHECK(state.gamepad.right_trigger == 0);

  driver.OnKeyUp(w);
}
