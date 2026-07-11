#pragma once

#include <filesystem>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/metal/draw_renderer.h>
#include <rex/graphics/metal/graphics_system.h>
#include <rex/graphics/metal/shader.h>
#include <rex/graphics/metal/shared_memory.h>
#include <rex/graphics/metal/texture_cache.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/util/draw.h>
#include <rex/string/buffer.h>

namespace rex::graphics::metal {

class MetalCommandProcessor final : public CommandProcessor {
 public:
  MetalCommandProcessor(MetalGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~MetalCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;
  void ClearCaches() override;

 private:
  // Phase 0.5: per-frame lifecycle tracking. Set on the first draw of a frame so
  // texture_cache_->BeginSubmission()/BeginFrame() run once; cleared at swap.
  bool frame_open_ = false;

  MetalShader* LoadShaderFromCache(xenos::ShaderType shader_type, const uint32_t* host_address,
                                   uint32_t dword_count, uint64_t data_hash);
  uint64_t GetDefaultShaderModification(MetalShader& shader) const;
  uint64_t GetCurrentVertexShaderModification(MetalShader& shader, uint32_t interpolator_mask,
                                              bool ps_param_gen_used) const;
  uint64_t GetCurrentPixelShaderModification(MetalShader& shader, uint32_t interpolator_mask,
                                             uint32_t param_gen_pos) const;
  void GetCurrentShaderModifications(MetalShader* vertex_shader, MetalShader* pixel_shader,
                                     uint64_t& vertex_modification, uint64_t& pixel_modification,
                                     uint32_t* interpolator_mask_out = nullptr,
                                     uint32_t* ps_param_gen_pos_out = nullptr) const;
  MetalShader::MetalTranslation* GetTranslatedShader(MetalShader& shader, uint64_t modification);
  MetalShader::MetalTranslation* GetTranslatedShader(MetalShader& shader);
  bool EnsureShaderTranslated(MetalShader& shader, uint64_t modification);
  bool EnsureShaderTranslated(MetalShader& shader);
  void* EnsureRenderPipeline(MetalShader& vertex_shader, MetalShader& pixel_shader,
                             uint32_t rt_index = 0, uint32_t color_write_mask = 0xF);
  void UpdateMinimalSystemConstants(xenos::PrimitiveType prim_type,
                                    const IndexBufferInfo* index_buffer_info);
  void UpdateGuestConstantBuffers();
  bool EnsureVertexFetchRangesResident(const MetalShader& vertex_shader);
  void TryRenderPipelineProbe(
      MetalShader& vertex_shader, MetalShader& pixel_shader, void* pipeline_state,
      xenos::PrimitiveType prim_type, uint32_t index_count, bool host_render_target_debug = false,
      const PrimitiveProcessor::ProcessingResult* primitive_processing_result = nullptr);
  struct HostRenderTarget;
  bool RefreshPipelineProbeBacking(uint32_t width, uint32_t height);
  bool RefreshHostRenderTargetBacking(uint32_t width, uint32_t height);
  bool EnsureEdramBgraBacking();
  bool DumpHostRenderTargetToEdram(const HostRenderTarget& target);
  bool ResolveEdramToBgra(const draw_util::ResolveInfo& resolve_info, uint32_t width,
                          uint32_t height, std::vector<uint8_t>& bgra_out);
  uint64_t GetHostRenderTargetKey(uint32_t rt_index) const;
  HostRenderTarget* FindHostRenderTarget(uint32_t rt_index);
  HostRenderTarget* FindHostRenderTargetForResolve(uint32_t rt_index, uint32_t color_base,
                                                   uint32_t color_format,
                                                   uint32_t min_surface_pitch,
                                                   xenos::MsaaSamples msaa_samples);
  HostRenderTarget* EnsureHostRenderTarget(uint32_t rt_index);
  void* EnsureFullscreenPixelPipeline(MetalShader& pixel_shader);
  void* EnsureHostPixelPipeline(MetalShader& pixel_shader);
  void* EnsureHostFallbackPixelPipeline();
  void* EnsureHostVertexColorPixelPipeline();
  void* EnsureSolidColorPipeline(MetalShader& vertex_shader);
  bool RenderFullscreenPixelShader(MetalShader& pixel_shader, uint32_t width, uint32_t height,
                                   std::vector<uint8_t>& bgra_out,
                                   bool host_render_target_context = false);
  bool RenderHostPixelShader(MetalShader& pixel_shader,
                             const std::vector<MetalHostVertex>& host_vertices,
                             size_t host_vertex_start, size_t host_vertex_count, uint32_t width,
                             uint32_t height, std::vector<uint8_t>& bgra_out,
                             void* persistent_context_override = nullptr,
                             bool use_host_vertex_color_fragment = false);
  bool IsHostPixelProbeAllowed(MetalShader& pixel_shader) const;
  void* EnsureMemExportPipeline(MetalShader& vertex_shader);
  bool ExecuteMemExportVertexShader(MetalShader& vertex_shader, xenos::PrimitiveType prim_type,
                                    uint32_t index_count);
  bool WriteBgraToTiledResolve(uint32_t dest_base, uint32_t pitch, uint32_t height,
                               const std::vector<uint8_t>& bgra, uint32_t width,
                               uint32_t source_height);
  bool WriteBgraToTiledResolveRect(uint32_t dest_base, uint32_t pitch, uint32_t height,
                                   const std::vector<uint8_t>& bgra, uint32_t width,
                                   uint32_t source_height, uint32_t rect_x, uint32_t rect_y,
                                   uint32_t rect_width, uint32_t rect_height,
                                   xenos::Endian128 dest_endian = xenos::Endian128::kNone);
  bool WriteBgraToTiledResolveRegion(uint32_t dest_base, uint32_t pitch, uint32_t height,
                                     const std::vector<uint8_t>& bgra, uint32_t width,
                                     uint32_t source_height, uint32_t source_x, uint32_t source_y,
                                     uint32_t dest_x, uint32_t dest_y, uint32_t copy_width,
                                     uint32_t copy_height,
                                     xenos::Endian128 dest_endian = xenos::Endian128::kNone);
  bool EnsureResolvedColorBacking(uint32_t width, uint32_t height);
  bool BlitToResolvedColorBacking(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height,
                                  uint32_t rect_x, uint32_t rect_y, uint32_t rect_width,
                                  uint32_t rect_height);
  bool CompositeVisibleToResolvedColorBacking(const std::vector<uint8_t>& bgra, uint32_t width,
                                              uint32_t height);
  bool BlitAndWriteResolvedColor(uint32_t dest_base, uint32_t pitch, uint32_t height,
                                 const std::vector<uint8_t>& bgra, uint32_t width,
                                 uint32_t source_height, uint32_t rect_x, uint32_t rect_y,
                                 uint32_t rect_width, uint32_t rect_height);
  bool CompositeAndWriteResolvedColor(uint32_t dest_base, uint32_t pitch, uint32_t height,
                                      const std::vector<uint8_t>& bgra, uint32_t width,
                                      uint32_t source_height, uint32_t rect_x, uint32_t rect_y,
                                      uint32_t rect_width, uint32_t rect_height);
  bool DecodeResolvedColorBackingToRgba(uint32_t base_physical, uint32_t width, uint32_t height,
                                        std::vector<uint8_t>& rgba_out);
  void RetainResolvedFrameForBase(uint32_t base_physical, const std::vector<uint8_t>& bgra,
                                  uint32_t width, uint32_t height, const char* label);
  void InvalidateRetainedResolvedFrames(uint32_t base_physical, uint32_t length);
  bool DecodeTextureFetchToRgba(const xenos::xe_gpu_texture_fetch_t& fetch,
                                uint32_t fallback_base_physical,
                                uint32_t decode_base_override_physical,
                                std::vector<uint8_t>& rgba_out, uint32_t& width_out,
                                uint32_t& height_out);
  static bool InterpreterTextureFetchThunk(void* user_data,
                                           const ucode::TextureFetchInstruction& instr,
                                           const float* coordinates, uint32_t coordinate_count,
                                           float* rgba_out);
  bool SampleInterpreterTextureFetch(const ucode::TextureFetchInstruction& instr,
                                     const float* coordinates, uint32_t coordinate_count,
                                     float* rgba_out);
  bool DecodeSwapTextureToBgra(uint32_t fallback_frontbuffer_ptr, uint32_t fallback_width,
                               uint32_t fallback_height, std::vector<uint8_t>& bgra_out,
                               uint32_t& width_out, uint32_t& height_out);
  bool RetainTextureCandidateIfUseful(const std::vector<uint8_t>& bgra, uint32_t width,
                                      uint32_t height, const char* label);
  void LogIncompleteOnce(const char* path);

  std::unordered_map<uint64_t, std::unique_ptr<MetalShader>> shaders_;
  std::unique_ptr<MetalSharedMemory> shared_memory_;
  std::unique_ptr<MetalTextureCache> texture_cache_;
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;
  std::unique_ptr<MetalDrawRenderer> draw_renderer_;
  std::unique_ptr<PrimitiveProcessor> primitive_processor_;
  void* metal_device_ = nullptr;
  void* fullscreen_vertex_library_ = nullptr;
  void* host_pixel_vertex_library_ = nullptr;
  void* host_fallback_pixel_fragment_library_ = nullptr;
  void* host_fallback_pixel_pipeline_state_ = nullptr;
  void* host_vertex_color_pixel_fragment_library_ = nullptr;
  void* host_vertex_color_pixel_pipeline_state_ = nullptr;
  void* dummy_fragment_library_ = nullptr;
  void* solid_fragment_library_ = nullptr;
  void* pipeline_probe_context_ = nullptr;
  void* host_pixel_probe_context_ = nullptr;
  void* host_render_target_context_ = nullptr;
  std::unordered_map<uint64_t, void*> render_pipeline_states_;
  std::unordered_map<uint64_t, void*> fullscreen_pixel_pipeline_states_;
  std::unordered_map<uint64_t, void*> host_pixel_pipeline_states_;
  std::unordered_map<uint64_t, void*> solid_color_pipeline_states_;
  std::unordered_map<uint64_t, void*> memexport_pipeline_states_;
  std::unordered_set<uint64_t> probed_pipeline_keys_;
  std::unordered_set<uint64_t> disabled_host_pixel_shader_hashes_;
  std::unordered_map<uint64_t, uint32_t> host_pixel_shader_draws_this_swap_;
  SpirvShaderTranslator::SystemConstants system_constants_ = {};
  std::array<uint32_t, 512 * 4> float_constants_ = {};
  std::array<uint32_t, 32 * 6> fetch_constants_ = {};
  std::array<uint32_t, 8 + 32> bool_loop_constants_ = {};
  std::array<uint32_t, RegisterFile::kRegisterCount> last_position_registers_ = {};
  string::StringBuffer ucode_disasm_buffer_;
  std::vector<MetalDrawEvent> pending_draw_events_;
  std::vector<MetalHostVertex> pending_host_vertices_;
  std::vector<uint8_t> latest_pipeline_probe_bgra_;
  std::vector<uint8_t> latest_host_render_target_bgra_;
  std::vector<uint8_t> latest_draw_event_frame_bgra_;
  std::vector<uint8_t> latest_host_pixel_frame_bgra_;
  std::vector<uint8_t> latest_fullscreen_postprocess_bgra_;
  std::vector<uint8_t> latest_texture_candidate_bgra_;
  std::vector<uint8_t> resolved_color_bgra_;
  std::vector<uint8_t> edram_bgra_;
  std::vector<uint8_t> pending_texture_resolve_bgra_;
  std::vector<uint8_t> pending_host_texture_rgba_;
  struct RetainedResolvedFrame {
    std::vector<uint8_t> bgra;
    std::string label;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t visible_pixels = 0;
    uint64_t score = 0;
    uint32_t draw_count = 0;
  };
  struct HostRenderTarget {
    void* context = nullptr;
    std::vector<uint8_t> bgra;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rt_index = 0;
    uint32_t color_info = 0;
    uint32_t surface_info = 0;
  };
  struct PendingReadbackResolveSlice {
    uint32_t copy_dest_base = 0;
    uint32_t pitch = 0;
    uint32_t source_x = 0;
    uint32_t source_y = 0;
    uint32_t dest_y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    xenos::Endian128 endian = xenos::Endian128::kNone;
    std::vector<uint8_t> bgra;
    uint32_t bgra_width = 0;
    uint32_t bgra_height = 0;
  };
  std::unordered_map<uint32_t, RetainedResolvedFrame> retained_resolve_frames_by_base_;
  std::unordered_map<uint64_t, HostRenderTarget> host_render_targets_;
  std::vector<PendingReadbackResolveSlice> pending_readback_resolve_slices_;
  uint32_t latest_pipeline_probe_width_ = 0;
  uint32_t latest_pipeline_probe_height_ = 0;
  uint32_t latest_host_render_target_width_ = 0;
  uint32_t latest_host_render_target_height_ = 0;
  uint32_t latest_draw_event_frame_width_ = 0;
  uint32_t latest_draw_event_frame_height_ = 0;
  uint32_t latest_host_pixel_frame_width_ = 0;
  uint32_t latest_host_pixel_frame_height_ = 0;
  uint32_t latest_host_pixel_frame_draw_count_ = 0;
  bool latest_host_pixel_frame_from_fallback_ = false;
  uint32_t latest_fullscreen_postprocess_width_ = 0;
  uint32_t latest_fullscreen_postprocess_height_ = 0;
  uint32_t latest_fullscreen_postprocess_draw_count_ = 0;
  uint32_t latest_texture_candidate_width_ = 0;
  uint32_t latest_texture_candidate_height_ = 0;
  uint32_t pending_texture_resolve_width_ = 0;
  uint32_t pending_texture_resolve_height_ = 0;
  uint32_t pending_host_texture_width_ = 0;
  uint32_t pending_host_texture_height_ = 0;
  uint32_t resolved_color_width_ = 0;
  uint32_t resolved_color_height_ = 0;
  MetalShader* last_position_vertex_shader_ = nullptr;
  Shader::HostVertexShaderType current_host_vertex_shader_type_ =
      Shader::HostVertexShaderType::kVertex;
  uint32_t last_copy_dest_base_ = 0;
  xenos::xe_gpu_texture_fetch_t last_swap_fetch_ = {};
  uint32_t last_swap_frontbuffer_ptr_ = 0;
  uint32_t last_swap_frontbuffer_width_ = 0;
  uint32_t last_swap_frontbuffer_height_ = 0;
  uint32_t fallback_output_width_ = 1280;
  uint32_t fallback_output_height_ = 720;
  uint32_t translated_shader_count_ = 0;
  uint32_t failed_shader_translation_count_ = 0;
  uint32_t draw_count_ = 0;
  uint32_t draw_calls_this_swap_ = 0;
  uint32_t color_depth_draws_this_swap_ = 0;
  uint32_t color_target_candidate_draws_this_swap_ = 0;
  uint32_t register_color_candidate_draws_this_swap_ = 0;
  uint32_t register_color_unrouted_draws_this_swap_ = 0;
  uint32_t owned_rt_routed_draws_this_swap_ = 0;
  uint32_t owned_rt_routed_targets_this_swap_ = 0;
  uint32_t diagnostic_frame_count_ = 0;
  uint32_t pipeline_probe_success_count_ = 0;
  uint32_t pipeline_probe_failure_count_ = 0;
  uint32_t pipeline_probe_draws_this_swap_ = 0;
  uint32_t pipeline_probe_skipped_this_swap_ = 0;
  uint32_t host_pixel_draws_this_swap_ = 0;
  uint32_t host_fallback_pixel_draws_this_swap_ = 0;
  uint32_t host_pixel_skipped_vertices_this_swap_ = 0;
  uint64_t latest_texture_candidate_score_ = 0;
  bool logged_incomplete_ = false;
  bool last_host_render_target_probe_read_ = false;
  bool last_swap_fetch_valid_ = false;
  bool last_position_registers_valid_ = false;
};

}  // namespace rex::graphics::metal
