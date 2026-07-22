#include "ge_testing_tools.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

int failures = 0;

#define CHECK_TRUE(expression)                                                            \
  do {                                                                                    \
    if (!(expression)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expression); \
      ++failures;                                                                         \
    }                                                                                     \
  } while (false)

using ge::testing::Tool;
using ge::testing::detail::AvailabilityBlock;
using ge::testing::detail::CheatDefinition;
using ge::testing::detail::MutationConditions;
using ge::testing::detail::PendingToggle;
using ge::testing::detail::RequestQueue;

constexpr std::array<uint32_t, 14> kExpectedIds = {2,  3,  10, 11, 12, 14, 15,
                                                   23, 24, 27, 76, 77, 78, 79};

constexpr bool MappingIsCompleteAndUnique() {
  if (ge::testing::detail::kCheatDefinitions.size() != kExpectedIds.size()) {
    return false;
  }
  for (size_t index = 0; index < kExpectedIds.size(); ++index) {
    const CheatDefinition& definition = ge::testing::detail::kCheatDefinitions[index];
    if (definition.cheat_id != kExpectedIds[index] ||
        ge::testing::detail::ToolIndex(definition.tool) != index) {
      return false;
    }
    const CheatDefinition::Behavior expected_behavior =
        definition.cheat_id == 27
            ? CheatDefinition::Behavior::kTimeScaleToggle
            : (definition.cheat_id >= 76 ? CheatDefinition::Behavior::kQueryDrivenBit
                                         : CheatDefinition::Behavior::kRetailToggle);
    if (definition.behavior != expected_behavior) {
      return false;
    }
    for (size_t other = index + 1; other < kExpectedIds.size(); ++other) {
      const CheatDefinition& candidate = ge::testing::detail::kCheatDefinitions[other];
      if (definition.tool == candidate.tool || definition.cheat_id == candidate.cheat_id) {
        return false;
      }
    }
  }
  return true;
}

static_assert(MappingIsCompleteAndUnique());
static_assert(!ge::testing::detail::SlowMotionActive(1.0, 0.5));
static_assert(!ge::testing::detail::SlowMotionActive(0.75, 0.5));
static_assert(ge::testing::detail::SlowMotionActive(0.5, 0.5));
static_assert(ge::testing::detail::SlowMotionActive(0.25, 0.5));
static_assert(ge::testing::detail::FindCheat(Tool::kInvulnerableCharacters)->cheat_id == 76);
static_assert(ge::testing::detail::FindCheat(Tool::kStickInsects)->cheat_id == 77);
static_assert(ge::testing::detail::FindCheat(Tool::kVaselineVision)->cheat_id == 78);
static_assert(ge::testing::detail::FindCheat(Tool::kFrescoMode)->cheat_id == 79);
static_assert(!ge::testing::detail::IsToggle(Tool::kRestartMission));
static_assert(!ge::testing::detail::IsToggle(Tool::kOriginalRemastered));

MutationConditions ReadyMission() {
  return {
      .level_id = 33,
      .player_count = 1,
      .network_session = false,
      .retail_debug_menu_visible = false,
      .pause_requested = true,
      .pause_request_applied = true,
      .pause_available = true,
      .gameplay_paused = true,
      .host_owned = true,
      .pause_generation_matches = true,
  };
}

