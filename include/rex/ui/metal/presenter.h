#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/ui/presenter.h>

namespace rex::ui::metal {

class MetalPresenter final : public Presenter {
 public:
  static std::unique_ptr<MetalPresenter> Create(void* metal_device,
                                                HostGpuLossCallback host_gpu_loss_callback);

  ~MetalPresenter() override;

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(RawImage& image_out) override;
  void UpdateGuestFrontbuffer(uint32_t width, uint32_t height, const void* pixels,
                              size_t row_pitch);

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

  void* metal_device_ = nullptr;
  void* command_queue_ = nullptr;
  void* metal_layer_ = nullptr;
  void* guest_texture_ = nullptr;
  void* guest_pipeline_state_ = nullptr;
  uint32_t guest_texture_width_ = 0;
  uint32_t guest_texture_height_ = 0;
  std::mutex guest_frame_mutex_;
  std::vector<uint8_t> guest_frame_bgra_;
  uint32_t guest_frame_width_ = 0;
  uint32_t guest_frame_height_ = 0;
};

}  // namespace rex::ui::metal
