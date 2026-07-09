#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <rex/assert.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace rex::graphics {

class ShaderInterpreter {
 public:
  ShaderInterpreter(const RegisterFile& register_file, const memory::Memory& memory)
      : register_file_(register_file), memory_(memory) {}

  class ExportSink {
   public:
    virtual ~ExportSink() = default;
    virtual void AllocExport(ucode::AllocType /*type*/, uint32_t /*size*/) {}
    virtual void Export(ucode::ExportRegister /*export_register*/, const float* /*value*/,
                        uint32_t /*value_mask*/) {}
  };

  void SetTraceWriter(TraceWriter* new_trace_writer) { trace_writer_ = new_trace_writer; }

  ExportSink* GetExportSink() const { return export_sink_; }
  void SetExportSink(ExportSink* new_export_sink) { export_sink_ = new_export_sink; }

  using TextureFetchCallback = bool (*)(void* user_data,
                                        const ucode::TextureFetchInstruction& instr,
                                        const float* coordinates,
                                        uint32_t coordinate_count,
                                        float* rgba_out);
  void SetTextureFetchCallback(TextureFetchCallback callback, void* user_data) {
    texture_fetch_callback_ = callback;
    texture_fetch_callback_user_data_ = user_data;
  }

  const float* temp_registers() const { return &temp_registers_[0][0]; }
  float* temp_registers() { return &temp_registers_[0][0]; }

  static bool CanInterpretShader(const Shader& shader) {
    assert_true(shader.is_ucode_analyzed());
    // Texture fetches are interpreted as zero in the lightweight CPU fallback.
    // This is approximate, but still useful for Metal bring-up because many
    // shaders can produce valid positions without the sampled data.
    (void)shader;
    return true;
  }
  void SetShader(xenos::ShaderType shader_type, const uint32_t* ucode) {
    shader_type_ = shader_type;
    ucode_ = ucode;
  }
  void SetShader(const Shader& shader) {
    assert_true(CanInterpretShader(shader));
    SetShader(shader.type(), shader.ucode_dwords());
  }

  void Execute();
  bool ExecuteWithInstructionBudget(uint32_t instruction_budget);

 private:
  struct State {
    ucode::VertexFetchInstruction vfetch_full_last;
    uint32_t vfetch_address_dwords;
    float previous_scalar;
    uint32_t call_stack_depth;
    uint32_t call_return_addresses[4];
    uint32_t loop_stack_depth;
    xenos::LoopConstant loop_constants[4];
    uint32_t loop_iterators[4];
    int32_t address_register;
    bool predicate;

    void Reset() { std::memset(this, 0, sizeof(*this)); }

    int32_t GetLoopAddress() const {
      if (!loop_stack_depth || loop_stack_depth > 4) {
        return 0;
      }
      assert_true(loop_stack_depth && loop_stack_depth <= 4);
      uint32_t loop_stack_index = loop_stack_depth - 1;
      xenos::LoopConstant loop_constant = loop_constants[loop_stack_index];
      // Clamp to the real range specified in the IPR2015-00325 sequencer
      // specification.
      // https://portal.unifiedpatents.com/ptab/case/IPR2015-00325
      return std::min(INT32_C(256),
                      std::max(INT32_C(-256), int32_t(int32_t(loop_iterators[loop_stack_index]) *
                                                          loop_constant.step +
                                                      loop_constant.start)));
    }
  };

  static float FlushDenormal(float value) {
    uint32_t bits = *reinterpret_cast<const uint32_t*>(&value);
    bits &= (bits & UINT32_C(0x7F800000)) ? ~UINT32_C(0) : (UINT32_C(1) << 31);
    return *reinterpret_cast<const float*>(&bits);
  }

  uint32_t GetTempRegisterIndex(uint32_t address, bool is_relative) const {
    return (int32_t(address) + (is_relative ? state_.GetLoopAddress() : 0)) &
           ((UINT32_C(1) << xenos::kMaxShaderTempRegistersLog2) - 1);
  }
  const float* GetTempRegister(uint32_t address, bool is_relative) const {
    return temp_registers_[GetTempRegisterIndex(address, is_relative)];
  }
  float* GetTempRegister(uint32_t address, bool is_relative) {
    return temp_registers_[GetTempRegisterIndex(address, is_relative)];
  }
  const std::array<float, 4> GetFloatConstant(uint32_t address, bool is_relative,
                                              bool relative_address_is_a0) const;

  void ExecuteAluInstruction(ucode::AluInstruction instr);
  void StoreFetchResult(uint32_t dest, bool is_dest_relative, uint32_t swizzle, const float* value);
  void ExecuteVertexFetchInstruction(ucode::VertexFetchInstruction instr);
  void ExecuteTextureFetchInstruction(ucode::TextureFetchInstruction instr);

  const RegisterFile& register_file_;
  const memory::Memory& memory_;

  TraceWriter* trace_writer_ = nullptr;

  ExportSink* export_sink_ = nullptr;
  TextureFetchCallback texture_fetch_callback_ = nullptr;
  void* texture_fetch_callback_user_data_ = nullptr;

  xenos::ShaderType shader_type_ = xenos::ShaderType::kVertex;
  const uint32_t* ucode_ = nullptr;

  // For both inputs and locals.
  float temp_registers_[xenos::kMaxShaderTempRegisters][4];

  State state_;
};

}  // namespace rex::graphics