void TestMutationAvailability() {
  MutationConditions conditions = ReadyMission();
  CHECK_TRUE(ge::testing::detail::MutationAvailability(conditions) == AvailabilityBlock::kNone);

  conditions.level_id = 90;
  CHECK_TRUE(ge::testing::detail::MutationAvailability(conditions) ==
             AvailabilityBlock::kNoMission);
  conditions = ReadyMission();
  conditions.player_count = 2;
  CHECK_TRUE(ge::testing::detail::MutationAvailability(conditions) ==
             AvailabilityBlock::kNoMission);
  conditions = ReadyMission();
  conditions.network_session = true;
  CHECK_TRUE(ge::testing::detail::MutationAvailability(conditions) ==
             AvailabilityBlock::kNetworkSession);
  conditions = ReadyMission();
  conditions.retail_debug_menu_visible = true;
  CHECK_TRUE(ge::testing::detail::MutationAvailability(conditions) ==
             AvailabilityBlock::kRetailDebugMenu);
  conditions = ReadyMission();
  conditions.pause_requested = false;
  CHECK_TRUE(ge::testing::detail::MutationAvailability(conditions) ==
             AvailabilityBlock::kHostSettingsClosed);

  // Every part of the host-pause acknowledgement is required. It is not
  // enough for input to be captured or for the pause request merely to exist.
  for (size_t field = 0; field < 5; ++field) {
    conditions = ReadyMission();
    switch (field) {
      case 0:
        conditions.pause_request_applied = false;
        break;
      case 1:
        conditions.pause_available = false;
        break;
      case 2:
        conditions.gameplay_paused = false;
        break;
      case 3:
        conditions.host_owned = false;
        break;
      case 4:
        conditions.pause_generation_matches = false;
        break;
    }
    CHECK_TRUE(ge::testing::detail::MutationAvailability(conditions) ==
               AvailabilityBlock::kPausePending);
  }
}

void TestEveryCheatCanBeQueued() {
  RequestQueue queue;
  constexpr uint64_t token = 41;
  for (const CheatDefinition& definition : ge::testing::detail::kCheatDefinitions) {
    CHECK_TRUE(queue.QueueToggle(definition.tool, true, token));
    const PendingToggle enabled = queue.TakeToggle(definition.tool, token);
    CHECK_TRUE(enabled.present);
    CHECK_TRUE(enabled.enabled);
    CHECK_TRUE(enabled.token == token);

    CHECK_TRUE(queue.QueueToggle(definition.tool, false, token));
    const PendingToggle disabled = queue.TakeToggle(definition.tool, token);
    CHECK_TRUE(disabled.present);
    CHECK_TRUE(!disabled.enabled);
  }
}

void TestLatestRequestWins() {
  RequestQueue queue;
  CHECK_TRUE(queue.QueueToggle(Tool::kGodMode, true, 7));
  CHECK_TRUE(queue.QueueToggle(Tool::kGodMode, false, 7));
  const PendingToggle pending = queue.TakeToggle(Tool::kGodMode, 7);
  CHECK_TRUE(pending.present);
  CHECK_TRUE(!pending.enabled);
  CHECK_TRUE(!queue.TakeToggle(Tool::kGodMode, 7).present);
}

void TestOldMissionRequestIsConsumedAndRejected() {
  RequestQueue queue;
  CHECK_TRUE(queue.QueueToggle(Tool::kFrescoMode, true, 100));

  // Token 101 represents a new mission or host-menu pause generation. Taking
  // with it consumes the stale request without applying it. A later attempt to
  // use the old token cannot recover that request.
  CHECK_TRUE(!queue.TakeToggle(Tool::kFrescoMode, 101).present);
  CHECK_TRUE(!queue.TakeToggle(Tool::kFrescoMode, 100).present);

  CHECK_TRUE(queue.QueueToggle(Tool::kVaselineVision, true, 101));
  queue.DiscardAll();
  CHECK_TRUE(!queue.TakeToggle(Tool::kVaselineVision, 101).present);
}

