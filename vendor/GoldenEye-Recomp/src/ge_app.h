
// ge - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/perf/metal_performance.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <atomic>
#include <string>

#include "ge_menu.h"
#include "ge_host_pause.h"
#include "ge_launcher.h"
#include "ge_postfx.h"

// Relaunch the current executable as a fresh process (implemented in
// ge_hooks.cpp, which owns the Win32 includes). Used by the ONLINE menu's
// "Save & Restart" so username/server/enable changes take effect on a clean
// boot -- they are read at startup (UserProfile ctor, online client start).
namespace ge {
void LaunchSelfDetached();
// Start the raw-mouse + cursor-capture thread at startup. Implemented in
// ge_hooks.cpp.
void InitMouseLook();
// Suppress mouse-look while the pause menu is open (cursor is needed for the
// menu, and motion shouldn't turn into look). Implemented in ge_hooks.cpp.
void SetMouselookSuppressed(bool suppressed);
}  // namespace ge

class GeApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GeApp>(new GeApp(ctx, "ge", PPCImageConfig));
  }

  // GoldenEye boot defaults. Runs before the config file is loaded, so these
  // are just defaults -- the configuration file written by the in-game menu
  // overrides them.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    ge::ConfigureLauncherPaths(paths);
    // NOTE: vsync is NOT forced here. Its SDK default is false (off), so the
    // in-menu toggle persists: turning it ON differs from default -> written to
    // the config; OFF == default -> not written but still boots off. Forcing it here
    // would re-assert off every boot and the "on" choice would never survive a
    // restart (SaveConfig only writes cvars that differ from their default).
    rex::cvar::SetFlagByName("max_fps", "60");  // default 60 (clamped to native refresh)
    rex::cvar::SetFlagByName("window_width", "2560");
    rex::cvar::SetFlagByName("window_height", "1440");
    // NOTE: fullscreen is NOT forced here. Its default is set to true at the
    // framework level (window.cpp) instead. That makes "windowed" the
    // non-default value, so toggling to windowed actually saves to the config --
    // SaveConfig only writes cvars that differ from their default. Forcing
    // fullscreen=true here would re-assert it every boot and the windowed
    // choice would never persist. The throttle is the same story: its default
    // lives in its REXCVAR_DEFINE and it is tuned live from the pause menu, so
    // it is never written here (writing default==default is a no-op anyway).
  }

  void OnPostInitLogging() override {
#if defined(__APPLE__)
    bool repaired_macos_config = false;

    // Older macOS builds exposed the cross-platform GPU selector even though
    // this target contains Metal only. Repair a stale Vulkan / D3D choice
    // before presentation setup so an upgrade can never fail at startup.
    const std::string gpu = rex::cvar::GetFlagByName("gpu");
    if (gpu != "metal") {
      if (gpu == "auto") {
        REXLOG_INFO("Selecting Metal graphics for macOS");
      } else {
        REXLOG_WARN("Replacing unsupported gpu={} setting with Metal on macOS", gpu);
      }
      repaired_macos_config |= rex::cvar::SetFlagByName("gpu", "metal");
    }

    // The macOS build does not expose the unfinished online relay. Normalize
    // stale configs (and environment overrides) before the runtime can consult
    // this gate, so they can never route System Link traffic into a backend
    // that has no macOS implementation.
    if (rex::cvar::GetFlagByName("ge_online_enable") == "true") {
      REXLOG_WARN("Disabling unsupported ge_online_enable setting on macOS");
      repaired_macos_config |= rex::cvar::SetFlagByName("ge_online_enable", "false");
    }

    // GoldenEye's clock is 60 Hz. Older builds exposed a 30 FPS option that
    // changed game timing, so don't let that stale value survive after the
    // control is removed from the macOS menu.
    if (rex::cvar::GetFlagByName("max_fps") != "60") {
      REXLOG_WARN("Resetting unsupported max_fps setting to 60 on macOS");
      repaired_macos_config |= rex::cvar::SetFlagByName("max_fps", "60");
    }

    // Config loading validates this range, but environment cvars are applied
    // through their typed setters and may contain an out-of-range value. Keep
    // the Metal sampler behavior and the Video menu's displayed choice aligned.
    const std::string anisotropic_override = rex::cvar::GetFlagByName("anisotropic_override");
    if (anisotropic_override != "-1" && anisotropic_override != "0" &&
        anisotropic_override != "1" && anisotropic_override != "2" && anisotropic_override != "3" &&
        anisotropic_override != "4" && anisotropic_override != "5") {
      REXLOG_WARN("Resetting invalid anisotropic_override={} to 3 on macOS", anisotropic_override);
      repaired_macos_config |= rex::cvar::SetFlagByName("anisotropic_override", "3");
    }

    // OnPostInitLogging runs after the config path is resolved and both the
    // saved config and environment overrides are applied. Flush repairs now
    // so an old unsupported value is removed from disk instead of recurring
    // on every launch.
    if (repaired_macos_config) {
      PersistConfig();
    }
#endif
    // Lets tester logs prove that the crash-guard build was actually launched.
    REXLOG_INFO(
        "[GE-GUARD-823DFB70-v1] active; protects packed-data loads at "
        "823DFBAC and 823DFBD8");
    REXLOG_INFO(
        "[GE-GUARD-823DFB70-purecall-v1] active; protects packed-data dispatches at "
        "823DFBA8 and 823DFBD0");
    REXLOG_INFO(
        "[GE-GUARD-823CFC00-v1] active; preserves cleanup callback ABI at "
        "823CFC84");
    if (auto* logger = rex::GetLoggerRaw(rex::log::core())) {
      logger->flush();
    }
  }

  std::optional<rex::PathConfig> OnPreparePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    return ge::PrepareLauncherPaths(defaults, std::move(resume), app_context());
  }

  // Create the Post-FX spatial layer once the ImGui drawer exists, but only if
  // vignette or scanlines actually need it.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
