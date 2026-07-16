/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation — compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/graphics/graphics_system.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#if REX_HAS_METAL
#include <rex/graphics/metal/graphics_system.h>
#endif
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <fmt/format.h>
#include <imgui.h>

#include <filesystem>
#include <string>
#include <string_view>

// DXGI is used (Windows, both backends built) to read the primary GPU's PCI
// vendor id so "auto" can route AMD parts to Vulkan. Loaded dynamically below,
// but the types/IIDs come from this header.
#if defined(REX_PLATFORM_WINDOWS) && REX_HAS_D3D12 && REX_HAS_VULKAN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#endif

// Render backend selection. "auto" defaults to native Metal on macOS when it is
// built, otherwise Vulkan, and only uses D3D12 on confirmed NVIDIA GPUs on
// Windows -- D3D12 on this title black-screens / TDRs on AMD and Intel Arc.
// kRequiresRestart: the backend is built once at presentation setup, but the
// in-game menu must be able to write the choice at runtime (it takes effect on
// the next launch). kInitOnly would reject those post-init writes.
#if REX_HAS_METAL
REXCVAR_DEFINE_STRING(gpu, "auto", "GPU",
                      "Render backend: auto (macOS->Metal, NVIDIA Windows->D3D12, all "
                      "else->Vulkan), d3d12, vulkan, or metal")
    .allowed({"auto", "d3d12", "vulkan", "metal"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#else
REXCVAR_DEFINE_STRING(gpu, "auto", "GPU",
                      "Render backend: auto (NVIDIA->D3D12, all else->Vulkan), d3d12, or vulkan")
    .allowed({"auto", "d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#endif

namespace {
#if defined(REX_PLATFORM_WINDOWS) && REX_HAS_D3D12 && REX_HAS_VULKAN
constexpr uint32_t kVendorNVIDIA = 0x10DEu;
constexpr uint32_t kVendorAMD = 0x1002u;
constexpr uint32_t kVendorIntel = 0x8086u;

// PCI vendor id of the primary hardware GPU via DXGI (loaded dynamically so no
// link-time dependency is added and a missing dxgi.dll degrades gracefully).
// Returns 0 when it cannot be determined (no DXGI, only software adapters, etc.)
// -- callers treat "unknown" as "not NVIDIA" and prefer Vulkan, which is the
// safe path on this title (D3D12 black-screens / TDRs on AMD + Intel Arc; under
// Proton the Windows build still hits this, so 0x1002 must route to Vulkan too).
uint32_t PrimaryGpuVendorId() {
  HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
  if (!dxgi) {
    return 0u;
  }
  using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
  auto create_factory1 =
      reinterpret_cast<CreateFactory1Fn>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));
  uint32_t vendor = 0u;
  IDXGIFactory1* factory = nullptr;
  if (create_factory1 && SUCCEEDED(create_factory1(IID_PPV_ARGS(&factory))) && factory) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 desc;
      if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        vendor = desc.VendorId;  // first hardware adapter decides
        adapter->Release();
        break;
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
  }
  ::FreeLibrary(dxgi);
  return vendor;
}
#endif
}  // namespace

namespace rex {

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;

  auto paths = OnPreparePaths(resolved_defaults_, MakePreparePathsResumeCallback());
  if (!paths) {
    // Async native launcher. No graphics/game window has been created yet;
    // the application event loop keeps the platform UI responsive until the
    // consumer invokes resume.
    return true;
  }

  return ContinueInitialization(std::move(*paths));
}

bool ReXApp::ContinueInitialization(PathConfig paths) {
  if (!SetupPresentation())
    return false;

  auto finalized_paths = OnFinalizePaths(paths, MakeResumeCallback());
  if (!finalized_paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

  if (!ConstructRuntime(*finalized_paths))
    return false;
  LaunchModule();
  return true;
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  std::filesystem::path game_dir;
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  std::string cache_path_cvar = REXCVAR_GET(cache_path);
  if (!cache_path_cvar.empty()) {
    cache_dir = cache_path_cvar;
  } else {
    cache_dir = user_dir / "cache";
  }

  PathConfig path_config{game_dir, user_dir, update_dir, cache_dir,
                         exe_dir / (std::string(GetName()) + ".toml")};
  OnConfigurePaths(path_config);
  game_data_root_ = path_config.game_data_root;
  user_data_root_ = path_config.user_data_root;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  config_path_ = path_config.config_path;
  resolved_defaults_ = std::move(path_config);

  // Load config FIRST so log cvars have final values
  if (std::filesystem::exists(config_path_))
    rex::cvar::LoadConfig(config_path_);

  // Environment variables are explicit per-launch overrides. They are applied
  // once by the platform entry point so path cvars are available above, then
  // again after the persisted config so a launcher can reliably select a
  // graphics or input mode without requiring a pre-existing config edit.
  rex::cvar::ApplyEnvironment();

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    // Application bundles are immutable after signing. Keep default logs with
    // the rest of the per-user state instead of beside the executable.
    const auto& writable_root = user_data_root_.empty() ? exe_dir : user_data_root_;
    log_config.log_dir = (writable_root / "Logs").string();
  }

  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_INFO("Loaded config: {}", config_path_.filename().string());

  REXLOG_INFO("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_INFO("  Game data:      {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_INFO("  Cache root:     {}", cache_root_.string());

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  std::filesystem::path game_data_root = paths.game_data_root;
  if (game_data_root.empty()) {
    // No --game_data_root supplied (e.g. launched by double-click): default to
    // an "assets" folder staged next to the executable.
    auto fallback = rex::filesystem::GetExecutableFolder() / "assets";
    if (std::filesystem::is_directory(fallback)) {
      game_data_root = fallback;
      REXLOG_INFO("--game_data_root not given; defaulting to {}", game_data_root.string());
    }
  }
  if (game_data_root.empty()) {
    auto msg = std::string(
        "--game_data_root was not provided and no 'assets' folder was found next "
        "to the executable.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(game_data_root) &&
      !std::filesystem::is_regular_file(game_data_root)) {
    auto msg =
        fmt::format("--game_data_root must be an extracted directory or STFS package file: {}",
                    game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  // Path hooks may resolve game data asynchronously after SetupEnvironment.
  // Publish the final values through the normal accessors before constructing
  // the runtime so restarts and application-specific hooks see the same paths.
  game_data_root_ = game_data_root;
  user_data_root_ = paths.user_data_root;
  update_data_root_ = paths.update_data_root;
  cache_root_ = paths.cache_root;

  runtime_ = std::make_unique<rex::Runtime>(game_data_root, paths.user_data_root,
                                            paths.update_data_root, paths.cache_root);
  runtime_->set_app_context(&app_context());

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([this]() { return IsEffectiveInputActive(); });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Resolve through the runtime VFS so this works identically for extracted
  // directories and directly mounted STFS packages.
  auto* xex_entry = runtime_->file_system()->ResolvePath(xex_image);
  if (!xex_entry || (xex_entry->attributes() & rex::filesystem::kFileAttributeDirectory) != 0) {
    auto msg = fmt::format("Entrypoint XEX not found in game data: {} ({})", xex_image,
                           game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
  const std::string& gpu_pref = REXCVAR_GET(gpu);
  bool backend_selected = false;
#if REX_HAS_METAL
  if (gpu_pref == "metal"
#if defined(REX_PLATFORM_MAC)
      || gpu_pref == "auto"
#endif
  ) {
    config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::metal::MetalGraphicsSystem);
    backend_selected = true;
    REXLOG_INFO("Graphics backend: Metal (gpu={})", gpu_pref);
  }
#endif

#if REX_HAS_D3D12 && REX_HAS_VULKAN
  if (!backend_selected) {
    // Both backends compiled in -- choose per the `gpu` cvar. "auto" defaults to
    // Vulkan and only uses D3D12 when the GPU is confirmed NVIDIA: D3D12 on this
    // title black-screens / TDRs on AMD and Intel Arc (issues #5/#18/#21/#22/#23/
    // #25/#36/#44), and on Linux/Steam Deck (Windows build under Proton) there is
    // no native D3D12 at all -- Vulkan is the working path everywhere except NVIDIA
    // Windows, where D3D12 is preferred and well-tested. "d3d12"/"vulkan" force it.
    bool use_vulkan;
    if (gpu_pref == "vulkan") {
      use_vulkan = true;
    } else if (gpu_pref == "d3d12") {
      use_vulkan = false;
    } else {
#if defined(REX_PLATFORM_WINDOWS)
      uint32_t vendor = PrimaryGpuVendorId();
      use_vulkan = (vendor != kVendorNVIDIA);  // NVIDIA -> D3D12; all else -> Vulkan
      REXLOG_INFO("Auto GPU backend: vendor=0x{:04X} -> {}", vendor,
                  use_vulkan ? "Vulkan" : "D3D12");
#else
      use_vulkan = true;
#endif
    }
    if (use_vulkan) {
      config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
    } else {
      config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
    }
    backend_selected = true;
    REXLOG_INFO("Graphics backend: {} (gpu={})", use_vulkan ? "Vulkan" : "D3D12", gpu_pref);
  }
#elif REX_HAS_D3D12
  if (!backend_selected) {
    config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
    backend_selected = true;
    REXLOG_INFO("Graphics backend: D3D12 (gpu={})", gpu_pref);
  }
#elif REX_HAS_VULKAN
  if (!backend_selected) {
    config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
    backend_selected = true;
    REXLOG_INFO("Graphics backend: Vulkan (gpu={})", gpu_pref);
  }
#endif
  if (!backend_selected) {
    REXLOG_ERROR("No compiled graphics backend can satisfy gpu={}", gpu_pref);
    return false;
  }
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }

  // Create window. Honor the window_width/height cvars (set by the in-game
  // video settings) so a chosen resolution applies on the next launch; fall
  // back to 1280x720 when unset (0).
  int32_t cfg_w = REXCVAR_GET(window_width);
  int32_t cfg_h = REXCVAR_GET(window_height);
  uint32_t win_w = cfg_w > 0 ? static_cast<uint32_t>(cfg_w) : 1280;
  uint32_t win_h = cfg_h > 0 ? static_cast<uint32_t>(cfg_h) : 720;
  window_ = rex::ui::Window::Create(app_context(), GetName(), win_w, win_h);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->Open();

  auto* graphics_system = static_cast<rex::graphics::GraphicsSystem*>(config_.graphics.get());
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
            window_.get(), 64, [this](ImFontAtlas* atlas) { OnConfigureFonts(atlas); });
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] {
          if (debug_overlay_) {
            debug_overlay_.reset();
          } else {
            debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(),
                                                                      frame_stats_provider_);
          }
          NotifyInputActiveChanged();
        });
        rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] {
          if (console_overlay_) {
            console_overlay_.reset();
          } else {
            console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
          }
          NotifyInputActiveChanged();
        });
        rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
          if (settings_overlay_) {
            settings_overlay_.reset();
          } else {
            settings_overlay_ =
                std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_);
          }
          NotifyInputActiveChanged();
        });

        OnCreateDialogs(imgui_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  }

  return true;
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    auto* graphics_system =
        static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
      }
    }

    OnPostLaunchModule(main_thread.get());
    main_thread->Resume();

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

