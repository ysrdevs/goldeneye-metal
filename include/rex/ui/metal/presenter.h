#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/graphics/metal/profile.h>
#include <rex/ui/presenter.h>

namespace rex::ui::metal {

// Metal-specific UI drawing context passed to UIDrawers while the presenter's
// render encoder is active. Immediate drawers append their commands after the
// guest image and before the drawable is presented.
class MetalUIDrawContext final : public UIDrawContext {
 public:
  void* render_command_encoder() const { return render_command_encoder_; }

 private:
  friend class MetalPresenter;

  MetalUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height, void* render_command_encoder)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        render_command_encoder_(render_command_encoder) {}

  void* render_command_encoder_ = nullptr;
};

class MetalPresenter final : public Presenter {
 public:
  // The callback refreshing a mailbox image must submit all writes to
  // command_queue() before returning. The presenter orders the mailbox write
  // and draw on this queue, and currently waits for the write to complete
  // before returning so a renderer queue may safely reuse its source texture.
  class MetalGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    // Borrowed id<MTLTexture>. The texture is BGRA8Unorm, private, and supports
    // shader reads, shader writes and render-target writes.
    void* writable_texture() const { return writable_texture_; }

    // Borrowed id<MTLCommandQueue>. Direct output is valid only when all writes
    // to writable_texture() are submitted with normal retaining command
    // buffers on this queue before the refresher returns. Unretained-reference
    // command buffers are not supported.
    void* command_queue() const { return command_queue_; }

    // Selects the mailbox texture for this refresh. The producer is responsible
    // for routing asynchronous command-buffer failures to host GPU loss. If
    // this is never called, presentation safely falls back to the existing
    // CPU-uploaded frame.
    void SetDirectOutputValid(bool direct_valid = true, uint32_t guest_swizzle = 0x688) {
      direct_valid_ = direct_valid && writable_texture_;
      guest_swizzle_ = direct_valid_ ? guest_swizzle : 0x688;
    }

    bool direct_valid() const { return direct_valid_; }
    uint32_t guest_swizzle() const { return guest_swizzle_; }

   private:
    friend class MetalPresenter;

    MetalGuestOutputRefreshContext(bool& is_8bpc_out_ref, void* writable_texture,
                                   void* command_queue)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          writable_texture_(writable_texture),
          command_queue_(command_queue) {}

