/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "", "Input",
                      "Optional path to an additional SDL gamepad mappings file");

namespace rex::input::sdl {

namespace {

constexpr std::array<uint16_t, SDL_GAMEPAD_BUTTON_COUNT> kXButtonLookup = {
    X_INPUT_GAMEPAD_A,
    X_INPUT_GAMEPAD_B,
    X_INPUT_GAMEPAD_X,
    X_INPUT_GAMEPAD_Y,
    X_INPUT_GAMEPAD_BACK,
    X_INPUT_GAMEPAD_GUIDE,
    X_INPUT_GAMEPAD_START,
    X_INPUT_GAMEPAD_LEFT_THUMB,
    X_INPUT_GAMEPAD_RIGHT_THUMB,
    X_INPUT_GAMEPAD_LEFT_SHOULDER,
    X_INPUT_GAMEPAD_RIGHT_SHOULDER,
    X_INPUT_GAMEPAD_DPAD_UP,
    X_INPUT_GAMEPAD_DPAD_DOWN,
    X_INPUT_GAMEPAD_DPAD_LEFT,
    X_INPUT_GAMEPAD_DPAD_RIGHT,
    X_INPUT_GAMEPAD_GUIDE,  // Share, microphone, or capture button.
    X_INPUT_GAMEPAD_Y,      // Optional right upper paddle.
    X_INPUT_GAMEPAD_B,      // Optional left upper paddle.
    X_INPUT_GAMEPAD_X,      // Optional right lower paddle.
    X_INPUT_GAMEPAD_A,      // Optional left lower paddle.
    X_INPUT_GAMEPAD_GUIDE,  // PlayStation touchpad click.
    0,
    0,
    0,
    0,
    0,
};

bool ApplyAxis(X_INPUT_GAMEPAD& pad, SDL_GamepadAxis axis, int16_t value) {
  switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = value;
      return true;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~value;
      return true;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = value;
      return true;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~value;
      return true;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(std::max<int16_t>(0, value) >> 7);
      return true;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(std::max<int16_t>(0, value) >> 7);
      return true;
    default:
      return false;
  }
}

bool ApplyButton(X_INPUT_GAMEPAD& pad, SDL_GamepadButton button, bool down) {
  if (button < 0 || static_cast<size_t>(button) >= kXButtonLookup.size()) {
    return false;
  }
  const uint16_t xbutton = kXButtonLookup.at(static_cast<size_t>(button));
  if (!xbutton) {
    return false;
  }
  if (down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(guide_button)) {
      return false;
    }
    pad.buttons = static_cast<uint16_t>(pad.buttons) | xbutton;
  } else {
    pad.buttons = static_cast<uint16_t>(pad.buttons) & ~xbutton;
  }
  return true;
}

}  // namespace

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_(),
      keystroke_states_() {}

SDLInputDriver::~SDLInputDriver() {
  assert_null(attached_window_);
}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  std::lock_guard lifecycle_guard(lifecycle_mutex_);
  if (!window || attached_window_) {
    return;
  }
  attached_window_ = window;
  window->AddListener(this);
  window->app_context().CallInUIThreadSynchronous([this]() {
    // Register the watch before gamepad initialization so already-connected
    // devices are observed when SDL emits its initial added events.
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
      REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
      return;
    }
    sdl_events_initialized_ = true;
    pending_events_.reserve(64);
    accepting_events_.store(true, std::memory_order_release);
    if (!SDL_AddEventWatch(EventWatch, this)) {
      REXLOG_ERROR("SDL: Failed to register gamepad event watch: {}", SDL_GetError());
      accepting_events_.store(false, std::memory_order_release);
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
      return;
    }
    sdl_event_watch_registered_ = true;

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
      REXLOG_ERROR("SDL: Failed to init gamepad subsystem: {}", SDL_GetError());
      accepting_events_.store(false, std::memory_order_release);
      SDL_RemoveEventWatch(EventWatch, this);
      sdl_event_watch_registered_ = false;
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
      return;
    }
    SDL_Gamepad_initialized_ = true;

    // Load custom controller mappings if available
    if (!REXCVAR_GET(hid_mappings_file).empty()) {
      std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
      if (!std::filesystem::exists(mappings_path)) {
        REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                    REXCVAR_GET(hid_mappings_file));
      } else {
        auto mappings_result =
            SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
        if (mappings_result < 0) {
          REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                       REXCVAR_GET(hid_mappings_file), mappings_result);
        } else {
          REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
        }
      }
    }

    // Do not depend exclusively on queue timing for startup discovery.
    // Added events that are already pending are harmless because opening an
    // instance is idempotent below.
    {
      std::lock_guard guard(controllers_mutex_);
      OpenUnassignedControllersLocked();
    }
    ready_.store(true, std::memory_order_release);
    REXLOG_INFO("SDL gamepad input initialized successfully");
  });
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  OnWindowUnavailable();
}

