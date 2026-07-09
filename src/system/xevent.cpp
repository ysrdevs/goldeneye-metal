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

#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/xevent.h>

namespace rex::system {

XEvent::XEvent(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(event_);

  this->CreateNative<X_KEVENT>();

  if (manual_reset) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(event_);

  switch (header->type) {
    case 0x00:  // EventNotificationObject (manual reset)
      manual_reset_ = true;
      break;
    case 0x01:  // EventSynchronizationObject (auto reset)
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  bool initial_state = header->signal_state ? true : false;
  if (manual_reset_) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  if (out_type) {
    *out_type = manual_reset_ ? 0x00 : 0x01;
  }
  if (out_state) {
    // Query the live host event, not the stale guest header
    auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
    if (result == rex::thread::WaitResult::kSuccess) {
      *out_state = 1;
      // Re-signal since we consumed the signal by waiting
      event_->Set();
    } else {
      *out_state = 0;
    }
  }
}

// Mirror the signalled state into the guest KEVENT header. Real KeSetEvent /
// KeResetEvent update Header.SignalState, and some guest code reads it directly
// (GoldenEye's render thread does, to choose between a timed and an INFINITE
// wait). Previously Set/Reset only touched the host event, leaving SignalState
// stale -> the render thread picked an infinite wait and deadlocked = freeze.
void XEvent::SetGuestSignalState(uint32_t state) {
  uint32_t gobj = guest_object();
  if (!gobj) {
    return;
  }
  auto* header = memory()->TranslateVirtual<X_DISPATCH_HEADER*>(gobj);
  if (header) {
    header->signal_state = state;
  }
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  if (is_render_.load(std::memory_order_relaxed)) {
    // Hold this wakeup until a Wait actually consumes it (see Reset / WaitCallback).
    render_pending_.store(true, std::memory_order_relaxed);
  }
  SetGuestSignalState(1);
  event_->Set();
  return 1;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  event_->Pulse();
  SetGuestSignalState(0);
  return 1;
}

int32_t XEvent::Reset() {
  // A render event must not drop a pending GPU-completion Set. GoldenEye's render
  // loop calls KeResetEvent on the same event the GPU interrupt signals, right in
  // the window where the Set can land first; under emulation timing the Reset
  // then destroys it and the render thread hangs forever -> visual freeze. So if
  // a Set is pending, keep the event set; only a successful Wait clears it.
  if (is_render_.load(std::memory_order_relaxed) &&
      render_pending_.load(std::memory_order_relaxed)) {
    return 1;
  }
  SetGuestSignalState(0);
  event_->Reset();
  return 1;
}

void XEvent::Clear() {
  SetGuestSignalState(0);
  event_->Reset();
}

void XEvent::WaitCallback() {
  // The waiter received the signal, so any held render Set has been delivered.
  render_pending_.store(false, std::memory_order_relaxed);
}

bool XEvent::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("XEvent {:08X} ({})", handle(), manual_reset_ ? "manual" : "auto");
  SaveObject(stream);

  bool signaled = true;
  auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
  if (result == rex::thread::WaitResult::kSuccess) {
    signaled = true;
  } else if (result == rex::thread::WaitResult::kTimeout) {
    signaled = false;
  } else {
    assert_always();
  }

  if (signaled) {
    // Reset the event in-case it's an auto-reset.
    event_->Set();
  }

  stream->Write<bool>(signaled);
  stream->Write<bool>(manual_reset_);

  return true;
}

object_ref<XEvent> XEvent::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto evt = new XEvent(nullptr);
  evt->kernel_state_ = kernel_state;

  evt->RestoreObject(stream);
  bool signaled = stream->Read<bool>();
  evt->manual_reset_ = stream->Read<bool>();

  if (evt->manual_reset_) {
    evt->event_ = rex::thread::Event::CreateManualResetEvent(false);
  } else {
    evt->event_ = rex::thread::Event::CreateAutoResetEvent(false);
  }
  assert_not_null(evt->event_);

  if (signaled) {
    evt->event_->Set();
  }

  return object_ref<XEvent>(evt);
}

}  // namespace rex::system
