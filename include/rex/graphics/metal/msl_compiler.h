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
};

struct ProbeColorTargetState {
  // Xenos RB_COLOR_MASK nibble order: R, G, B, A in bits 0...3.
  uint8_t write_mask = 0xF;
  // Normalized RB_BLENDCONTROL value (reserved bits removed).
  uint32_t blend_control = 0x00010001;
};

void* CreateMslLibrary(void* metal_device, const std::string& source, std::string* error_out);
void ReleaseMslLibrary(void* metal_library);
bool ValidateMslSource(void* metal_device, const std::string& source, std::string* error_out);
void* CreateRenderPipelineState(void* metal_device, void* vertex_library, void* fragment_library,
                                std::string* error_out,
                                const ProbeColorTargetState* color_target_state = nullptr);
void ReleaseRenderPipelineState(void* pipeline_state);
void* CreatePipelineProbeContext(void* metal_device, std::string* error_out);
void* CreateHostRenderTargetContext(void* metal_device, std::string* error_out);
void ResetPipelineProbeContext(void* context);
void ReleasePipelineProbeContext(void* context);
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
    const ProbeRasterizationState* rasterization_state = nullptr);
bool ReadPipelineProbeContext(void* context, uint32_t width, uint32_t height,
                              std::vector<uint8_t>& bgra_out, std::string* error_out);
// Reads a tightly packed BGRA rectangle. Like the full read, this is a fence:
// pending submissions are drained before metadata and bounds are validated.
bool ReadPipelineProbeContextRect(void* context, uint32_t width, uint32_t height, uint32_t x,
                                  uint32_t y, uint32_t read_width, uint32_t read_height,
                                  std::vector<uint8_t>& bgra_out, std::string* error_out);
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
    const ProbeRasterizationState* rasterization_state = nullptr);

}  // namespace rex::graphics::metal