void SDLInputDriver::OnWindowUnavailable() {
  std::lock_guard lifecycle_guard(lifecycle_mutex_);
  auto* window = attached_window_;
  if (!window) {
    return;
  }
  ready_.store(false, std::memory_order_release);
  attached_window_ = nullptr;
  window->RemoveListener(this);
  accepting_events_.store(false, std::memory_order_release);
  if (sdl_event_watch_registered_) {
    SDL_RemoveEventWatch(EventWatch, this);
    sdl_event_watch_registered_ = false;
  }
  if (sdl_pumpevents_queued_.load(std::memory_order_acquire)) {
    window->app_context().CallInUIThreadSynchronous(
        [window]() { window->app_context().ExecutePendingFunctionsFromUIThread(); });
  }
  sdl_pumpevents_queued_.store(false, std::memory_order_release);
  {
    std::lock_guard guard(controllers_mutex_);
    for (auto& controller : controllers_) {
      if (controller.sdl) {
        SDL_RumbleGamepad(controller.sdl, 0, 0, 0);
        SDL_CloseGamepad(controller.sdl);
        controller = {};
      }
    }
    keystroke_states_ = {};
  }
  {
    std::lock_guard guard(event_queue_mutex_);
    pending_events_.clear();
  }
  sdl_events_unflushed_.store(0, std::memory_order_release);
  if (SDL_Gamepad_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_Gamepad_initialized_ = false;
  }
  if (sdl_events_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    sdl_events_initialized_ = false;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  StopRumble();
}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::OnInputActiveChanged(bool active) {
  if (!active) {
    StopRumble();
  }
}

X_RESULT SDLInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (user_index >= HID_SDL_USER_COUNT || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index >= HID_SDL_USER_COUNT || !out_state) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (user_index >= HID_SDL_USER_COUNT || !vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const bool active = is_active();
  const uint16_t left = active ? static_cast<uint16_t>(vibration->left_motor_speed) : 0;
  const uint16_t right = active ? static_cast<uint16_t>(vibration->right_motor_speed) : 0;
  // SDL clamps longer requests to this duration. XInput vibration updates
  // refresh it during normal play.
  const uint32_t duration = (left || right) ? std::numeric_limits<uint16_t>::max() : 0;
  return SDL_RumbleGamepad(controller->sdl, left, right, duration) ? X_ERROR_SUCCESS
                                                                   : X_ERROR_FUNCTION_FAILED;
}

X_RESULT SDLInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  (void)flags;
  bool user_any = users == 0xFF;
  if (users >= HID_SDL_USER_COUNT && !user_any) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  for (uint32_t user_index = (user_any ? 0 : users);
       user_index < (user_any ? HID_SDL_USER_COUNT : users + 1); user_index++) {
    auto controller = GetControllerState(user_index);
    if (!controller) {
      if (user_any) {
        continue;
      } else {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }

    // If input is not active (e.g. due to a dialog overlay), force buttons to
    // "unpressed". The algorithm will automatically send UP events when
    // `is_active()` goes low and DOWN events when it goes high again.
    const uint64_t curr_butts =
        is_active
            ? (controller->state.gamepad.buttons | AnalogToKeyfield(controller->state.gamepad))
            : uint64_t(0);
    KeystrokeState& last = keystroke_states_.at(user_index);

    // Handle repeating
    auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
    static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
    if (last.repeat_state == RepeatState::Waiting &&
        (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
      last.repeat_state = RepeatState::Repeating;
    }
    if (last.repeat_state == RepeatState::Repeating &&
        (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
      last.repeat_time = guest_now;
      rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
      assert_true(vk != rex::ui::VirtualKey::kNone);
      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;
      out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }

    auto butts_changed = curr_butts ^ last.buttons;
    if (!butts_changed) {
      continue;
    }

    // First try to clear buttons with up events. This is to match xinput
    // behaviour when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
    // up before THUMB_LEFT is down.
    for (auto [clear_pass, i] = std::tuple{true, 0}; i < 2; clear_pass = false, i++) {
      for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
        auto fbutton = uint64_t(1) << i;
        if (!(butts_changed & fbutton)) {
          continue;
        }
        rex::ui::VirtualKey vk = kVkLookup.at(i);
        if (vk == rex::ui::VirtualKey::kNone) {
          continue;
        }

        out_keystroke->virtual_key = uint16_t(vk);
        out_keystroke->unicode = 0;
        out_keystroke->user_index = user_index;
        out_keystroke->hid_code = 0;

        bool is_pressed = curr_butts & fbutton;
        if (clear_pass && !is_pressed) {
          // up
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
          last.buttons &= ~fbutton;
          last.repeat_state = RepeatState::Idle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && is_pressed) {
          // down
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
          last.buttons |= fbutton;
          last.repeat_state = RepeatState::Waiting;
          last.repeat_butt_idx = i;
          last.repeat_time = guest_now;
          return X_ERROR_SUCCESS;
        }
      }
    }
  }
  return X_ERROR_EMPTY;
}

bool SDLCALL SDLInputDriver::EventWatch(void* userdata, SDL_Event* event) {
  if (!userdata || !event) {
    return false;
  }
  auto* driver = static_cast<SDLInputDriver*>(userdata);
  if (!driver->accepting_events_.load(std::memory_order_acquire)) {
    return false;
  }
  switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      driver->HandleEvent(*event);
      break;
    default:
      break;
  }
  return false;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMAPPED:
      OnControllerDeviceRemappedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  OpenControllerLocked(event.gdevice.which);
}

bool SDLInputDriver::OpenControllerLocked(SDL_JoystickID instance_id) {
  if (auto existing = GetControllerIndexFromInstanceID(instance_id)) {
    RefreshControllerStateLocked(controllers_.at(*existing));
    UpdateXCapabilities(controllers_.at(*existing));
    return true;
  }

  const auto controller = SDL_OpenGamepad(instance_id);
  if (!controller) {
    REXLOG_WARN("SDL: Could not open gamepad {}: {}", instance_id, SDL_GetError());
    return false;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X})",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller));

  int user_id = -1;
  // GoldenEye consumes player 1. Always fill slots from zero instead of
  // trusting a remembered host player index that may start at another slot.
  for (size_t i = 0; i < controllers_.size(); i++) {
    if (!controllers_.at(i).sdl) {
      user_id = static_cast<int>(i);
      SDL_SetGamepadPlayerIndex(controller, user_id);
      break;
    }
  }
  if (user_id >= 0) {
    auto& state = controllers_.at(user_id);
    state = {};
    state.sdl = controller;
    state.state_changed = true;
    RefreshControllerStateLocked(state);
    UpdateXCapabilities(state);

    REXLOG_INFO("SDL OnControllerDeviceAdded: Added at index {}.", user_id);
    return true;
  } else {
    SDL_CloseGamepad(controller);
    REXLOG_WARN("SDL OnControllerDeviceAdded: Ignored. No free slots.");
    return false;
  }
}