void TestRequestValidationAndActions() {
  RequestQueue queue;
  CHECK_TRUE(!queue.QueueToggle(Tool::kRestartMission, true, 1));
  CHECK_TRUE(!queue.QueueToggle(Tool::kOriginalRemastered, true, 1));
  CHECK_TRUE(!queue.QueueToggle(Tool::kBigHeads, true, 0));
  CHECK_TRUE(!queue.QueueToggle(Tool::kBigHeads, true, RequestQueue::kMaximumToken + 1));

  CHECK_TRUE(queue.QueueAction(Tool::kOriginalRemastered, 12));
  CHECK_TRUE(!queue.TakeAction(Tool::kOriginalRemastered, 13));
  CHECK_TRUE(!queue.TakeAction(Tool::kOriginalRemastered, 12));
  CHECK_TRUE(queue.QueueAction(Tool::kOriginalRemastered, 13));
  CHECK_TRUE(queue.TakeAction(Tool::kOriginalRemastered, 13));
  CHECK_TRUE(!queue.TakeAction(Tool::kOriginalRemastered, 13));
  CHECK_TRUE(!queue.QueueAction(Tool::kGodMode, 13));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void TestRuntimeSourceContract() {
#if defined(GOLDENEYE_TESTING_TOOLS_SOURCE_FILE) && defined(GOLDENEYE_GENERATED_DIRECTORY)
  const std::string source = ReadFile(GOLDENEYE_TESTING_TOOLS_SOURCE_FILE);
  CHECK_TRUE(!source.empty());
  CHECK_TRUE(source.contains("0x830CAEA0u"));
  CHECK_TRUE(source.contains("0x83063318u"));
  CHECK_TRUE(source.contains("sub_82091FB8(context, base)"));
  CHECK_TRUE(source.contains("host_pause::GetSnapshot()"));
  CHECK_TRUE(source.contains("pause.generation == pause.applied_generation"));
  CHECK_TRUE(source.contains("sub_82136FE8(context, base)"));
  CHECK_TRUE(source.contains("sub_82137968(context, base)"));
  CHECK_TRUE(source.contains("sub_8211E008(context, base)"));
  CHECK_TRUE(source.contains("0x82003264u"));

  std::string generated_cheat_source;
  for (const auto& entry : std::filesystem::directory_iterator(GOLDENEYE_GENERATED_DIRECTORY)) {
    const std::string filename = entry.path().filename().string();
    if (!entry.is_regular_file() || !filename.starts_with("ge_recomp.") ||
        entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string candidate = ReadFile(entry.path());
    if (candidate.contains("DEFINE_REX_FUNC(sub_82136FA0)")) {
      generated_cheat_source = candidate;
      break;
    }
  }
  CHECK_TRUE(!generated_cheat_source.empty());
  CHECK_TRUE(generated_cheat_source.contains("addi r11,r11,13080"));
  CHECK_TRUE(generated_cheat_source.contains("lbzx r11,r11,r31"));

  // The supported retail image only dispatches mutation side effects for IDs
  // 1-74. IDs 76-79 still use the same active byte queried above, but fall
  // into the report path after setting/clearing it. This contract justifies
  // the backend's narrow query-driven-bit special case for those four IDs.
  const size_t enable_start = generated_cheat_source.find("DEFINE_REX_FUNC(sub_82136FE8)");
  const size_t disable_start = generated_cheat_source.find("DEFINE_REX_FUNC(sub_82137968)");
  CHECK_TRUE(enable_start != std::string::npos);
  CHECK_TRUE(disable_start != std::string::npos);
  if (enable_start != std::string::npos && disable_start != std::string::npos) {
    const std::string enable =
        generated_cheat_source.substr(enable_start, disable_start - enable_start);
    CHECK_TRUE(enable.contains("ctx.cr6.compare<uint32_t>(ctx.r11.u32, 73"));
    CHECK_TRUE(enable.contains("REX_STORE_U8(ctx.r11.u32 + ctx.r31.u32"));
    CHECK_TRUE(enable.contains("sub_823ED380(ctx, base)"));
  }
#else
  CHECK_TRUE(false);
#endif
}

}  // namespace

int main() {
  TestMutationAvailability();
  TestEveryCheatCanBeQueued();
  TestLatestRequestWins();
  TestOldMissionRequestIsConsumedAndRejected();
  TestRequestValidationAndActions();
  TestRuntimeSourceContract();
  return failures == 0 ? 0 : 1;
}
