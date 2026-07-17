#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rex::graphics::metal {

struct ProbeTextureSlot {
  const uint8_t* rgba = nullptr;
  void* metal_texture = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t array_length = 1;
  size_t bytes_per_row = 0;
  size_t bytes_per_image = 0;
};

struct ProbeSamplerSlot {
  uint8_t min_linear = 0;
  uint8_t mag_linear = 0;
  uint8_t mip_linear = 0;
  uint8_t address_mode_s = 2;
  uint8_t address_mode_t = 2;
  uint8_t address_mode_r = 2;
  uint8_t max_anisotropy = 1;
};

struct ProbeIndexBuffer {
  const void* data = nullptr;
  void* metal_buffer = nullptr;
  size_t size = 0;
  size_t offset = 0;
  uint8_t index_size = 0;
};

enum class ProbeCullMode : uint8_t {
  kNone,
  kFront,
  kBack,
};

struct ProbeRasterizationState {
  double viewport_x = 0.0;
  double viewport_y = 0.0;
  double viewport_width = 0.0;
  double viewport_height = 0.0;
  double viewport_z_min = 0.0;
  double viewport_z_max = 1.0;
  uint32_t scissor_x = 0;
  uint32_t scissor_y = 0;
  uint32_t scissor_width = 0;
  uint32_t scissor_height = 0;
  double blend_red = 0.0;
  double blend_green = 0.0;
  double blend_blue = 0.0;
  double blend_alpha = 0.0;
  ProbeCullMode cull_mode = ProbeCullMode::kNone;
  bool front_face_clockwise = false;
};

struct ProbeStencilFaceState {
  // xenos::CompareFunction and xenos::StencilOp values map directly to the
  // corresponding Metal enum values (0 through 7).
  uint8_t compare_function = 7;
  uint8_t stencil_failure_operation = 0;
  uint8_t depth_failure_operation = 0;
  uint8_t depth_stencil_pass_operation = 0;
  uint8_t read_mask = 0xFF;
  uint8_t write_mask = 0xFF;
  uint8_t reference = 0;
};

struct ProbeDepthStencilState {
  bool depth_test_enabled = false;
  bool depth_write_enabled = false;
  uint8_t depth_compare_function = 7;
  bool stencil_test_enabled = false;
  ProbeStencilFaceState front;
  ProbeStencilFaceState back;
};

struct ProbeColorTargetState {
  // Xenos RB_COLOR_MASK nibble order: R, G, B, A in bits 0...3.
  uint8_t write_mask = 0xF;
  // Normalized RB_BLENDCONTROL value (reserved bits removed).
  uint32_t blend_control = 0x00010001;
};

struct ProbeTiledResolveTarget {
  // Existing caller-owned resident id<MTLBuffer>. Submitted Metal command
  // buffers retain it for their in-flight lifetime. It must use shared storage
  // if the caller intends to consume the resolved bytes on the CPU.
  void* metal_buffer = nullptr;
  // Byte offset of the start of the tiled surface in metal_buffer.
  size_t buffer_offset = 0;
  uint32_t pitch = 0;
  uint32_t height = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  // xenos::Endian128 values supported by 32bpp resolves: kNone, k8in16,
  // k8in32, and k16in32 (0 through 3).
  uint32_t endian = 0;
  // Optional no-copy MTLBuffer alias of guest physical memory. If supplied,
  // the resolve command mirrors a resident-buffer byte range into it after the
  // tiled compute pass, preserving GPU ordering without a CPU memcpy.
  void* guest_memory_metal_buffer = nullptr;
  uint32_t guest_memory_copy_source_offset = 0;
  uint32_t guest_memory_copy_destination_offset = 0;
  uint32_t guest_memory_copy_length = 0;
  // Optional notification for a deferred resolve that fails after this
  // function has returned success. The callback is invoked when the owning
  // command buffer is consumed, before its retained resources are released.
  void (*async_failure_callback)(void* context, uint32_t start, uint32_t length) = nullptr;
  void* async_failure_callback_context = nullptr;
  uint32_t async_failure_start = 0;
  uint32_t async_failure_length = 0;
  // Optional caller-owned BGRA8 texture receiving the untiled resolved color
  // before any later render-target clear. The destination origin defaults to
  // the top-left and the copied size is resolve_width x resolve_height.
  void* presentation_snapshot_texture = nullptr;
  uint32_t presentation_snapshot_x = 0;
  uint32_t presentation_snapshot_y = 0;
};

struct PipelineProbeUploadStats {
  uint64_t buffer_allocation_count = 0;
  uint64_t buffer_allocation_bytes = 0;
  uint64_t suballocation_count = 0;
  uint64_t suballocation_bytes = 0;
};

