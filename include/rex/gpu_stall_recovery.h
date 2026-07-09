// GPU render-thread wakeup protection.
//
// GoldenEye's render thread sleeps on a guest event waiting for the GPU-
// completion interrupt to wake it for the next batch of work. The interrupt
// signals that event with KeSetEvent; the render thread, in its loop, calls
// KeResetEvent on the same event. On the real console the fixed timing kept the
// Set safely after the Reset. Under emulation the threads run on different host
// cores at different speeds, so the Set can land just before the Reset -> the
// Reset destroys it -> the render thread waits forever -> the ring drains and the
// screen freezes while everything else keeps running (worse under load).
//
// Fix (see XEvent): an event that the GPU completion interrupt signals is marked
// "render". A Set on a render event is remembered as pending; a Reset will NOT
// drop a pending Set; only a successful Wait consumes it. So the completion
// wakeup can never be lost -- no stall, no recovery, it just never goes missing.
//
// This file only brackets the guest interrupt handler so Sets made inside it can
// be identified as render-thread wakeups.
#pragma once

namespace rex {
namespace gpu_recovery {

void EnterGpuIsr();
void ExitGpuIsr();
bool InGpuIsr();  // true while the guest GPU interrupt handler is running

}  // namespace gpu_recovery
}  // namespace rex