#if defined(__APPLE__)
    // The SDK's raw F4 settings overlay exposes every cross-platform cvar,
    // including backends and effects that don't exist in this Metal build.
    // Keep the player-facing GoldenEye settings as the single honest UI.
    rex::ui::UnregisterBind("bind_settings");

    // Keep the physical controller available to the host settings even while
    // guest input is intentionally suppressed. Dear ImGui consumes this for
    // A/B, D-pad and left-stick navigation; it never feeds back into the game.
    drawer->SetGamepadStateProvider([this](rex::ui::ImGuiGamepadState* out) {
      // The drawer may remain alive for a passive Post-FX overlay. Polling
      // the controller there would add needless work to every gameplay
      // frame, so activate this host-only feed only for the settings menu.
      if (!menu_ || !out || !runtime() || !runtime()->input_system()) {
        return false;
      }
      auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
      rex::input::ControllerSnapshot snapshot;
      if (!input->GetControllerSnapshot(0, &snapshot)) {
        return false;
      }
      const uint16_t buttons = static_cast<uint16_t>(snapshot.raw_gamepad.buttons);
      out->face_down = (buttons & rex::input::X_INPUT_GAMEPAD_A) != 0;
      out->face_right = (buttons & rex::input::X_INPUT_GAMEPAD_B) != 0;
      out->face_left = (buttons & rex::input::X_INPUT_GAMEPAD_X) != 0;
      out->face_up = (buttons & rex::input::X_INPUT_GAMEPAD_Y) != 0;
      out->dpad_left = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) != 0;
      out->dpad_right = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) != 0;
      out->dpad_up = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_UP) != 0;
      out->dpad_down = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) != 0;
      out->left_shoulder = (buttons & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) != 0;
      out->right_shoulder = (buttons & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
      out->left_stick_button = (buttons & rex::input::X_INPUT_GAMEPAD_LEFT_THUMB) != 0;
      out->right_stick_button = (buttons & rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB) != 0;
      out->start = (buttons & rex::input::X_INPUT_GAMEPAD_START) != 0;
      out->back = (buttons & rex::input::X_INPUT_GAMEPAD_BACK) != 0;
      out->left_stick_x =
          rex::input::controller::AxisToUnit(static_cast<int16_t>(snapshot.raw_gamepad.thumb_lx));
      out->left_stick_y =
          rex::input::controller::AxisToUnit(static_cast<int16_t>(snapshot.raw_gamepad.thumb_ly));
      return true;
    });