void* CreateMslLibrary(void* metal_device, const std::string& source, std::string* error_out);
void ReleaseMslLibrary(void* metal_library);
bool ValidateMslSource(void* metal_device, const std::string& source, std::string* error_out);
void* CreateRenderPipelineState(void* metal_device, void* vertex_library, void* fragment_library,
                                std::string* error_out,
                                const ProbeColorTargetState* color_target_state = nullptr);
void ReleaseRenderPipelineState(void* pipeline_state);
void* CreatePipelineProbeContext(void* metal_device, std::string* error_out);
void* CreatePipelineProbeContext(void* metal_device, void* metal_command_queue,
                                 std::string* error_out);
void* CreateHostRenderTargetContext(void* metal_device, std::string* error_out);
void* CreateHostRenderTargetContext(void* metal_device, void* metal_command_queue,
                                    std::string* error_out);
// Makes destination_context use source_context's persistent depth/stencil
// target while retaining its own color target. Both contexts must use the same
// Metal device and command queue. Existing destination work is drained before
// ownership changes; later switches between the contexts finalize the previous
// open draw batch so shared depth/stencil accesses stay in guest draw order.
bool SharePipelineProbeDepthStencilTarget(void* destination_context, void* source_context,
                                          std::string* error_out);
void* CreatePipelineProbeSnapshotTexture(void* metal_device, uint32_t width, uint32_t height,
                                         std::string* error_out);
void ReleasePipelineProbeSnapshotTexture(void* snapshot_texture);
// Enqueues and commits a full rectangular texture copy on the supplied queue.
// The source must be complete before a different queue is used.
bool QueuePipelineProbeSnapshotCopy(void* metal_command_queue, void* source_texture,
                                    void* destination_texture, uint32_t width, uint32_t height,
                                    std::string* error_out);
void ResetPipelineProbeContext(void* context);
void ReleasePipelineProbeContext(void* context);
// Ends and commits the context's currently open draw command buffer without
// waiting for GPU completion. Used to place an ordered shared-memory upload
// after every draw encoded before the upload request.
bool FinalizePipelineProbeContext(void* context, std::string* error_out);
// Waits for all render submissions currently owned by a persistent context.
// Normal Metal command buffers retain their encoded resources, so callers may
// release externally supplied Metal objects after submission. Callers must not
// mutate those resources until this, Read, Clear, resize, or release drains the
// context.
bool WaitPipelineProbeContext(void* context, std::string* error_out,
                              uint32_t* waited_submission_count_out = nullptr);
// Counts encoded draw submissions, including draws in the currently open
// encoder and metadata for committed command buffers (not command-buffer count).
uint32_t GetPipelineProbeContextPendingSubmissionCount(void* context);
// Returns cumulative statistics for the reusable per-command-buffer upload
// arenas used by persistent draws. Primarily useful for diagnostics and tests.
bool GetPipelineProbeContextUploadStats(void* context, PipelineProbeUploadStats* stats_out);
bool ClearPipelineProbeContext(void* context, uint32_t width, uint32_t height, double red,
                               double green, double blue, double alpha, std::string* error_out);
bool ClearPipelineProbeContextRect(void* context, uint32_t width, uint32_t height, uint32_t x,
                                   uint32_t y, uint32_t clear_width, uint32_t clear_height,
                                   double red, double green, double blue, double alpha,
                                   std::string* error_out);
// Encodes a rectangular clear in the context's ordered asynchronous batch.
// A later read, wait, resize, clear, or release drains it.
bool QueuePipelineProbeContextClearRect(void* context, uint32_t width, uint32_t height, uint32_t x,
                                        uint32_t y, uint32_t clear_width, uint32_t clear_height,
                                        double red, double green, double blue, double alpha,
                                        std::string* error_out);
