#pragma once

#include <cstdint>

namespace ge::crash_guards {

// This is the title's MSVC pure-virtual-call handler for the supported XEX.
// Only the packed-data accessor's two vtable+16 dispatches use this predicate;
// pure virtual calls anywhere else remain fatal and visible.
inline constexpr uint32_t kPackedDataPureVirtualTarget = 0x823EDF20u;

constexpr bool IsPackedDataPureVirtualTarget(uint32_t callback_target) {
  return callback_target == kPackedDataPureVirtualTarget;
}

constexpr bool RecoverPackedDataPureVirtualDispatch(uint32_t callback_target,
                                                    uint64_t& result) {
  if (!IsPackedDataPureVirtualTarget(callback_target)) {
    return false;
  }
  result = 0;
  return true;
}

static_assert(IsPackedDataPureVirtualTarget(0x823EDF20u));
static_assert(!IsPackedDataPureVirtualTarget(0));
static_assert(!IsPackedDataPureVirtualTarget(0x823EDF1Cu));
static_assert(!IsPackedDataPureVirtualTarget(0x823EDF24u));
static_assert(!IsPackedDataPureVirtualTarget(0x823D5E48u));
static_assert(!IsPackedDataPureVirtualTarget(0x823EF6C0u));

}  // namespace ge::crash_guards
