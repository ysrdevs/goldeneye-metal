/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <limits>
#include <mutex>

namespace rex::graphics {

// Thread-safe software model of the Xenos command processor's primary ring
// registers. Register addresses supplied to this class are dword indices (the
// byte MMIO offset divided by four), matching RegisterFile indices.
//
// The class deliberately separates programming the ring from ringing the WPTR
// doorbell. A base or effective-size change invalidates the previous WPTR, so a
// worker cannot accidentally consume a newly programmed ring using a pointer
// left over from the previous ring. A fresh WPTR write is required before the
// ring reports pending work.
class CommandRingState {
 public:
  static constexpr uint32_t kRegisterBase = 0x01C0;                // 0x700
  static constexpr uint32_t kRegisterControl = 0x01C1;             // 0x704
  static constexpr uint32_t kRegisterReadPointerAddress = 0x01C3;  // 0x70C
  static constexpr uint32_t kRegisterReadPointer = 0x01C4;         // 0x710, read-only
  static constexpr uint32_t kRegisterWritePointer = 0x01C5;        // 0x714
  static constexpr uint32_t kRegisterWritePointerDelay = 0x01C6;   // 0x718
  static constexpr uint32_t kRegisterReadPointerWrite = 0x01C7;    // 0x71C

  static constexpr uint32_t kControlBufferSizeMask = 0x0000003F;
  static constexpr uint32_t kControlBlockSizeMask = 0x00003F00;
  static constexpr uint32_t kControlBufferSwapMask = 0x00030000;
  static constexpr uint32_t kControlBufferSwap32 = 0x00020000;
  static constexpr uint32_t kControlNoUpdate = 0x08000000;
  static constexpr uint32_t kControlReadPointerWriteEnable = 0x80000000;

  static constexpr uint32_t kPointerMask = 0x007FFFFF;
  static constexpr uint32_t kAddressMask = 0xFFFFFFFC;
  static constexpr uint32_t kReadPointerSwapMask = 0x00000003;

  // The Xbox graphics setup uses 32-bit swapping for the big-endian CPU view
  // and initially disables RPTR writeback until an address has been supplied.
  static constexpr uint32_t kDefaultInitializeControl = kControlBufferSwap32 | kControlNoUpdate;

  // Outside the 23-bit hardware pointer domain, so it cannot collide with a
  // valid ring index. This value is intentionally shared with the existing CP
  // worker's uninitialized-WPTR sentinel.
  static constexpr uint32_t kInvalidWritePointer = 0xBAADF00D;

  static constexpr uint32_t kMaximumBufferSizeLog2 = 22;

  struct Snapshot {
    uint64_t generation = 0;
    uint32_t base = 0;
    uint32_t control = 0;
    uint32_t read_pointer_address_register = 0;
    uint32_t read_pointer = 0;
    uint32_t read_pointer_write = 0;
    uint32_t write_pointer = kInvalidWritePointer;
    uint32_t capacity_dwords = 0;
    bool base_programmed = false;
    bool control_programmed = false;
    bool configured = false;

    [[nodiscard]] bool write_pointer_valid() const noexcept {
      return write_pointer != kInvalidWritePointer;
    }

    [[nodiscard]] uint32_t capacity_bytes() const noexcept {
      return capacity_dwords * uint32_t(sizeof(uint32_t));
    }

    [[nodiscard]] uint32_t read_pointer_writeback_address() const noexcept {
      return read_pointer_address_register & kAddressMask;
    }

    [[nodiscard]] uint32_t read_pointer_writeback_swap() const noexcept {
      return read_pointer_address_register & kReadPointerSwapMask;
    }

    [[nodiscard]] bool read_pointer_writeback_enabled() const noexcept {
      return configured && !(control & kControlNoUpdate) && read_pointer_writeback_address() != 0;
    }

    [[nodiscard]] uint32_t buffer_swap() const noexcept {
      return (control & kControlBufferSwapMask) >> 16;
    }

    [[nodiscard]] uint32_t block_size_log2_quadwords() const noexcept {
      return (control & kControlBlockSizeMask) >> 8;
    }

    // RPTR writeback is performed after 2^RB_BLKSZ quadwords, or twice that
    // many dwords. The largest encodable interval is 2^64 dwords, which cannot
    // be represented by uint64_t, so that one value is saturated.
    [[nodiscard]] uint64_t read_pointer_writeback_interval_dwords() const noexcept {
      const uint32_t block_size_log2 = block_size_log2_quadwords();
      if (block_size_log2 >= 63) {
        return std::numeric_limits<uint64_t>::max();
      }
      return uint64_t{2} << block_size_log2;
    }

