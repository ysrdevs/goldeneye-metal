/**
 * @file        physical_host_offset_test.cpp
 * @brief       Physical guest-view host offset tests
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/memory/physical_host_offset.h>

TEST_CASE("Physical host offset follows mapping granularity", "[memory][physical]") {
  using rex::memory::PhysicalHostOffsetForGranularity;

  CHECK(PhysicalHostOffsetForGranularity(0xDFFFFFFFu, 0x10000u) == 0u);
  CHECK(PhysicalHostOffsetForGranularity(0xE0000000u, 0x1000u) == 0u);
  CHECK(PhysicalHostOffsetForGranularity(0xFFFFFFFFu, 0x1000u) == 0u);

  CHECK(PhysicalHostOffsetForGranularity(0xE0000000u, 0x4000u) == 0x1000u);
  CHECK(PhysicalHostOffsetForGranularity(0xFFC9C000u, 0x4000u) == 0x1000u);
  CHECK(PhysicalHostOffsetForGranularity(0xFFFFFFFFu, 0x10000u) == 0x1000u);
}

TEST_CASE("Runtime physical host offset matches this host", "[memory][physical]") {
  const uint32_t expected = rex::memory::allocation_granularity() > 0x1000u ? 0x1000u : 0u;

  CHECK(rex::memory::PhysicalHostOffset(0xDFFFFFFFu) == 0u);
  CHECK(rex::memory::PhysicalHostOffset(0xE0000000u) == expected);
  CHECK(rex::memory::PhysicalHostOffset(0xFFC9C000u) == expected);
}