void SDLInputDriver::OpenUnassignedControllersLocked() {
  int count = 0;
  SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
  if (!gamepads) {
    return;
  }
  for (int i = 0; i < count; ++i) {
    OpenControllerLocked(gamepads[i]);
  }
  SDL_free(gamepads);
}

void SDLInputDriver::CompactControllerSlotsLocked() {
  size_t destination = 0;
  for (size_t source = 0; source < controllers_.size(); ++source) {
    if (!controllers_.at(source).sdl) {
      continue;
    }
    if (source != destination) {
      controllers_.at(destination) = controllers_.at(source);
      controllers_.at(source) = {};
      keystroke_states_.at(destination) = keystroke_states_.at(source);
      keystroke_states_.at(source) = {};
    }
    SDL_SetGamepadPlayerIndex(controllers_.at(destination).sdl, static_cast<int>(destination));
    ++destination;
  }
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  // Find the disconnected gamecontroller and close it.
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (idx) {
    SDL_CloseGamepad(controllers_.at(*idx).sdl);
    controllers_.at(*idx) = {};
    keystroke_states_.at(*idx) = {};
    REXLOG_INFO("SDL OnControllerDeviceRemoved: Removed at player index {}.", *idx);
    CompactControllerSlotsLocked();
    // A fifth connected controller may have been waiting for a free guest
    // slot. Reconsider all currently connected, unopened devices now.
    OpenUnassignedControllersLocked();
  } else {
    REXLOG_DEBUG("SDL OnControllerDeviceRemoved: Ignored unused device.");
  }
}