#endif
    // Window/taskbar title shown while running. Overrides the SDK default
    // ("ge <build stamp>"); the internal app name stays "ge" for runtime
    // identity and compatibility.
    if (window())
      window()->SetTitle("GoldenEye");
    ge::InitMouseLook();  // start raw-mouse capture/look thread
#if defined(__APPLE__)
    SyncPostFxOverlay(drawer);
#else
    postfx_ = std::make_unique<ge::PostFxOverlay>(drawer);
#endif
    // Username/server are set in the ONLINE pause-menu tab now -- no first-boot
    // prompt. They apply on the Save & Restart the ONLINE tab triggers.
  }

  void OnPostSetup() override {
#if defined(__APPLE__)
    if (runtime() && runtime()->input_system()) {
      auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
      input->SetMouseMotionMode(rex::input::MouseMotionMode::kApplication);
      REXLOG_INFO("GEMOUSE macOS direct camera/menu motion active");
    }
#endif
  }

  // Tear down the menu, overlay and keybind before the drawer is destroyed.
  void OnShutdown() override {
    input_suppressed_.store(true, std::memory_order_release);
    NotifyInputActiveChanged();
    // A direct shutdown/restart can destroy the dialog without its normal
    // OnClose callback. Queue an idempotent release while the game-thread
    // bridge is still alive; process termination remains the final fallback.
    ge::host_pause::RequestPaused(false);
#if defined(__APPLE__)
    if (auto* drawer = imgui_drawer()) {
      // The provider captures this application object. Release it before the
      // application lifetime ends even if another host dialog was added.
      drawer->SetGamepadStateProvider({});
    }
#endif
    if (menu_) {
      // Direct delete (not Close()) so we don't re-enter pause bookkeeping
      // during shutdown; removes itself from the drawer in its destructor.
      delete menu_;
      menu_ = nullptr;
    }
    postfx_.reset();
  }

 private:
  void OnKeyDown(rex::ui::KeyEvent& event) override {
    // Keep the player menu on a hard host-level Escape path. Going through the
    // configurable bind registry made this essential control vulnerable to a
    // missing/stale late-registered bind on macOS. Ignore key repeat so holding
    // Escape cannot immediately reopen/close it.
    if (event.virtual_key() == rex::ui::VirtualKey::kEscape) {
      if (!event.prev_state()) {
        TogglePauseMenu();
      }
      event.set_handled(true);
      return;
    }
    // Preserve the SDK's F3/console host bindings for every other key.
    rex::ui::ProcessKeyEvent(event);
  }

  bool IsInputActive() const override { return !input_suppressed_.load(std::memory_order_acquire); }

  // ESC handler: open or close the menu. Active offline local gameplay uses
  // the retail title's own pause state while the host UI remains responsive.
  void TogglePauseMenu() {
    if (menu_) {
      REXLOG_INFO("GEUI Escape requested pause-menu close");
      menu_->RequestClose();  // on_closed clears menu_
      return;
    }
    REXLOG_INFO("GEUI Escape requested pause-menu open");
    auto* drawer = imgui_drawer();
    if (!drawer) {
      // This should be unreachable once a graphics backend provides its UI
      // drawer, but never turn a missing optional host UI renderer into a game
      // crash again.
      REXLOG_ERROR("GEUI cannot open pause menu: host UI renderer is unavailable");
      return;
    }
    // A performance report measures unobstructed gameplay. Keep its collected
    // samples, but exclude all host-menu frames and presentation work.
    rex::perf::PauseMetalPerformanceReport();
#if defined(__APPLE__)
    // Stage the passive layer before the menu. While the menu is open it costs
    // no additional presentation mode change, draws nothing unless spatial
    // effects are enabled, and is ready behind the folder for live presets.
    if (!postfx_) {
      postfx_ = std::make_unique<ge::PostFxOverlay>(drawer);
    }
#endif
    GeMenuDialog::Callbacks cb;
    cb.on_closed = [this] {
      ge::host_pause::RequestPaused(false);
      menu_ = nullptr;
#if defined(__APPLE__)
      SyncPostFxOverlay();
#endif
      input_suppressed_.store(false, std::memory_order_release);
      NotifyInputActiveChanged();
      ge::SetMouselookSuppressed(false);  // re-enable mouse-look on menu close
      rex::perf::ResumeMetalPerformanceReport();
    };
    cb.on_quit = [this] { RequestShutdown(); };
    cb.get_fullscreen = [this] { return window() && window()->IsFullscreen(); };
    cb.request_fullscreen = [this](bool v) {
      // Persist the choice: update the cvar (so SaveConfig writes it) and flush
      // the config now. Without this the window changes but reverts next boot.
      rex::cvar::SetFlagByName("fullscreen", v ? "true" : "false");
      PersistConfig();
      // Defer off the paint thread: applying a window/surface change from inside
      // the ImGui draw (which runs during the presenter's paint) tears down the
      // surface being painted and crashes. Running it from the UI loop between
      // frames is the same safe path as a normal window resize.
      app_context().CallInUIThreadDeferred([this, v] {
        if (window())
          window()->SetFullscreen(v);
      });
    };
#if defined(__APPLE__)
    cb.on_vsync_changed = [this](bool) {
      // The checkbox is drawn from inside the presenter's paint. Defer the
      // layer update until that paint has completed so CAMetalLayer and the
      // presenter's surface ownership are changed only on the UI thread and
      // between frames.
      app_context().CallInUIThreadDeferred([this] {
        if (!runtime() || !runtime()->graphics_system()) {
          return;
        }
        auto* graphics_system =
            static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
        if (auto* presenter = graphics_system->presenter()) {
          presenter->OnVsyncChangedFromUIThread();
        }
      });
    };
#endif
    cb.persist_config = [this] { PersistConfig(); };
    cb.request_restart = [this] {
      // ONLINE tab "Save & Restart": the menu has already persisted the cvars;
      // launch a fresh process (which reads the new config at boot) then tear
      // this one down. Deferred to the UI thread -- never quit/relaunch from
      // inside the paint (same reason as request_fullscreen).
      app_context().CallInUIThreadDeferred([this] {
        ge::LaunchSelfDetached();
        RequestShutdown();
      });
    };
    input_suppressed_.store(true, std::memory_order_release);
    NotifyInputActiveChanged();
    ge::SetMouselookSuppressed(true);  // freeze mouse-look while the menu is up
    ge::host_pause::RequestPaused(true);
    menu_ = new GeMenuDialog(drawer, std::move(cb));
  }

#if defined(__APPLE__)
  void SyncPostFxOverlay(rex::ui::ImGuiDrawer* drawer = nullptr) {
    const bool spatial_effects_enabled = ge::PostFxSpatialEffectsEnabled();
    if (!spatial_effects_enabled) {
      postfx_.reset();
      return;
    }
    if (!postfx_) {
      if (!drawer) {
        drawer = imgui_drawer();
      }
      if (drawer) {
        postfx_ = std::make_unique<ge::PostFxOverlay>(drawer);
      }
    }
  }
#endif

  std::atomic<bool> input_suppressed_{false};
  GeMenuDialog* menu_ = nullptr;               // non-owning; self-deletes via the drawer
  std::unique_ptr<ge::PostFxOverlay> postfx_;  // spatial-effect layer, attached only when needed
};