    void* writable_texture_ = nullptr;
    void* command_queue_ = nullptr;
    bool direct_valid_ = false;
    uint32_t guest_swizzle_ = 0x688;
  };

  static std::unique_ptr<MetalPresenter> Create(void* metal_device,
                                                HostGpuLossCallback host_gpu_loss_callback);

  ~MetalPresenter() override;

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(RawImage& image_out) override;
  void UpdateGuestFrontbuffer(uint32_t width, uint32_t height, const void* pixels,
                              size_t row_pitch);
  void UpdateGuestFrontbuffer(uint32_t width, uint32_t height, std::vector<uint8_t>&& packed_bgra);

 private:
  MetalPresenter(void* metal_device, HostGpuLossCallback host_gpu_loss_callback);

  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;
  bool EnsureGuestPipeline();
  bool EnsureMetalFxResources(uint32_t input_width, uint32_t input_height,
                              uint32_t output_width, uint32_t output_height);
  void ReleaseMetalFxResources();
  void FinalizeGuestFrameLocked();
  uint64_t RecordGuestFrameArrivalLocked();
  void BuildGuestFpsOverlayLocked(uint32_t width, uint32_t height);
  void EndProfiledPresentAttempt(bool drawable_nil, uint64_t present_commit_ns = 0);
  void ReportProfileWindowLocked();

  struct GuestOutputMailboxTexture {
    void* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool direct_valid = false;
    uint32_t guest_swizzle = 0x688;
    uint64_t guest_frame_id = 0;
  };

  // Overlay pixels live in shared textures. Multiple presentation command
  // buffers may read the current texture concurrently, so a completion-
  // tracked ring is used to ensure CPU uploads only target an idle texture.
  struct FpsOverlayTexture {
    void* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;
    std::shared_ptr<std::atomic<uint32_t>> in_flight =
        std::make_shared<std::atomic<uint32_t>>(0);
  };

  void* metal_device_ = nullptr;
  void* command_queue_ = nullptr;
  void* metal_layer_ = nullptr;
  std::array<GuestOutputMailboxTexture, kGuestOutputMailboxSize> guest_output_mailbox_textures_;
  void* guest_texture_ = nullptr;
  static constexpr size_t kFpsOverlayTextureCount = 3;
  std::array<FpsOverlayTexture, kFpsOverlayTextureCount> fps_overlay_textures_;
  size_t fps_overlay_current_texture_ = kFpsOverlayTextureCount;
  void* guest_pipeline_state_ = nullptr;
  void* fps_pipeline_state_ = nullptr;
  void* metalfx_spatial_scaler_ = nullptr;
  void* metalfx_input_texture_ = nullptr;
  void* metalfx_output_texture_ = nullptr;
  uint32_t metalfx_input_width_ = 0;
  uint32_t metalfx_input_height_ = 0;
  uint32_t metalfx_output_width_ = 0;
  uint32_t metalfx_output_height_ = 0;
  uint32_t metalfx_failed_input_width_ = 0;
  uint32_t metalfx_failed_input_height_ = 0;
  uint32_t metalfx_failed_output_width_ = 0;
  uint32_t metalfx_failed_output_height_ = 0;
  bool metalfx_supported_ = false;
  uint32_t guest_texture_width_ = 0;
  uint32_t guest_texture_height_ = 0;
  std::mutex guest_frame_mutex_;
  std::vector<uint8_t> guest_frame_bgra_;
  uint32_t guest_frame_width_ = 0;
  uint32_t guest_frame_height_ = 0;
  uint64_t guest_frame_generation_ = 0;
  uint64_t guest_frame_id_ = 0;
  uint64_t guest_frame_refresh_generation_ = 0;
  uint64_t guest_texture_generation_ = 0;
  std::vector<uint8_t> fps_overlay_bgra_;
  uint32_t fps_overlay_width_ = 0;
  uint32_t fps_overlay_height_ = 0;
  uint64_t fps_overlay_generation_ = 0;
  std::array<char, 128> fps_overlay_label_ = {};
  uint32_t fps_overlay_scale_ = 0;
  bool fps_overlay_detailed_ = false;
  bool fps_performance_snapshot_valid_ = false;
  uint64_t fps_performance_snapshot_tick_ = 0;
  double fps_cached_performance_fps_ = 0.0;
  double fps_cached_frame_time_ms_ = 0.0;
  double fps_cached_gpu_time_ms_ = 0.0;
  double fps_cached_one_percent_low_fps_ = 0.0;
  uint64_t fps_cached_compile_hitches_ = 0;
  uint64_t guest_fps_sample_start_tick_ = 0;
  uint32_t guest_fps_sample_frame_count_ = 0;
  double guest_fps_ = 0.0;
  std::mutex profile_mutex_;
  graphics::metal::profiling::PresenterProfileWindow profile_window_;
  uint64_t profiled_present_attempt_count_ = 0;
  uint64_t profile_last_source_hash_ = 0;
  uint32_t profile_last_source_width_ = 0;
  uint32_t profile_last_source_height_ = 0;
  bool profile_last_source_valid_ = false;
  bool profile_enabled_ = graphics::metal::profiling::IsEnabled();
  std::atomic<int32_t> last_reported_fxaa_quality_{-1};
  std::atomic<int32_t> last_reported_postfx_enabled_{-1};
  std::atomic<int32_t> last_reported_requested_scaler_{-1};
  std::atomic<int32_t> last_reported_active_scaler_{-1};
};

}  // namespace rex::ui::metal
