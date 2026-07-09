#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <array>
#include <vector>

namespace rex::graphics::metal {

struct MetalDrawEvent {
  uint32_t primitive_type = 0;
  uint32_t index_count = 0;
  uint32_t vertex_binding_count = 0;
  uint32_t texture_binding_count = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
};

struct MetalHostVertex {
  static constexpr uint32_t kInterpolatorCount = 16;

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
  float viewport_x = 0.0f;
  float viewport_y = 0.0f;
  float viewport_width = 1.0f;
  float viewport_height = 1.0f;
  float u = 0.0f;
  float v = 0.0f;
  float texture_weight = 0.0f;
  float _pad = 0.0f;
  std::array<std::array<float, 4>, kInterpolatorCount> interpolators = {};
  uint32_t interpolator_mask = 0;
  uint32_t _pad_interpolators[3] = {};
};

struct MetalHostTexture {
  const uint8_t* rgba = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  size_t bytes_per_row = 0;
};

class MetalDrawRenderer {
 public:
  static constexpr uint32_t kMaxDrawEventsPerFrame = 128;
  static constexpr uint32_t kMaxHostVerticesPerFrame = 8192;

  static std::unique_ptr<MetalDrawRenderer> Create(void* metal_device);
  ~MetalDrawRenderer();

  bool RenderDiagnosticFrame(uint32_t width, uint32_t height, uint32_t draw_count,
                             uint32_t swap_count, std::vector<uint8_t>& bgra_out);
  bool RenderDrawEventFrame(uint32_t width, uint32_t height, uint32_t draw_count,
                            uint32_t swap_count, const std::vector<MetalDrawEvent>& events,
                            const std::vector<MetalHostVertex>& host_vertices,
                            std::vector<uint8_t>& bgra_out,
                            const MetalHostTexture* host_texture = nullptr);

 private:
  explicit MetalDrawRenderer(void* metal_device);

  bool Initialize();

  void* metal_device_ = nullptr;
  void* command_queue_ = nullptr;
  void* pipeline_state_ = nullptr;
  void* event_pipeline_state_ = nullptr;
  void* host_triangle_pipeline_state_ = nullptr;
  void* host_textured_triangle_pipeline_state_ = nullptr;
  void* render_texture_ = nullptr;
  uint32_t render_texture_width_ = 0;
  uint32_t render_texture_height_ = 0;
};

}  // namespace rex::graphics::metal
