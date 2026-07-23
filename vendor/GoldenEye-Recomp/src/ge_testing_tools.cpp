#include "ge_testing_tools.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <rex/logging.h>

#include "ge_host_pause.h"
#include "ge_init.h"

namespace ge::testing {
namespace {

using detail::AvailabilityBlock;
using detail::CheatDefinition;
using detail::ToolIndex;

constexpr int32_t kTitleLevelId = 90;
constexpr uint32_t kNetworkSessionFlagAddress = 0x830CAEA0u;
constexpr uint32_t kCheatActiveFlagsAddress = 0x83063318u;
constexpr uint32_t kSlowMotionTimeScaleAddress = 0x82003264u;

struct PublishedState {
  std::atomic<bool> available{false};
  std::atomic<bool> active_known{false};
  std::atomic<bool> active{false};
  std::atomic<bool> request_rejected{false};
};

std::array<PublishedState, detail::kToolCount> g_states;
detail::RequestQueue g_requests;
std::atomic<bool> g_refresh_requested{true};
std::atomic<uint64_t> g_mutation_token{0};
std::atomic<AvailabilityBlock> g_availability_block{AvailabilityBlock::kNoMission};

constexpr bool IsSupported(Tool tool) noexcept {
  return detail::IsToggle(tool) || detail::IsAction(tool);
}

const char* UnavailableReason(AvailabilityBlock block) noexcept {
  switch (block) {
    case AvailabilityBlock::kNone:
      return "";
    case AvailabilityBlock::kNoMission:
      return "Available during an active single-player mission.";
    case AvailabilityBlock::kNetworkSession:
      return "Unavailable during network play.";
    case AvailabilityBlock::kRetailDebugMenu:
      return "Close the retail debug menu before changing host cheats.";
    case AvailabilityBlock::kHostSettingsClosed:
    case AvailabilityBlock::kPausePending:
      return "Waiting for host settings to pause gameplay.";
  }
  return "Testing control is currently unavailable.";
}

// Recompiled guest routines use the live PPC context as their register file.
// This hook is inserted in the middle of another guest routine, so restore the
// entire register context (including the host FPSCR mode) after side effects
// have been applied to guest memory.
class ScopedContextRestore {
 public:
  explicit ScopedContextRestore(PPCContext& context) : context_(context), saved_(context) {}

  ~ScopedContextRestore() {
    const uint32_t saved_fpscr = saved_.fpscr.csr;
    context_ = saved_;
    context_.fpscr.setcsr(saved_fpscr);
  }

  ScopedContextRestore(const ScopedContextRestore&) = delete;
  ScopedContextRestore& operator=(const ScopedContextRestore&) = delete;