std::function<void(PathConfig)> ReXApp::MakePreparePathsResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ContinueInitialization(std::move(paths))) {
      app_context().QuitFromUIThread();
    }
  };
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::RequestShutdown() {
  app_context().CallInUIThreadDeferred([this]() {
    if (shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    if (window_ && window_->phase() == ui::Window::Phase::kOpen) {
      window_->RequestClose();
      return;
    }

    // A platform-native setup window may not have created the game window.
    // Keep this fallback orderly and idempotent as well.
    REXLOG_INFO("Application shutting down...");
    shutting_down_.store(true, std::memory_order_release);
#if REX_PLATFORM_MAC
    // macOS can't recover ordinary mutexes owned by a guest thread stopped via
    // asynchronous pthread cancellation. Let the native entry point finish
    // the accepted close event and end the process at the OS boundary instead
    // of destructing a runtime containing potentially orphaned locks.
    immediate_process_exit_.store(true, std::memory_order_release);
#else
    if (runtime_ && runtime_->kernel_state()) {
      runtime_->kernel_state()->TerminateTitle();
    }
#endif
    app_context().QuitFromUIThread();
  });
}

bool ReXApp::RequiresImmediateProcessExit() const {
  return immediate_process_exit_.load(std::memory_order_acquire);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
#if REX_PLATFORM_MAC
  // See RequestShutdown. RequestCloseImpl will restore native cursor/window
  // state after this callback returns; the macOS main then exits the process
  // before unsafe guest-runtime destruction begins.
  immediate_process_exit_.store(true, std::memory_order_release);
#else
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
#endif
  app_context().QuitFromUIThread();
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");

  // ImGui cleanup (reverse of setup)
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  auto* input_system = runtime_ && runtime_->input_system()
                           ? static_cast<rex::input::InputSystem*>(runtime_->input_system())
                           : nullptr;
  if (input_system) {
    // Prevent a final guest poll from recapturing the mouse while shutdown is
    // waiting for that guest thread to exit.
    input_system->NotifyInputActiveChanged(false);
  }
  // Release a guest thread waiting for a queued UI-thread transition before
  // waiting for the guest itself. A poll may have queued one while the
  // application-specific shutdown hook was running.
  app_context().ExecutePendingFunctionsFromUIThread();
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (input_system) {
    // Some quit paths (including in-game restart) do not request a native
    // window close. Detach drivers explicitly while their Window is alive.
    input_system->DetachWindow();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

void ReXApp::PersistConfig() {
  if (!config_path_.empty()) {
    rex::cvar::SaveConfig(config_path_);
  }
}

void ReXApp::NotifyInputActiveChanged() {
  if (!runtime_ || !runtime_->input_system()) {
    return;
  }
  static_cast<rex::input::InputSystem*>(runtime_->input_system())
      ->NotifyInputActiveChanged(IsInputActive());
}

bool ReXApp::IsEffectiveInputActive() const {
  // Don't feed input to an unfocused window (e.g. a second local instance when
  // testing online) -- otherwise one gamepad drives every instance.
  if (window_ && !window_->HasFocus()) {
    return false;
  }
  if (!IsInputActive()) {
    return false;
  }
  if (!debug_overlay_ && !console_overlay_ && !settings_overlay_) {
    return true;
  }
  return imgui_drawer_ && !imgui_drawer_->GetIO().WantCaptureMouse;
}

}  // namespace rex
