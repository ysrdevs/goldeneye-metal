/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>

namespace {

class TestGraphicsSystem final : public rex::graphics::GraphicsSystem {
 public:
  explicit TestGraphicsSystem(rex::memory::Memory& memory) { memory_ = &memory; }

  std::string name() const override { return "Metal"; }

 protected:
  void CreateProvider(bool with_presentation) override { (void)with_presentation; }

  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override {
    return nullptr;
  }
};

void StorePacketDword(rex::memory::Memory& memory, uint32_t packet_address, uint32_t index,
                      uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(
      memory.TranslatePhysical(packet_address + index * sizeof(uint32_t)), value);
}

}  // namespace

TEST_CASE("Metal completion packets are immediately guest-visible",
          "[graphics][metal][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  rex::graphics::metal::MetalCommandProcessor command_processor(&graphics_system, nullptr);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kShdTargetAddress = 0x00110000;
  constexpr uint32_t kExtTargetAddress = 0x00120000;
  constexpr uint32_t kShdValue = 0x12345678;

  auto* shd_target = memory.TranslatePhysical(kShdTargetAddress);
  std::memset(shd_target, 0xCD, sizeof(kShdValue));

  StorePacketDword(
      memory, kPacketAddress, 0,
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_EVENT_WRITE_SHD, 3));
  StorePacketDword(memory, kPacketAddress, 1, 0);
  StorePacketDword(memory, kPacketAddress, 2, kShdTargetAddress);
  StorePacketDword(memory, kPacketAddress, 3, kShdValue);

  command_processor.ExecutePacket(kPacketAddress, 4);

  uint32_t shd_actual = 0;
  std::memcpy(&shd_actual, shd_target, sizeof(shd_actual));
  CHECK(shd_actual == kShdValue);

  auto* ext_target = memory.TranslatePhysical(kExtTargetAddress);
  std::memset(ext_target, 0xCD, 12);

  StorePacketDword(
      memory, kPacketAddress, 0,
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_EVENT_WRITE_EXT, 2));
  StorePacketDword(memory, kPacketAddress, 1, 0);
  StorePacketDword(
      memory, kPacketAddress, 2,
      kExtTargetAddress | uint32_t(rex::graphics::xenos::Endian::k8in16));

  command_processor.ExecutePacket(kPacketAddress, 3);

  constexpr std::array<uint8_t, 12> kExpectedExtents = {
      0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
  };
  std::array<uint8_t, 12> ext_actual;
  std::memcpy(ext_actual.data(), ext_target, ext_actual.size());
  CHECK(ext_actual == kExpectedExtents);
}