    [[nodiscard]] uint32_t pending_dword_count() const noexcept {
      if (!configured || !write_pointer_valid() || !capacity_dwords) {
        return 0;
      }
      if (write_pointer >= read_pointer) {
        return write_pointer - read_pointer;
      }
      return capacity_dwords - read_pointer + write_pointer;
    }

    [[nodiscard]] bool has_pending_commands() const noexcept { return pending_dword_count() != 0; }
  };

  CommandRingState() = default;
  CommandRingState(const CommandRingState&) = delete;
  CommandRingState& operator=(const CommandRingState&) = delete;

  // Hardware BUFSZ is log2(size in quadwords). In dwords, values 0 and 1
  // clamp to 8, values 2 through 22 produce 2^(BUFSZ + 1), and larger values
  // clamp to the value for 22.
  [[nodiscard]] static constexpr uint32_t DecodeCapacityDwords(uint32_t control) noexcept {
    uint32_t size_log2 = control & kControlBufferSizeMask;
    if (size_log2 < 2) {
      size_log2 = 2;
    } else if (size_log2 > kMaximumBufferSizeLog2) {
      size_log2 = kMaximumBufferSizeLog2;
    }
    return uint32_t{1} << (size_log2 + 1);
  }

  [[nodiscard]] static constexpr uint32_t DecodeCapacityBytes(uint32_t control) noexcept {
    return DecodeCapacityDwords(control) * uint32_t(sizeof(uint32_t));
  }

  [[nodiscard]] static constexpr uint32_t NormalizePointer(uint32_t pointer,
                                                           uint32_t capacity_dwords) noexcept {
    if (!capacity_dwords) {
      return 0;
    }
    // All valid decoded capacities are powers of two.
    return (pointer & kPointerMask) & (capacity_dwords - 1);
  }

  // Semantic initialization used by VdInitializeRingBuffer. Unlike separate
  // MMIO register writes, this is a complete ring replacement, so all pointer
  // state and writeback configuration are reset together.
  void Initialize(uint32_t base, uint32_t size_log2) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t encoded_size =
        size_log2 > kMaximumBufferSizeLog2 ? kMaximumBufferSizeLog2 : size_log2;
    base_ = base & kAddressMask;
    control_ = kDefaultInitializeControl | encoded_size;
    read_pointer_address_register_ = 0;
    read_pointer_ = 0;
    read_pointer_write_ = 0;
    write_pointer_ = kInvalidWritePointer;
    base_programmed_ = true;
    control_programmed_ = true;
    ++generation_;
  }

