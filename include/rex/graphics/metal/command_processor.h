#pragma once

#include <filesystem>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/metal/draw_renderer.h>
#include <rex/graphics/metal/graphics_system.h>
#include <rex/graphics/metal/msl_compiler.h>
#include <rex/graphics/metal/profile.h>
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
  MetalCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~MetalCommandProcessor() override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  void WriteRegister(uint32_t index, uint32_t value) override;
  bool WriteGpuCompletionMemory(uint32_t address, const void* data, size_t length) override;
  bool FlushGpuCompletionMemoryWrites() override;
  void PrepareForWait() override;
  void OnPrimaryBufferEnd() override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  void OnWaitRegMemComplete(bool is_memory, uint32_t poll_address, uint32_t reference,
                            uint32_t mask, uint32_t operation, uint32_t wait, uint32_t last_value,
                            uint64_t poll_count, uint64_t duration_ns, bool matched,
                            bool timed_out) override;

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
  void* CreateCachedRenderPipelineState(void* vertex_library, void* fragment_library,
                                        std::string* error_out,
                                        const ProbeColorTargetState* color_target_state = nullptr,
                                        uint32_t sample_count = 1);
  void FinalizePipelineArchiveSerializationLocked();
  void HandlePipelineArchiveSerializationResultLocked(bool succeeded, uint64_t archive_size,
                                                      uint64_t elapsed_ns,
                                                      std::string serialize_error);
  void SerializePipelineArchiveIfNeeded(bool force);
  void ShutdownPipelineArchive();
  void* EnsureRenderPipeline(MetalShader& vertex_shader, MetalShader& pixel_shader,
                             uint32_t rt_index = 0, uint32_t color_write_mask = 0xF);
  void UpdateMinimalSystemConstants(xenos::PrimitiveType prim_type,
                                    const IndexBufferInfo* index_buffer_info);
  void UpdateGuestConstantBuffers();
  void TrackPositionRegisterWrite(uint32_t index);
  void BeginPositionRegisterSnapshot();
  bool EnsureVertexFetchRangesResident(const MetalShader& vertex_shader);
  bool TryRenderPipelineProbe(
      MetalShader& vertex_shader, MetalShader* pixel_shader, void* pipeline_state,
      xenos::PrimitiveType prim_type, uint32_t index_count, bool host_render_target_debug = false,
      const PrimitiveProcessor::ProcessingResult* primitive_processing_result = nullptr);
  struct HostRenderTarget;
  struct HostDepthStencilTarget;
  // Drains every persistent context except ordered_context. Work on the
  // excluded context may be followed by a same-queue operation that provides
  // the required ordering and completion fence itself.
  bool WaitForPipelineProbeSubmissions(const char* reason, void* ordered_context = nullptr);
  // Commits all currently open persistent render command buffers without a CPU
  // wait. The shared-memory upload subsequently committed on the common Metal
  // queue is therefore ordered after prior draws and before later draws.
  bool FinalizePipelineProbeSubmissions();
  void ReportProfileWindow();
  void* GetActiveHostRenderTargetContext() const {
    return host_render_target_context_override_ ? host_render_target_context_override_
                                                : host_render_target_context_;
  }
  bool RefreshPipelineProbeBacking(uint32_t width, uint32_t height);
  bool RefreshHostRenderTargetBacking(uint32_t width, uint32_t height);
  bool EnsureEdramBgraBacking();
  bool DumpHostRenderTargetToEdram(const HostRenderTarget& target);
  bool ResolveEdramToBgra(const draw_util::ResolveInfo& resolve_info, uint32_t width,
                          uint32_t height, std::vector<uint8_t>& bgra_out);
  bool PublishPendingGpuTiledResolveRangesToGuest();
  uint64_t GetHostRenderTargetKey(uint32_t rt_index) const;
  HostRenderTarget* FindHostRenderTarget(uint32_t rt_index);
  HostRenderTarget* FindHostRenderTargetForResolve(uint32_t rt_index, uint32_t color_base,
                                                   uint32_t color_format,
                                                   uint32_t min_surface_pitch,
                                                   xenos::MsaaSamples msaa_samples);
  HostDepthStencilTarget* EnsureHostDepthStencilTarget(uint32_t depth_info, uint32_t surface_info);
  HostRenderTarget* EnsureHostRenderTarget(uint32_t rt_index);
  void* EnsureFullscreenPixelPipeline(MetalShader& pixel_shader, uint32_t sample_count = 1);
  void* EnsureHostPixelPipeline(MetalShader& pixel_shader, uint32_t sample_count = 1);
  void* EnsureHostFallbackPixelPipeline(uint32_t sample_count = 1);
  void* EnsureHostVertexColorPixelPipeline(uint32_t sample_count = 1);
  void* EnsureSolidColorPipeline(MetalShader& vertex_shader, uint32_t sample_count = 1);
  void* EnsureDepthOnlyPipeline(MetalShader& vertex_shader, MetalShader* pixel_shader);
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
  void UpdateExactResolvedSurfaceCache(uint32_t dest_base, uint32_t pitch, uint32_t surface_height,
                                       const std::vector<uint8_t>& bgra, uint32_t width,
                                       uint32_t source_height, uint32_t source_x, uint32_t source_y,
                                       uint32_t dest_x, uint32_t dest_y, uint32_t write_width,
                                       uint32_t write_height, xenos::Endian128 dest_endian);
  bool EnsureExactResolvedSurfaceSnapshot(uint32_t width, uint32_t height);
  void UpdateExactResolvedSurfaceGpuCache(uint32_t dest_base, uint32_t pitch,
                                          uint32_t surface_height, uint32_t snapshot_width,
                                          uint32_t snapshot_height, xenos::Endian128 dest_endian);
  void ReleaseExactResolvedSurfaceSnapshot();
  static void ExactResolvedSurfaceWatchCallback(
      const std::unique_lock<std::recursive_mutex>& global_lock, void* context,
      uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu);
  void InvalidateExactResolvedSurfaceCache(uint32_t base_physical, uint32_t length);
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
  void* host_vertex_color_pixel_fragment_library_ = nullptr;
  void* dummy_fragment_library_ = nullptr;
  void* solid_fragment_library_ = nullptr;
  void* pipeline_probe_context_ = nullptr;
  void* host_pixel_probe_context_ = nullptr;
  // The standalone debug context remains immutable so a temporary per-target
  // routing override can never hide it from a global synchronization pass.
  void* host_render_target_context_ = nullptr;
  void* host_render_target_context_override_ = nullptr;
  std::unordered_map<uint64_t, void*> render_pipeline_states_;
  std::unordered_map<uint64_t, void*> fullscreen_pixel_pipeline_states_;
  std::unordered_map<uint64_t, void*> host_pixel_pipeline_states_;
  std::unordered_map<uint32_t, void*> host_fallback_pixel_pipeline_states_;
  std::unordered_map<uint32_t, void*> host_vertex_color_pixel_pipeline_states_;
  std::unordered_map<uint64_t, void*> solid_color_pipeline_states_;
  std::unordered_map<uint64_t, void*> memexport_pipeline_states_;
  void* pipeline_binary_archive_ = nullptr;
  std::filesystem::path pipeline_binary_archive_path_;
  bool pipeline_binary_archive_dirty_ = false;
  uint64_t pipeline_archive_hit_count_ = 0;
  uint64_t pipeline_archive_miss_count_ = 0;
  uint64_t pipeline_archive_busy_bypass_count_ = 0;
  uint64_t pipeline_archive_update_failure_count_ = 0;
  uint64_t pipeline_archive_serialization_count_ = 0;
  uint64_t pipeline_archive_serialization_failure_count_ = 0;
  uint64_t pipeline_archive_serialization_ns_ = 0;
  uint64_t pipeline_archive_serialization_max_ns_ = 0;
  uint32_t pipeline_archive_consecutive_serialization_failures_ = 0;
  uint64_t pipeline_archive_lookup_ns_ = 0;
  uint64_t pipeline_archive_add_ns_ = 0;
  uint64_t pipeline_build_ns_ = 0;
  uint64_t pipeline_archive_swap_count_ = 0;
  uint64_t pipeline_archive_first_dirty_swap_ = 0;
  uint64_t pipeline_archive_last_update_swap_ = 0;
  uint64_t pipeline_archive_next_serialize_swap_ = 0;
  std::mutex pipeline_archive_mutex_;
  std::thread pipeline_archive_save_thread_;
  bool pipeline_archive_save_in_flight_ = false;
  bool pipeline_archive_save_result_ready_ = false;
  bool pipeline_archive_save_result_succeeded_ = false;
  uint64_t pipeline_archive_save_result_size_ = 0;
  uint64_t pipeline_archive_save_result_elapsed_ns_ = 0;
  std::string pipeline_archive_save_result_error_;
  uint64_t msl_library_compile_count_ = 0;
  uint64_t msl_library_compile_failure_count_ = 0;
  uint64_t msl_library_compile_ns_ = 0;
  uint64_t msl_library_compile_max_ns_ = 0;
  std::unordered_set<uint64_t> probed_pipeline_keys_;
  std::unordered_set<uint64_t> disabled_host_pixel_shader_hashes_;
  std::unordered_map<uint64_t, uint32_t> host_pixel_shader_draws_this_swap_;
  SpirvShaderTranslator::SystemConstants system_constants_ = {};
  std::array<uint32_t, 32 * 6> fetch_constants_ = {};
  std::array<uint32_t, 8 + 32> bool_loop_constants_ = {};
  struct PositionRegisterRollback {
    uint32_t index;
    uint32_t value;
  };
  static constexpr size_t kPositionRegisterRollbackWordCount =
      (RegisterFile::kRegisterCount + 63) / 64;
  std::array<uint64_t, kPositionRegisterRollbackWordCount> last_position_register_rollback_bits_ =
      {};
  std::vector<PositionRegisterRollback> last_position_register_rollbacks_;
  // Reused by the per-draw native Metal route. RenderPipelineProbeToContext
  // snapshots CPU argument bytes and retains Metal texture objects before it
  // returns, so these containers may be safely recycled by the next draw.
  std::vector<std::vector<uint8_t>> vertex_texture_storage_scratch_;
  std::vector<std::vector<uint8_t>> fragment_texture_storage_scratch_;
  std::vector<ProbeTextureSlot> vertex_texture_slots_scratch_;
  std::vector<ProbeTextureSlot> fragment_texture_slots_scratch_;
  std::vector<ProbeSamplerSlot> vertex_sampler_slots_scratch_;
  std::vector<ProbeSamplerSlot> fragment_sampler_slots_scratch_;
  std::vector<uint32_t> vertex_float_constants_scratch_;
  std::vector<uint32_t> fragment_float_constants_scratch_;
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
  // Unlike the score-based diagnostic frame retention above, this is an exact
  // mirror of a complete top-origin tiled resolve write. A persistent shared-
  // memory watch invalidates it on overlapping CPU or GPU writes.
  struct ExactResolvedSurface {
    std::vector<uint8_t> bgra;
    void* metal_texture = nullptr;
    // Invalidation keeps the allocations so the title's repeated partial-band
    // writes don't free and reallocate roughly 7.5 MiB before every complete
    // resolve. Only a fully republished surface may be used by swap.
    bool valid = false;
    uint32_t base = 0;
    uint32_t pitch = 0;
    uint32_t bgra_height = 0;
    uint32_t surface_height = 0;
    uint32_t tiled_extent = 0;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    bool gpu_snapshot = false;
    xenos::Endian128 endian = xenos::Endian128::kNone;
  };
  struct HostRenderTarget {
    void* context = nullptr;
    std::vector<uint8_t> bgra;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rt_index = 0;
    uint32_t color_info = 0;
    uint32_t depth_info = 0;
    uint32_t surface_info = 0;
    uint64_t depth_stencil_key = 0;
  };
  struct HostDepthStencilTarget {
    void* context = nullptr;
    uint32_t depth_info = 0;
    uint32_t surface_info = 0;
  };
  struct PendingReadbackResolveSlice {
    uint32_t copy_dest_base = 0;
    uint32_t pitch = 0;
    uint32_t source_x = 0;
    uint32_t source_y = 0;
    uint32_t dest_x = 0;
    uint32_t dest_y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    xenos::Endian128 endian = xenos::Endian128::kNone;
    std::vector<uint8_t> bgra;
    uint32_t bgra_width = 0;
    uint32_t bgra_height = 0;
  };
  struct PendingGpuCompletionWrite {
    static constexpr size_t kMaxDataLength = 16;
    uint32_t address = 0;
    uint32_t length = 0;
    std::array<uint8_t, kMaxDataLength> data = {};
  };
  std::unordered_map<uint32_t, RetainedResolvedFrame> retained_resolve_frames_by_base_;
  ExactResolvedSurface exact_resolved_surface_;
  SharedMemory::GlobalWatchHandle exact_resolved_surface_watch_ = nullptr;
  std::unordered_map<uint64_t, HostRenderTarget> host_render_targets_;
  std::unordered_map<uint64_t, HostDepthStencilTarget> host_depth_stencil_targets_;
  std::vector<PendingReadbackResolveSlice> pending_readback_resolve_slices_;
  // GPU tiled resolves stay resident until the next guest-visible completion
  // event. All ranges before that event are coalesced into one Metal blit
  // command, avoiding a large mirror command per resolve.
  std::vector<std::pair<uint32_t, uint32_t>> pending_gpu_tiled_resolve_publication_ranges_;
  std::vector<PendingGpuCompletionWrite> pending_gpu_completion_writes_;
  std::vector<uint8_t> pending_gpu_completion_staging_;
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
  bool collect_draw_route_summary_ = true;
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
  uint32_t async_probe_submissions_since_global_wait_ = 0;
  uint32_t async_probe_max_pending_submission_count_ = 0;
  uint64_t async_probe_submission_count_ = 0;
  uint64_t async_probe_wait_count_ = 0;
  uint64_t async_probe_waited_submission_count_ = 0;
  uint64_t gpu_tiled_resolve_count_ = 0;
  uint64_t gpu_tiled_resolve_pixel_count_ = 0;
  uint64_t gpu_tiled_resolve_fallback_count_ = 0;
  uint64_t gpu_publication_event_count_ = 0;
  uint64_t gpu_publication_input_range_count_ = 0;
  uint64_t gpu_publication_merged_range_count_ = 0;
  uint64_t gpu_publication_byte_count_ = 0;
  uint64_t gpu_completion_write_count_ = 0;
  uint64_t gpu_completion_batch_count_ = 0;
  uint32_t host_pixel_draws_this_swap_ = 0;
  uint32_t host_fallback_pixel_draws_this_swap_ = 0;
  uint32_t host_pixel_skipped_vertices_this_swap_ = 0;
  uint64_t latest_texture_candidate_score_ = 0;
  struct WaitRegMemProfileEntry {
    bool is_memory = false;
    uint32_t poll_address = 0;
    uint32_t reference = 0;
    uint32_t mask = 0;
    uint32_t operation = 0;
    uint32_t wait = 0;
    uint32_t last_value = 0;
    uint64_t call_count = 0;
    uint64_t poll_count = 0;
    uint64_t max_polls = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
    uint64_t unmatched_count = 0;
    uint64_t timeout_count = 0;
  };
  struct WaitRegMemProfileKey {
    bool is_memory = false;
    uint32_t poll_address = 0;
    uint32_t reference = 0;
    uint32_t mask = 0;
    uint32_t operation = 0;
    uint32_t wait = 0;

    bool operator==(const WaitRegMemProfileKey& other) const {
      return is_memory == other.is_memory && poll_address == other.poll_address &&
             reference == other.reference && mask == other.mask && operation == other.operation &&
             wait == other.wait;
    }
  };
  struct WaitRegMemProfileKeyHash {
    size_t operator()(const WaitRegMemProfileKey& key) const {
      size_t hash = key.is_memory ? size_t(0x9E3779B9u) : 0;
      auto combine = [&](uint32_t value) {
        hash ^= size_t(value) + size_t(0x9E3779B9u) + (hash << 6) + (hash >> 2);
      };
      combine(key.poll_address);
      combine(key.reference);
      combine(key.mask);
      combine(key.operation);
      combine(key.wait);
      return hash;
    }
  };
  std::unordered_map<WaitRegMemProfileKey, WaitRegMemProfileEntry, WaitRegMemProfileKeyHash>
      wait_reg_mem_profile_entries_;
  profiling::CommandProfileWindow profile_window_;
  uint64_t profiled_swap_count_ = 0;
  uint64_t profile_window_start_ns_ = 0;
  bool profile_enabled_ = profiling::IsEnabled();
  bool logged_incomplete_ = false;
  bool last_host_render_target_probe_read_ = false;
  bool last_swap_fetch_valid_ = false;
  bool last_position_registers_valid_ = false;
};

}  // namespace rex::graphics::metal
