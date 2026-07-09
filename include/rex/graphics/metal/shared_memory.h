#pragma once

#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/graphics/trace_writer.h>
#include <rex/memory.h>

namespace rex::graphics::metal {

class MetalSharedMemory final : public SharedMemory {
 public:
  // trace_writer records every uploaded guest page range into the GPU trace
  // (mirrors VulkanSharedMemory). Without it a Metal-captured trace omits the
  // vertex/texture backing memory the draws fetch, poisoning replay.
  MetalSharedMemory(memory::Memory& memory, TraceWriter& trace_writer);
  ~MetalSharedMemory() override;

  // metal_device must be a non-owning id<MTLDevice> passed as void* (the command
  // processor stores the device as void*). Allocates the backing MTLBuffer with
  // MTLStorageModeShared (Apple Silicon UMA: resolve compute write, texture-cache
  // read, and CPU readback all see the same bytes).
  bool Initialize(void* metal_device);
  void Shutdown(bool from_destructor = false);

  memory::Memory& guest_memory() const { return memory(); }

  // Returns the GPU buffer backing this shared memory as an opaque pointer.
  // Valid after a successful Initialize() and before Shutdown(). ObjC++ callers
  // must cast to id<MTLBuffer>. Returned as void* so this header stays usable
  // from plain C++ translation units (e.g. command_processor.cpp). Mirrors
  // VulkanSharedMemory::buffer() / D3D12SharedMemory::GetBuffer().
  void* buffer() const { return buffer_; }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  TraceWriter& trace_writer_;     // Records uploaded ranges into the GPU trace.
  void* metal_device_ = nullptr;  // Non-owning id<MTLDevice>.
  void* buffer_ = nullptr;        // Owned id<MTLBuffer>, MTLStorageModeShared.
};

}  // namespace rex::graphics::metal
