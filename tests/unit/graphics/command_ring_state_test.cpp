/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/command_ring_state.h>

namespace {

using rex::graphics::CommandRingState;

TEST_CASE("Command ring decodes Xenos BUFSZ clamps", "[graphics][command_ring]") {
  CHECK(CommandRingState::DecodeCapacityDwords(0) == 8);
  CHECK(CommandRingState::DecodeCapacityDwords(1) == 8);
  CHECK(CommandRingState::DecodeCapacityDwords(2) == 8);
  CHECK(CommandRingState::DecodeCapacityDwords(3) == 16);
  CHECK(CommandRingState::DecodeCapacityDwords(12) == 8192);
  CHECK(CommandRingState::DecodeCapacityBytes(12) == 32768);
  CHECK(CommandRingState::DecodeCapacityDwords(22) == 8388608);
  CHECK(CommandRingState::DecodeCapacityDwords(23) == 8388608);
  CHECK(CommandRingState::DecodeCapacityDwords(63) == 8388608);
}

TEST_CASE("Command ring initialization uses byte base and invalid WPTR",
          "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x1FC9D003, 12);

  const auto snapshot = state.GetSnapshot();
  CHECK(snapshot.configured);
  CHECK(snapshot.base == 0x1FC9D000);
  CHECK(snapshot.capacity_dwords == 8192);
  CHECK(snapshot.capacity_bytes() == 0x8000);
  CHECK(snapshot.buffer_swap() == 2);
  CHECK((snapshot.control & CommandRingState::kControlNoUpdate) != 0);
  CHECK_FALSE(snapshot.write_pointer_valid());
  CHECK(snapshot.pending_dword_count() == 0);
}

TEST_CASE("Command ring reprogramming cannot consume a stale WPTR", "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 3);  // 16 dwords.
  REQUIRE(state.WriteWritePointer(7));
  CHECK(state.GetSnapshot().pending_dword_count() == 7);

  state.WriteBase(0x00200000);
  auto snapshot = state.GetSnapshot();
  CHECK(snapshot.base == 0x00200000);
  CHECK(snapshot.read_pointer == 0);
  CHECK_FALSE(snapshot.write_pointer_valid());
  CHECK(snapshot.pending_dword_count() == 0);

  REQUIRE(state.WriteWritePointer(3));
  snapshot = state.GetSnapshot();
  CHECK(snapshot.write_pointer == 3);
  CHECK(snapshot.pending_dword_count() == 3);

  state.WriteControl((snapshot.control & ~CommandRingState::kControlBufferSizeMask) | 4);
  snapshot = state.GetSnapshot();
  CHECK(snapshot.capacity_dwords == 32);
  CHECK(snapshot.read_pointer == 0);
  CHECK_FALSE(snapshot.write_pointer_valid());
}

TEST_CASE("Command ring control-only changes preserve active pointer state",
          "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 3);
  REQUIRE(state.WriteWritePointer(9));
  const auto before = state.GetSnapshot();

  state.WriteControl((before.control & ~CommandRingState::kControlNoUpdate) | (6u << 8));
  const auto after = state.GetSnapshot();
  CHECK(after.capacity_dwords == before.capacity_dwords);
  CHECK(after.generation == before.generation);
  CHECK(after.write_pointer == 9);
  CHECK(after.pending_dword_count() == 9);
}

TEST_CASE("Command ring defers RPTR_WR transfer until WPTR is written",
          "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 3);  // 16 dwords.
  state.WriteReadPointerWrite(14);

  auto snapshot = state.GetSnapshot();
  state.WriteControl(snapshot.control | CommandRingState::kControlReadPointerWriteEnable);
  CHECK(state.GetSnapshot().read_pointer == 0);

  REQUIRE(state.WriteWritePointer(3));
  snapshot = state.GetSnapshot();
  CHECK(snapshot.read_pointer == 14);
  CHECK(snapshot.write_pointer == 3);
  CHECK(snapshot.pending_dword_count() == 5);

  state.WriteControl(snapshot.control & ~CommandRingState::kControlReadPointerWriteEnable);
  state.WriteReadPointerWrite(7);
  REQUIRE(state.WriteWritePointer(5));
  snapshot = state.GetSnapshot();
  CHECK(snapshot.read_pointer == 14);
  CHECK(snapshot.write_pointer == 5);
}

TEST_CASE("Command ring applies 23-bit and capacity masks to pointers",
          "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 3);  // 16 dwords.
  state.WriteReadPointerWrite(0xFFFFFFFF);
  auto snapshot = state.GetSnapshot();
  state.WriteControl(snapshot.control | CommandRingState::kControlReadPointerWriteEnable);
  REQUIRE(state.WriteWritePointer(0x00800012));

  snapshot = state.GetSnapshot();
  CHECK(snapshot.read_pointer_write == CommandRingState::kPointerMask);
  CHECK(snapshot.read_pointer == 15);
  CHECK(snapshot.write_pointer == 2);
  CHECK(snapshot.pending_dword_count() == 3);
}

TEST_CASE("Command ring honors NO_UPDATE and RPTR_ADDR swap bits", "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 12);
  state.WriteReadPointerAddress(0x1234567A);

  auto snapshot = state.GetSnapshot();
  CHECK(snapshot.read_pointer_writeback_address() == 0x12345678);
  CHECK(snapshot.read_pointer_writeback_swap() == 2);
  CHECK_FALSE(snapshot.read_pointer_writeback_enabled());

  state.WriteControl(snapshot.control & ~CommandRingState::kControlNoUpdate);
  snapshot = state.GetSnapshot();
  CHECK(snapshot.read_pointer_writeback_enabled());

  state.WriteReadPointerAddress(2);
  CHECK_FALSE(state.GetSnapshot().read_pointer_writeback_enabled());
}

TEST_CASE("Command ring decodes RPTR writeback frequency in dwords", "[graphics][command_ring]") {
  CommandRingState state;
  state.Reprogram(0x00100000, 12 | (6u << 8));
  CHECK(state.GetSnapshot().read_pointer_writeback_interval_dwords() == 128);

  state.WriteControl(12 | (19u << 8));
  CHECK(state.GetSnapshot().read_pointer_writeback_interval_dwords() == 1048576);

  state.WriteControl(12 | (63u << 8));
  CHECK(state.GetSnapshot().read_pointer_writeback_interval_dwords() ==
        std::numeric_limits<uint64_t>::max());
}

TEST_CASE("Command ring computes pending work across wraparound", "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 3);  // 16 dwords.
  REQUIRE(state.SetReadPointer(14));
  REQUIRE(state.WriteWritePointer(3));
  CHECK(state.GetSnapshot().pending_dword_count() == 5);

  REQUIRE(state.SetReadPointer(2));
  REQUIRE(state.WriteWritePointer(10));
  CHECK(state.GetSnapshot().pending_dword_count() == 8);

  REQUIRE(state.SetReadPointer(10));
  CHECK(state.GetSnapshot().pending_dword_count() == 0);
  CHECK_FALSE(state.GetSnapshot().has_pending_commands());
}

TEST_CASE("Command ring rejects WPTR written before complete configuration",
          "[graphics][command_ring]") {
  CommandRingState state;
  CHECK_FALSE(state.WriteWritePointer(5));
  CHECK_FALSE(state.GetSnapshot().write_pointer_valid());

  state.WriteControl(3);
  CHECK_FALSE(state.WriteWritePointer(6));
  CHECK_FALSE(state.GetSnapshot().configured);
  CHECK_FALSE(state.GetSnapshot().write_pointer_valid());

  state.WriteBase(0x00100000);
  CHECK(state.GetSnapshot().configured);
  CHECK_FALSE(state.GetSnapshot().write_pointer_valid());
  REQUIRE(state.WriteWritePointer(7));
  CHECK(state.GetSnapshot().write_pointer == 7);
}

TEST_CASE("Command ring rejects stale worker completion after reprogramming",
          "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 3);
  REQUIRE(state.WriteWritePointer(7));
  const auto old_snapshot = state.GetSnapshot();

  state.WriteBase(0x00200000);
  CHECK_FALSE(state.CommitReadPointer(old_snapshot.generation, old_snapshot.write_pointer));
  CHECK(state.GetSnapshot().read_pointer == 0);

  REQUIRE(state.WriteWritePointer(4));
  const auto current_snapshot = state.GetSnapshot();
  REQUIRE(state.CommitReadPointer(current_snapshot.generation, current_snapshot.write_pointer));
  CHECK(state.GetSnapshot().read_pointer == 4);
  CHECK(state.GetSnapshot().pending_dword_count() == 0);
}

TEST_CASE("Command ring rejects stale worker completion after RPTR_WR transfer",
          "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 3);
  REQUIRE(state.WriteWritePointer(7));
  const auto worker_snapshot = state.GetSnapshot();

  state.WriteReadPointerWrite(5);
  state.WriteControl(worker_snapshot.control | CommandRingState::kControlReadPointerWriteEnable);
  REQUIRE(state.WriteWritePointer(9));

  CHECK_FALSE(state.CommitReadPointer(worker_snapshot.generation, worker_snapshot.write_pointer));
  const auto current_snapshot = state.GetSnapshot();
  CHECK(current_snapshot.read_pointer == 5);
  CHECK(current_snapshot.write_pointer == 9);
  CHECK(current_snapshot.pending_dword_count() == 4);
}

TEST_CASE("Command ring commit returns current writeback configuration",
          "[graphics][command_ring]") {
  CommandRingState state;
  state.Initialize(0x00100000, 3);
  REQUIRE(state.WriteWritePointer(7));
  const auto worker_snapshot = state.GetSnapshot();

  state.WriteReadPointerAddress(0x1234567Au);
  state.WriteControl(worker_snapshot.control & ~CommandRingState::kControlNoUpdate);

  CommandRingState::Snapshot committed_snapshot;
  REQUIRE(state.CommitReadPointerAndApply(
      worker_snapshot.generation, worker_snapshot.write_pointer,
      [&committed_snapshot](const CommandRingState::Snapshot& snapshot) {
        committed_snapshot = snapshot;
      }));
  CHECK(committed_snapshot.read_pointer == 7);
  CHECK(committed_snapshot.read_pointer_writeback_enabled());
  CHECK(committed_snapshot.read_pointer_writeback_address() == 0x12345678u);
  CHECK(committed_snapshot.read_pointer_writeback_swap() == 2);
}

TEST_CASE("Command ring restores its complete register state", "[graphics][command_ring]") {
  CommandRingState source;
  source.Initialize(0x00100003, 3);
  source.WriteReadPointerAddress(0x12345679u);
  source.WriteReadPointerWrite(14);
  auto source_snapshot = source.GetSnapshot();
  source.WriteControl((source_snapshot.control & ~CommandRingState::kControlNoUpdate) |
                      CommandRingState::kControlReadPointerWriteEnable | (6u << 8));
  REQUIRE(source.WriteWritePointer(3));
  source_snapshot = source.GetSnapshot();

  CommandRingState restored;
  restored.Restore(source_snapshot);
  const auto restored_snapshot = restored.GetSnapshot();

  CHECK(restored_snapshot.base == source_snapshot.base);
  CHECK(restored_snapshot.control == source_snapshot.control);
  CHECK(restored_snapshot.read_pointer_address_register ==
        source_snapshot.read_pointer_address_register);
  CHECK(restored_snapshot.read_pointer == source_snapshot.read_pointer);
  CHECK(restored_snapshot.read_pointer_write == source_snapshot.read_pointer_write);
  CHECK(restored_snapshot.write_pointer == source_snapshot.write_pointer);
  CHECK(restored_snapshot.base_programmed == source_snapshot.base_programmed);
  CHECK(restored_snapshot.control_programmed == source_snapshot.control_programmed);
  CHECK(restored_snapshot.configured == source_snapshot.configured);
}

}  // namespace