void SDLInputDriver::OnControllerDeviceRemappedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    OpenControllerLocked(event.gdevice.which);
    return;
  }
  auto& controller = controllers_.at(*idx);
  RefreshControllerStateLocked(controller);
  UpdateXCapabilities(controller);
  keystroke_states_.at(*idx) = {};
  REXLOG_INFO("SDL gamepad mapping refreshed at player index {}.", *idx);
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  if (!idx) {
    return;
  }
  auto& controller = controllers_.at(*idx);
  if (ApplyAxis(controller.state.gamepad, static_cast<SDL_GamepadAxis>(event.gaxis.axis),
                event.gaxis.value)) {
    controller.state_changed = true;
  }
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gbutton.which);
  if (!idx) {
    return;
  }
  auto& controller = controllers_.at(*idx);
  if (ApplyButton(controller.state.gamepad, static_cast<SDL_GamepadButton>(event.gbutton.button),
                  event.gbutton.down)) {
    controller.state_changed = true;
  }
}

void SDLInputDriver::RefreshControllerStateLocked(ControllerState& controller) {
  assert_not_null(controller.sdl);
  auto& pad = controller.state.gamepad;
  std::memset(&pad, 0, sizeof(pad));
  for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
    ApplyAxis(pad, static_cast<SDL_GamepadAxis>(axis),
              SDL_GetGamepadAxis(controller.sdl, static_cast<SDL_GamepadAxis>(axis)));
  }
  for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
    if (SDL_GetGamepadButton(controller.sdl, static_cast<SDL_GamepadButton>(button))) {
      ApplyButton(pad, static_cast<SDL_GamepadButton>(button), true);
    }
  }
  controller.state_changed = true;
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    if (!controller) {
      continue;
    }
    auto joystick = SDL_GetGamepadJoystick(controller);
    if (!joystick) {
      continue;
    }
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::GetControllerState(uint32_t user_index) {
  if (user_index >= controllers_.size()) {
    return nullptr;
  }
  auto controller = &controllers_.at(user_index);
  if (!controller->sdl) {
    return nullptr;
  }
  return controller;
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_INFO("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
              SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(state.sdl)) ==
      SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::StopRumble() {
  if (!ready_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard guard(controllers_mutex_);
  for (auto& controller : controllers_) {
    if (controller.sdl) {
      SDL_RumbleGamepad(controller.sdl, 0, 0, 0);
    }
  }
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date.
  if (!ready_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard lifecycle_guard(lifecycle_mutex_);
  auto* window = attached_window_;
  if (!window || !ready_.load(std::memory_order_acquire)) {
    return;
  }
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    if (!window->app_context().CallInUIThread([this]() {
          if (ready_.load(std::memory_order_acquire)) {
            SDL_PumpEvents();
          }
          sdl_pumpevents_queued_.store(false, std::memory_order_release);
        })) {
      sdl_pumpevents_queued_.store(false, std::memory_order_release);
    }
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