  // Atomically programs the raw BASE and CNTL register pair. This is useful
  // for state restore and tests; normal MMIO may call WriteBase/WriteControl in
  // either order.
  void Reprogram(uint32_t base, uint32_t control) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    base_ = base & kAddressMask;
    control_ = control;
    read_pointer_ = 0;
    write_pointer_ = kInvalidWritePointer;
    base_programmed_ = true;
    control_programmed_ = true;
    ++generation_;
  }

  void Reset() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    base_ = 0;
    control_ = 0;
    read_pointer_address_register_ = 0;
    read_pointer_ = 0;
    read_pointer_write_ = 0;
    write_pointer_ = kInvalidWritePointer;
    base_programmed_ = false;
    control_programmed_ = false;
    ++generation_;
  }

  // Restores all software-visible ring registers as one new generation. The
  // saved generation itself is intentionally not reused: any worker snapshot
  // from before a restore must become stale.
  void Restore(const Snapshot& snapshot) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    base_ = snapshot.base & kAddressMask;
    control_ = snapshot.control;
    read_pointer_address_register_ = snapshot.read_pointer_address_register;
    read_pointer_write_ = snapshot.read_pointer_write & kPointerMask;
    base_programmed_ = snapshot.base_programmed;
    control_programmed_ = snapshot.control_programmed;
    if (IsConfiguredLocked()) {
      const uint32_t capacity = DecodeCapacityDwords(control_);
      read_pointer_ = NormalizePointer(snapshot.read_pointer, capacity);
      write_pointer_ = snapshot.write_pointer_valid()
                           ? NormalizePointer(snapshot.write_pointer, capacity)
                           : kInvalidWritePointer;
    } else {
      read_pointer_ = 0;
      write_pointer_ = kInvalidWritePointer;
    }
    ++generation_;
  }

  void WriteBase(uint32_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t new_base = value & kAddressMask;
    if (!base_programmed_ || new_base != base_) {
      base_ = new_base;
      base_programmed_ = true;
      BeginNewRingGenerationLocked();
    }
  }

  void WriteControl(uint32_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t old_capacity =
        control_programmed_ ? DecodeCapacityDwords(control_) : uint32_t{0};
    const uint32_t new_capacity = DecodeCapacityDwords(value);
    const bool effective_size_changed = !control_programmed_ || old_capacity != new_capacity;
    control_ = value;
    control_programmed_ = true;
    if (effective_size_changed) {
      BeginNewRingGenerationLocked();
    }
  }

  void WriteReadPointerAddress(uint32_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    read_pointer_address_register_ = value;
  }

  void WriteReadPointerWrite(uint32_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    read_pointer_write_ = value & kPointerMask;
  }

  // Writes CP_RB_WPTR and rings the command-processor doorbell. If
  // RB_RPTR_WR_ENA is set, the deferred RPTR_WR value is transferred first,
  // exactly as on hardware. Returns false if BASE/CNTL are not both programmed;
  // in that case the premature WPTR is discarded rather than becoming stale.
  [[nodiscard]] bool WriteWritePointer(uint32_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsConfiguredLocked()) {
      write_pointer_ = kInvalidWritePointer;
      return false;
    }

    const uint32_t capacity = DecodeCapacityDwords(control_);
    if (control_ & kControlReadPointerWriteEnable) {
      const uint32_t new_read_pointer = NormalizePointer(read_pointer_write_, capacity);
      if (new_read_pointer != read_pointer_) {
        read_pointer_ = new_read_pointer;
        // Prevent a worker using a snapshot taken before the hardware-style
        // RPTR transfer from committing its obsolete completion pointer.
        ++generation_;
      }
    }
    write_pointer_ = NormalizePointer(value, capacity);
    return true;
  }

  // Sets the live RPTR for explicit state restoration. Normal worker progress
  // should use CommitReadPointer so a concurrent ring replacement is detected.
  [[nodiscard]] bool SetReadPointer(uint32_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsConfiguredLocked()) {
      return false;
    }
    const uint32_t new_read_pointer = NormalizePointer(value, DecodeCapacityDwords(control_));
    if (new_read_pointer != read_pointer_) {
      read_pointer_ = new_read_pointer;
      ++generation_;
    }
    return true;
  }

  // Commits worker progress only if the ring generation still matches the
  // snapshot that the worker executed. A base/size change or an RPTR_WR
  // transfer invalidates the old generation.
  [[nodiscard]] bool CommitReadPointer(uint64_t generation, uint32_t value) noexcept {
    return CommitReadPointerAndApply(generation, value, [](const Snapshot&) noexcept {});
  }

  // Invokes on_commit with the current register snapshot while the state lock
  // is still held. The callback must not call back into this object. This lets
  // the command processor make RPTR memory writeback atomic with the pointer
  // commit and its NO_UPDATE/address/swap configuration.
  template <typename CommitCallback>
  [[nodiscard]] bool CommitReadPointerAndApply(uint64_t generation, uint32_t value,
                                               CommitCallback&& on_commit) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsConfiguredLocked() || generation != generation_) {
      return false;
    }
    read_pointer_ = NormalizePointer(value, DecodeCapacityDwords(control_));
    on_commit(GetSnapshotLocked());
    return true;
  }

  void InvalidateWritePointer() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (write_pointer_ != kInvalidWritePointer) {
      write_pointer_ = kInvalidWritePointer;
      ++generation_;
    }
  }

  [[nodiscard]] Snapshot GetSnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetSnapshotLocked();
  }

 private:
  [[nodiscard]] Snapshot GetSnapshotLocked() const noexcept {
    Snapshot snapshot;
    snapshot.generation = generation_;
    snapshot.base = base_;
    snapshot.control = control_;
    snapshot.read_pointer_address_register = read_pointer_address_register_;
    snapshot.read_pointer = read_pointer_;
    snapshot.read_pointer_write = read_pointer_write_;
    snapshot.write_pointer = write_pointer_;
    snapshot.base_programmed = base_programmed_;
    snapshot.control_programmed = control_programmed_;
    snapshot.configured = IsConfiguredLocked();
    snapshot.capacity_dwords = snapshot.configured ? DecodeCapacityDwords(control_) : uint32_t{0};
    return snapshot;
  }
  [[nodiscard]] bool IsConfiguredLocked() const noexcept {
    return base_programmed_ && control_programmed_;
  }

  void BeginNewRingGenerationLocked() noexcept {
    read_pointer_ = 0;
    write_pointer_ = kInvalidWritePointer;
    ++generation_;
  }

  mutable std::mutex mutex_;
  uint64_t generation_ = 0;
  uint32_t base_ = 0;
  uint32_t control_ = 0;
  uint32_t read_pointer_address_register_ = 0;
  uint32_t read_pointer_ = 0;
  uint32_t read_pointer_write_ = 0;
  uint32_t write_pointer_ = kInvalidWritePointer;
  bool base_programmed_ = false;
  bool control_programmed_ = false;
};

}  // namespace rex::graphics