bool RenderPipelineProbeToContext(
    void* context, void* pipeline_state, const void* system_constants, size_t system_constants_size,
    const void* float_constants, size_t float_constants_size, const void* fetch_constants,
    size_t fetch_constants_size, void* shared_memory, size_t shared_memory_size,
    void* shared_memory_metal_buffer, const ProbeTextureSlot* vertex_textures,
    size_t vertex_texture_count, size_t vertex_sampler_count,
    const ProbeTextureSlot* fragment_textures, size_t fragment_texture_count,
    size_t fragment_sampler_count, uint32_t primitive_type, uint32_t vertex_count, uint32_t width,
    uint32_t height, std::string* error_out, uint32_t vertex_shared_memory_buffer_index = 2,
    uint32_t vertex_float_constants_buffer_index = UINT32_MAX,
    uint32_t vertex_fetch_constants_buffer_index = 1,
    const void* fragment_float_constants = nullptr, size_t fragment_float_constants_size = 0,
    uint32_t fragment_float_constants_buffer_index = 1,
    uint32_t fragment_fetch_constants_buffer_index = 2,
    const ProbeSamplerSlot* vertex_samplers = nullptr,
    const ProbeSamplerSlot* fragment_samplers = nullptr, const void* vertex_data = nullptr,
    size_t vertex_data_size = 0, uint32_t vertex_data_buffer_index = UINT32_MAX,
    const void* bool_loop_constants = nullptr, size_t bool_loop_constants_size = 0,
    uint32_t vertex_bool_loop_constants_buffer_index = UINT32_MAX,
    uint32_t fragment_bool_loop_constants_buffer_index = UINT32_MAX,
    const ProbeIndexBuffer* index_buffer = nullptr,
    const ProbeRasterizationState* rasterization_state = nullptr,
    const ProbeDepthStencilState* depth_stencil_state = nullptr);
bool ReadPipelineProbeContext(void* context, uint32_t width, uint32_t height,
                              std::vector<uint8_t>& bgra_out, std::string* error_out);
// Reads a tightly packed BGRA rectangle. Like the full read, this is a fence:
// pending submissions are drained before metadata and bounds are validated.
bool ReadPipelineProbeContextRect(void* context, uint32_t width, uint32_t height, uint32_t x,
                                  uint32_t y, uint32_t read_width, uint32_t read_height,
                                  std::vector<uint8_t>& bgra_out, std::string* error_out);
// Resolves a BGRA8 rectangle from the persistent render texture into an
// externally owned MTLBuffer using the exact Xenos 32bpp tiled layout. Pending
// render work, texture-to-staging blit, compute conversion, and optional guest
// mirror copy are ordered on the context's queue. If bgra_out is null, valid
// work is queued asynchronously and counted by
// GetPipelineProbeContextPendingSubmissionCount; callers must later wait the
// context before treating completion as successful. If bgra_out is non-null,
// this remains a fence and returns the rectangle as tightly packed raw BGRA.
// Invalid metadata is also a fence so earlier work is never abandoned.
bool ResolvePipelineProbeContextToXenosTiled(void* context, uint32_t width, uint32_t height,
                                             uint32_t source_x, uint32_t source_y,
                                             uint32_t resolve_width, uint32_t resolve_height,
                                             const ProbeTiledResolveTarget& destination,
                                             std::vector<uint8_t>* bgra_out,
                                             std::string* error_out);
bool RenderPipelineProbe(
    void* metal_device, void* pipeline_state, const void* system_constants,
    size_t system_constants_size, const void* float_constants, size_t float_constants_size,
    const void* fetch_constants, size_t fetch_constants_size, void* shared_memory,
    size_t shared_memory_size, void* shared_memory_metal_buffer,
    const ProbeTextureSlot* vertex_textures, size_t vertex_texture_count,
    size_t vertex_sampler_count, const ProbeTextureSlot* fragment_textures,
    size_t fragment_texture_count, size_t fragment_sampler_count, uint32_t primitive_type,
    uint32_t vertex_count, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra_out,
    std::string* error_out, uint32_t vertex_shared_memory_buffer_index = 2,
    uint32_t vertex_float_constants_buffer_index = UINT32_MAX,
    uint32_t vertex_fetch_constants_buffer_index = 1, const uint8_t* initial_bgra = nullptr,
    size_t initial_bgra_row_pitch = 0, const void* fragment_float_constants = nullptr,
    size_t fragment_float_constants_size = 0, uint32_t fragment_float_constants_buffer_index = 1,
    uint32_t fragment_fetch_constants_buffer_index = 2,
    const ProbeSamplerSlot* vertex_samplers = nullptr,
    const ProbeSamplerSlot* fragment_samplers = nullptr, const void* vertex_data = nullptr,
    size_t vertex_data_size = 0, uint32_t vertex_data_buffer_index = UINT32_MAX,
    const void* bool_loop_constants = nullptr, size_t bool_loop_constants_size = 0,
    uint32_t vertex_bool_loop_constants_buffer_index = UINT32_MAX,
    uint32_t fragment_bool_loop_constants_buffer_index = UINT32_MAX,
    const ProbeIndexBuffer* index_buffer = nullptr,
    const ProbeRasterizationState* rasterization_state = nullptr,
    const ProbeDepthStencilState* depth_stencil_state = nullptr);

}  // namespace rex::graphics::metal
