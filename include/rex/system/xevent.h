#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>

#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

namespace rex::system {

// https://www.nirsoft.net/kernel_struct/vista/KEVENT.html
struct X_KEVENT {
  X_DISPATCH_HEADER header;
};
static_assert_size(X_KEVENT, 0x10);

class XEvent : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Event;

  explicit XEvent(KernelState* kernel_state);
  ~XEvent() override;

  void Initialize(bool manual_reset, bool initial_state);
  void InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

  void Query(uint32_t* out_type, uint32_t* out_state);

  int32_t Set(uint32_t priority_increment, bool wait);
  int32_t Pulse(uint32_t priority_increment, bool wait);
  int32_t Reset();
  void Clear();

  bool Save(stream::ByteStream* stream) override;
  static object_ref<XEvent> Restore(KernelState* kernel_state, stream::ByteStream* stream);

  // Mark this as an event the GPU completion interrupt signals the render thread
  // with. Once marked, a Set is held "pending" until a Wait consumes it, so a
  // racing guest KeResetEvent can't drop the wakeup (see Set / Reset). Without
  // this, that race makes the render thread hang forever -> visual freeze.
  void MarkRenderEvent() { is_render_.store(true, std::memory_order_relaxed); }

 protected:
  rex::thread::WaitHandle* GetWaitHandle() override { return event_.get(); }
  // Fires on a successful wait: the waiter received the signal, so a pending
  // render Set is now delivered and can be cleared.
  void WaitCallback() override;

 private:
  // Write the signalled state (1/0) into the guest KEVENT header so guest code
  // that reads Header.SignalState directly sees the truth.
  void SetGuestSignalState(uint32_t state);

  bool manual_reset_ = false;
  std::unique_ptr<rex::thread::Event> event_;
  std::atomic<bool> is_render_{false};       // GPU-completion wakeup event
  std::atomic<bool> render_pending_{false};  // a render Set awaits a Wait to consume it
};

}  // namespace rex::system
