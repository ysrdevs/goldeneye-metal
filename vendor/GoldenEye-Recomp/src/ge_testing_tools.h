// Host-side testing controls backed by verified retail game entry points.
//
// The pause-menu UI runs on the host render thread. Guest functions must only
// run from GoldenEye's game thread, so requests made here are queued and
// consumed by ProcessTestingToolRequests() during the next input poll.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct PPCContext;

namespace ge::testing {

enum class Tool : uint8_t {
  kGodMode,
  kAllGuns,
  kInvisible,
  kInfiniteAmmo,
  kBigHeads,
  kTinyBond,
  kPaintballMode,
  kNoRadar,
  kTurboMode,
  kSlowMotion,
  kInvulnerableCharacters,
  kStickInsects,
  kVaselineVision,
  kFrescoMode,
  kRestartMission,
  kOriginalRemastered,
  kUnlockOneLevel,
  kUnlockAllLevels,
  kCount,
};

struct ToolState {
  // False means no verified retail entry point has been mapped. The UI should
  // show the control disabled instead of attempting a fallback guest write.
  bool supported = false;

  // A supported control can still be unavailable at the title screen. Guest
  // gameplay state is sampled on the game thread and published atomically.
  bool available = false;

  // Toggle state is unknown until a mission is active and the game thread has
  // sampled it. For kOriginalRemastered, active means original/classic. This
  // polarity is verified against the retail property-19 resource selection:
  // true selects files/original and false selects files/new.
  bool active_known = false;
  bool active = false;

  // Set when the game thread processed the most recent toggle request but the
  // resulting retail state did not match it. A new request clears this flag.
  bool request_rejected = false;

  // Static storage; valid for the lifetime of the process. Empty when the
  // control is currently usable.
  const char* unavailable_reason = "";
};

// Thread-safe host/UI API.
ToolState GetToolState(Tool tool) noexcept;
// Returns false if the request could not be queued in the current mission and
// pause epoch. The UI must not show a pending state for an unqueued request.
bool RequestSetEnabled(Tool tool, bool enabled) noexcept;
// Returns false if the action could not be queued in the current mission and
// pause epoch.
bool RequestAction(Tool tool) noexcept;
void RequestRefresh() noexcept;

// Game-thread bridge. Called by ge_inject_keyboard; not for UI code.
void ProcessTestingToolRequests(PPCContext& context, uint8_t* base) noexcept;

namespace detail {

inline constexpr size_t kToolCount = static_cast<size_t>(Tool::kCount);
inline constexpr size_t kCheatCount = 14;

constexpr size_t ToolIndex(Tool tool) noexcept {
  return static_cast<size_t>(tool);
}

constexpr bool IsValidTool(Tool tool) noexcept {
  return ToolIndex(tool) < kToolCount;
}

struct CheatDefinition {
  Tool tool;
  uint32_t cheat_id;
  const char* log_name;
  enum class Behavior : uint8_t {
    kRetailToggle,
    kTimeScaleToggle,
    kQueryDrivenBit,
  } behavior;
};

// IDs come from the retail cheat table. Normal changes use
// sub_82136FE8/sub_82137968. Slow Motion is one retail special case whose
// state is the global time scale rather than the normal active bit. IDs 76-79
// use their verified active bit because the retail mutation routines fall into
// an out-of-range report after performing that same bit operation. Everything
// runs on the game thread.
inline constexpr std::array<CheatDefinition, kCheatCount> kCheatDefinitions = {{
    {Tool::kGodMode, 2, "god_mode", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kAllGuns, 3, "all_guns", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kInvisible, 10, "invisible", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kInfiniteAmmo, 11, "infinite_ammo", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kBigHeads, 12, "big_heads", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kTinyBond, 14, "tiny_bond", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kPaintballMode, 15, "paintball_mode", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kNoRadar, 23, "no_radar", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kTurboMode, 24, "turbo_mode", CheatDefinition::Behavior::kRetailToggle},
    {Tool::kSlowMotion, 27, "slow_motion", CheatDefinition::Behavior::kTimeScaleToggle},
    {Tool::kInvulnerableCharacters, 76, "invulnerable_characters",
     CheatDefinition::Behavior::kQueryDrivenBit},
    {Tool::kStickInsects, 77, "stick_insects", CheatDefinition::Behavior::kQueryDrivenBit},
    {Tool::kVaselineVision, 78, "vaseline_vision", CheatDefinition::Behavior::kQueryDrivenBit},
    {Tool::kFrescoMode, 79, "fresco_mode", CheatDefinition::Behavior::kQueryDrivenBit},
}};

constexpr bool SlowMotionActive(double current_time_scale, double slow_time_scale) noexcept {
  // This is the same threshold used by retail's enable routine. Avoid treating
  // a milder scripted/cinematic slowdown as ownership by the Slow Motion cheat.
  return current_time_scale <= slow_time_scale;
}

constexpr const CheatDefinition* FindCheat(Tool tool) noexcept {
  for (const CheatDefinition& definition : kCheatDefinitions) {
    if (definition.tool == tool) {
      return &definition;
    }
  }
  return nullptr;
}

constexpr bool IsToggle(Tool tool) noexcept {
  return FindCheat(tool) != nullptr;
}

constexpr bool IsAction(Tool tool) noexcept {
  return tool == Tool::kOriginalRemastered || tool == Tool::kUnlockOneLevel ||
         tool == Tool::kUnlockAllLevels;
}

enum class AvailabilityBlock : uint8_t {
  kNone,
  kNoMission,
  kNetworkSession,
  kRetailDebugMenu,
  kHostSettingsClosed,
  kPausePending,
};

struct MutationConditions {
  int32_t level_id = 0;
  int32_t player_count = 0;
  bool network_session = false;
  bool retail_debug_menu_visible = false;
  bool pause_requested = false;
  bool pause_request_applied = false;
  bool pause_available = false;
  bool gameplay_paused = false;
  bool host_owned = false;
  bool pause_generation_matches = false;
};

constexpr AvailabilityBlock MutationAvailability(const MutationConditions& conditions) noexcept {
  if (conditions.level_id <= 0 || conditions.level_id >= 90 || conditions.player_count != 1) {
    return AvailabilityBlock::kNoMission;
  }
  if (conditions.network_session) {
    return AvailabilityBlock::kNetworkSession;
  }
  if (conditions.retail_debug_menu_visible) {
    return AvailabilityBlock::kRetailDebugMenu;
  }
  if (!conditions.pause_requested) {
    return AvailabilityBlock::kHostSettingsClosed;
  }
  if (!conditions.pause_request_applied || !conditions.pause_available ||
      !conditions.gameplay_paused || !conditions.host_owned ||
      !conditions.pause_generation_matches) {
    return AvailabilityBlock::kPausePending;
  }
  return AvailabilityBlock::kNone;
}

struct PendingToggle {
  bool present = false;
  bool enabled = false;
  uint64_t token = 0;
};

// Host requests capture the exact game-thread mutation token that made their
// control available. A request with an old token is consumed but rejected, so
// it can never spill into a later mission or a later host-menu pause cycle.
class RequestQueue {
 public:
  static constexpr uint64_t kMaximumToken = (uint64_t{1} << 62) - 1;