 private:
  PPCContext& context_;
  PPCContext saved_;
};

int32_t CurrentLevel(PPCContext& context, uint8_t* base) {
  sub_820AE360(context, base);
  return context.r3.s32;
}

int32_t ActivePlayerCount(PPCContext& context, uint8_t* base) {
  // Retail counts non-null player slots here. Host cheat controls are
  // intentionally restricted to one player even though host pause itself also
  // supports local split-screen.
  sub_820C9AF0(context, base);
  return context.r3.s32;
}

bool RetailDebugMenuVisible(PPCContext& context, uint8_t* base) {
  // This is the retail debug-menu visibility getter used by the LB toggle path.
  // It only reads the game's visibility byte and is safe on the game thread.
  sub_82091FB8(context, base);
  return (context.r3.u32 & 0xFFu) != 0;
}

bool QueryCheat(PPCContext& context, uint8_t* base, uint32_t cheat_id) {
  context.r3.u64 = cheat_id;
  sub_82136FA0(context, base);
  return (context.r3.u32 & 1u) != 0;
}

bool QueryCheatState(PPCContext& context, uint8_t* base, const CheatDefinition& definition) {
  if (definition.behavior != CheatDefinition::Behavior::kTimeScaleToggle) {
    return QueryCheat(context, base, definition.cheat_id);
  }

  // Slow Motion (retail ID 27) deliberately does not use the normal per-player
  // active bit. Its enable/disable cases write the game's global time scale.
  // Compare against the same slow-speed threshold used by retail's enable
  // routine. This avoids claiming a milder scripted slowdown as cheat state.
  sub_8211E008(context, base);
  PPCRegister slow_time_scale{};
  slow_time_scale.u32 = REX_LOAD_U32(kSlowMotionTimeScaleAddress);
  return detail::SlowMotionActive(context.f1.f64, static_cast<double>(slow_time_scale.f32));
}

enum class SetCheatResult : uint8_t {
  kAlreadySet,
  kChanged,
  kRejected,
};

void SetQueryDrivenCheatBit(PPCContext& context, uint8_t* base, uint32_t cheat_id, bool enabled) {
  // IDs 76-79 are retail debug cheats whose effects query the normal active
  // bit, but the retail enable/disable switch only has side-effect cases up to
  // ID 74. Those routines set/clear this exact bit and then enter their
  // out-of-range diagnostic path. Reproduce only their verified current-player
  // bit operation here, avoiding the unwanted retail formatter/report call.
  sub_820C9B80(context, base);
  const uint32_t player_index = context.r3.u32 & 0xFFu;
  if (player_index >= 8) {
    return;
  }

  uint8_t& active_flags = base[kCheatActiveFlagsAddress + cheat_id];
  const uint8_t mask = static_cast<uint8_t>(1u << player_index);
  if (enabled) {
    active_flags = static_cast<uint8_t>(active_flags | mask);
  } else {
    active_flags = static_cast<uint8_t>(active_flags & ~mask);
  }
}

SetCheatResult SetCheat(PPCContext& context, uint8_t* base, const CheatDefinition& definition,
                        bool enabled) {
  if (QueryCheatState(context, base, definition) == enabled) {
    return SetCheatResult::kAlreadySet;
  }

  if (definition.behavior == CheatDefinition::Behavior::kQueryDrivenBit) {
    SetQueryDrivenCheatBit(context, base, definition.cheat_id, enabled);
  } else {
    context.r3.u64 = definition.cheat_id;
    if (enabled) {
      sub_82136FE8(context, base);
    } else {
      sub_82137968(context, base);
    }
  }

  if (QueryCheatState(context, base, definition) != enabled) {
    return SetCheatResult::kRejected;
  }
  return SetCheatResult::kChanged;
}

bool QueryOriginalGraphics(PPCContext& context, uint8_t* base, bool* known) {
  sub_82184A38(context, base);
  const uint32_t graphics_settings = context.r3.u32;
  if (graphics_settings == 0) {
    *known = false;
    return false;
  }

  context.r3.u64 = graphics_settings;
  context.r4.s64 = 19;
  sub_82183F28(context, base);
  *known = true;
  return (context.r3.u32 & 0xFFu) != 0;
}

void PublishUnavailable(AvailabilityBlock block) {
  g_mutation_token.store(0, std::memory_order_release);
  g_availability_block.store(block, std::memory_order_release);
  for (size_t index = 0; index < detail::kToolCount; ++index) {
    g_states[index].available.store(false, std::memory_order_release);
    g_states[index].active_known.store(false, std::memory_order_release);
    g_states[index].request_rejected.store(false, std::memory_order_release);
  }
}

void PublishAvailability(AvailabilityBlock block, uint64_t mutation_token) {
  const bool available = block == AvailabilityBlock::kNone;
  if (!available) {
    g_mutation_token.store(0, std::memory_order_release);
  }
  g_availability_block.store(block, std::memory_order_release);
  if (available) {
    // Publish the token before making controls available. A request racing the
    // opposite transition either captures this exact epoch or is ignored.
    g_mutation_token.store(mutation_token, std::memory_order_release);
  }
  for (size_t index = 0; index < detail::kToolCount; ++index) {
    const Tool tool = static_cast<Tool>(index);
    g_states[index].available.store(available && IsSupported(tool), std::memory_order_release);
  }
}

void PublishCheat(PPCContext& context, uint8_t* base, const CheatDefinition& definition) {
  const size_t index = ToolIndex(definition.tool);
  const bool active = QueryCheatState(context, base, definition);
  g_states[index].active.store(active, std::memory_order_release);
  g_states[index].active_known.store(true, std::memory_order_release);
}

void PublishGraphicsProperty(PPCContext& context, uint8_t* base) {
  constexpr Tool tool = Tool::kOriginalRemastered;
  const size_t index = ToolIndex(tool);
  bool known = false;
  const bool active = QueryOriginalGraphics(context, base, &known);
  g_states[index].active.store(active, std::memory_order_release);
  g_states[index].active_known.store(known, std::memory_order_release);
}

uint64_t UpdateMutationEpoch(int32_t level_id, uint64_t pause_generation,
                             AvailabilityBlock block) noexcept {
  struct Identity {
    int32_t level_id = 0;
    uint64_t pause_generation = 0;
    AvailabilityBlock block = AvailabilityBlock::kNoMission;
    bool initialized = false;
  };
  static Identity previous;
  static uint64_t epoch = 0;

  if (!previous.initialized || previous.level_id != level_id ||
      previous.pause_generation != pause_generation || previous.block != block) {
    previous = {.level_id = level_id,
                .pause_generation = pause_generation,
                .block = block,
                .initialized = true};
    if (++epoch == 0 || epoch > detail::RequestQueue::kMaximumToken) {
      epoch = 1;
    }
  }
  return epoch;
}

detail::MutationConditions MakeMutationConditions(int32_t level_id, int32_t player_count,
                                                  bool network_session, bool debug_menu_visible,
                                                  const host_pause::Snapshot& pause) noexcept {
  return {
      .level_id = level_id,
      .player_count = player_count,
      .network_session = network_session,
      .retail_debug_menu_visible = debug_menu_visible,
      .pause_requested = pause.requested,
      .pause_request_applied = pause.request_applied,
      .pause_available = pause.available,
      .gameplay_paused = pause.gameplay_paused,
      .host_owned = pause.host_owned,
      .pause_generation_matches = pause.generation == pause.applied_generation,
  };
}

}  // namespace

ToolState GetToolState(Tool tool) noexcept {
  if (!detail::IsValidTool(tool)) {
    return {.supported = false,
            .available = false,
            .active_known = false,
            .active = false,
            .request_rejected = false,
            .unavailable_reason = "Unknown testing control."};
  }

  if (tool == Tool::kRestartMission) {
    return {.supported = false,
            .available = false,
            .active_known = false,
            .active = false,
            .request_rejected = false,
            .unavailable_reason =
                "Restart Mission is disabled until a verified retail entry point is mapped."};
  }

  const PublishedState& state = g_states[ToolIndex(tool)];
  const bool available = state.available.load(std::memory_order_acquire);
  return {
      .supported = IsSupported(tool),
      .available = available,
      .active_known = state.active_known.load(std::memory_order_acquire),
      .active = state.active.load(std::memory_order_acquire),
      .request_rejected = state.request_rejected.load(std::memory_order_acquire),
      .unavailable_reason =
          available ? "" : UnavailableReason(g_availability_block.load(std::memory_order_acquire)),
  };
}

bool RequestSetEnabled(Tool tool, bool enabled) noexcept {
  if (!detail::IsToggle(tool)) {
    return false;
  }
  const size_t index = ToolIndex(tool);
  if (!g_states[index].available.load(std::memory_order_acquire)) {
    return false;
  }
  const uint64_t token = g_mutation_token.load(std::memory_order_acquire);
  if (token == 0) {
    return false;
  }
  g_states[index].request_rejected.store(false, std::memory_order_release);
  const bool queued = g_requests.QueueToggle(tool, enabled, token);
  if (!queued) {
    g_states[index].request_rejected.store(true, std::memory_order_release);
  }
  return queued;
}

bool RequestAction(Tool tool) noexcept {
  if (!detail::IsAction(tool)) {
    return false;
  }
  const size_t index = ToolIndex(tool);
  if (!g_states[index].available.load(std::memory_order_acquire)) {
    return false;
  }
  const uint64_t token = g_mutation_token.load(std::memory_order_acquire);
  if (token == 0) {
    return false;
  }
  return g_requests.QueueAction(tool, token);
}

void RequestRefresh() noexcept {
  g_refresh_requested.store(true, std::memory_order_release);
}

void ProcessTestingToolRequests(PPCContext& context, uint8_t* base) noexcept {
  if (!base) {
    PublishUnavailable(AvailabilityBlock::kNoMission);
    g_requests.DiscardAll();
    return;
  }

  ScopedContextRestore restore(context);
  const int32_t level_id = CurrentLevel(context, base);
  const int32_t player_count = ActivePlayerCount(context, base);
  const bool network_session = base[kNetworkSessionFlagAddress] != 0;
  const bool debug_menu_visible = RetailDebugMenuVisible(context, base);
  const host_pause::Snapshot pause = host_pause::GetSnapshot();
  const AvailabilityBlock block = detail::MutationAvailability(
      MakeMutationConditions(level_id, player_count, network_session, debug_menu_visible, pause));
  const uint64_t mutation_token = UpdateMutationEpoch(level_id, pause.generation, block);
  PublishAvailability(block, mutation_token);

  const bool mission_active =
      level_id > 0 && level_id < kTitleLevelId && player_count == 1 && !network_session;
  if (!mission_active) {
    PublishUnavailable(network_session ? AvailabilityBlock::kNetworkSession
                                       : AvailabilityBlock::kNoMission);
    // Never carry a host request through a title/mission transition,
    // split-screen transition, or network session.
    g_requests.DiscardAll();
    g_refresh_requested.store(true, std::memory_order_release);
    return;
  }

  if (block != AvailabilityBlock::kNone) {
    // A request is only retained during the short pause-acquisition window.
    // Every other loss of eligibility consumes it permanently.
    if (block != AvailabilityBlock::kPausePending) {
      g_requests.DiscardAll();
    }
  }

  bool state_changed = false;
  if (block == AvailabilityBlock::kNone) {
    for (const CheatDefinition& definition : detail::kCheatDefinitions) {
      const detail::PendingToggle pending = g_requests.TakeToggle(definition.tool, mutation_token);
      if (!pending.present) {
        continue;
      }

      const SetCheatResult result = SetCheat(context, base, definition, pending.enabled);
      state_changed = true;
      g_states[ToolIndex(definition.tool)].request_rejected.store(
          result == SetCheatResult::kRejected, std::memory_order_release);
      if (result == SetCheatResult::kChanged) {
        REXLOG_INFO("[ge] GETOOLS tool={} id={} enabled={}", definition.log_name,
                    definition.cheat_id, pending.enabled ? 1 : 0);
      } else if (result == SetCheatResult::kRejected) {
        REXLOG_WARN("[ge] GETOOLS rejected tool={} id={} enabled={}", definition.log_name,
                    definition.cheat_id, pending.enabled ? 1 : 0);
      }
    }

    if (g_requests.TakeAction(Tool::kOriginalRemastered, mutation_token)) {
      // This is the exact routine used by the retail right-bumper path. It
      // toggles graphics property 19 through the game's settings object.
      sub_82099778(context, base);
      state_changed = true;
      REXLOG_INFO("[ge] GETOOLS action=toggle_original_remastered");
    }

    // These are the exact retail Debug Menu > Unlockables callbacks. Both use
    // the current retail profile and its normal progression setter. Consume
    // both requests before choosing so an accidental same-frame pair can only
    // run the comprehensive action once.
    const bool unlock_all_levels =
        g_requests.TakeAction(Tool::kUnlockAllLevels, mutation_token);
    const bool unlock_one_level =
        g_requests.TakeAction(Tool::kUnlockOneLevel, mutation_token);
    if (unlock_all_levels) {
      sub_82091CA8(context, base);
      state_changed = true;
      REXLOG_INFO("[ge] GETOOLS action=unlock_all_levels");
    } else if (unlock_one_level) {
      sub_82091D18(context, base);
      state_changed = true;
      REXLOG_INFO("[ge] GETOOLS action=unlock_one_level");
    }
  }

  static uint32_t refresh_poll = 0;
  const bool explicit_refresh = g_refresh_requested.exchange(false, std::memory_order_acq_rel);
  if (state_changed || explicit_refresh || ++refresh_poll >= 30) {
    refresh_poll = 0;
    for (const CheatDefinition& definition : detail::kCheatDefinitions) {
      PublishCheat(context, base, definition);
    }
    PublishGraphicsProperty(context, base);
  }
}

}  // namespace ge::testing
