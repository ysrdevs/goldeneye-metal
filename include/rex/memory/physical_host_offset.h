/**
 ******************************************************************************
 * @file        physical_host_offset.h
 * @brief       Guest physical-heap to host-view offset helpers
 *
 * @license     BSD 3-Clause License
 ******************************************************************************
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/memory/utils.h>

namespace rex::memory {

// The 0xE0000000 guest view starts at backing-store offset 0x1000. Hosts whose
// mapping granularity is larger than 4 KiB must map from offset zero and add the
// skipped page in guest pointer arithmetic instead. Hosts with 4 KiB mappings
// can map the view at 0x1000 directly and need no pointer adjustment.
constexpr uint32_t PhysicalHostOffsetForGranularity(uint32_t guest_address,
                                                    size_t mapping_granularity) noexcept {
  return guest_address >= 0xE0000000u && mapping_granularity > 0x1000u ? 0x1000u : 0u;
}

inline uint32_t PhysicalHostOffset(uint32_t guest_address) noexcept {
#if defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))
  // Windows maps in 64 KiB units, and Apple Silicon uses 16 KiB pages.
  return guest_address >= 0xE0000000u ? 0x1000u : 0u;
#elif defined(__APPLE__)
  // Intel macOS uses 4 KiB pages, so the view can start at offset 0x1000.
  return 0u;
#else
  // Linux and Android page sizes vary by architecture and kernel.
  static const size_t host_mapping_granularity = allocation_granularity();
  return PhysicalHostOffsetForGranularity(guest_address, host_mapping_granularity);
#endif
}

}  // namespace rex::memory