  bool QueueToggle(Tool tool, bool enabled, uint64_t token) noexcept {
    if (!IsToggle(tool) || token == 0 || token > kMaximumToken) {
      return false;
    }
    const uint64_t encoded = (token << 2) | 2u | (enabled ? 1u : 0u);
    toggles_[ToolIndex(tool)].store(encoded, std::memory_order_release);
    return true;
  }

  PendingToggle TakeToggle(Tool tool, uint64_t expected_token) noexcept {
    if (!IsToggle(tool) || expected_token == 0) {
      return {};
    }
    const uint64_t encoded = toggles_[ToolIndex(tool)].exchange(0, std::memory_order_acq_rel);
    if ((encoded & 2u) == 0) {
      return {};
    }
    const uint64_t token = encoded >> 2;
    if (token != expected_token) {
      return {};
    }
    return {.present = true, .enabled = (encoded & 1u) != 0, .token = token};
  }

  bool QueueAction(Tool tool, uint64_t token) noexcept {
    if (!IsAction(tool) || token == 0 || token > kMaximumToken) {
      return false;
    }
    actions_[ToolIndex(tool)].store(token, std::memory_order_release);
    return true;
  }

  bool TakeAction(Tool tool, uint64_t expected_token) noexcept {
    if (!IsAction(tool) || expected_token == 0) {
      return false;
    }
    return actions_[ToolIndex(tool)].exchange(0, std::memory_order_acq_rel) == expected_token;
  }

  void DiscardAll() noexcept {
    for (std::atomic<uint64_t>& toggle : toggles_) {
      toggle.store(0, std::memory_order_release);
    }
    for (std::atomic<uint64_t>& action : actions_) {
      action.store(0, std::memory_order_release);
    }
  }

 private:
  std::array<std::atomic<uint64_t>, kToolCount> toggles_{};
  std::array<std::atomic<uint64_t>, kToolCount> actions_{};
};

}  // namespace detail

}  // namespace ge::testing
