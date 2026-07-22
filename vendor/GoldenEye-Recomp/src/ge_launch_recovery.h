// GoldenEye Metal crash-recovery state.
//
// A run marker is created only when the launcher hands control to the game and
// is removed at the accepted clean-close boundary. Safe Mode snapshots the
// player's config before applying temporary in-memory cvar overrides; the
// snapshot is restored before config loading on the following process launch.

#pragma once

#include <filesystem>
#include <string>

namespace ge::launch_recovery {

struct StartupResult {
  bool interrupted_run = false;
  bool restored_safe_mode_config = false;
  std::string warning;
};

// Must run after the writable paths are resolved, but before the runtime loads
// config_path. Restores a previous Safe Mode snapshot first, then reports
// whether the prior game run failed to reach the clean-close boundary.
StartupResult PrepareStartup(const std::filesystem::path& user_data_root,
                             const std::filesystem::path& config_path);

// Atomically publishes the durable marker for a game run. Existing interrupted
// markers are replaced only after the new marker is safely on disk.
bool BeginRun(const std::filesystem::path& user_data_root, std::string* error);

// Removes the run marker durably. This is intentionally not signal-safe and is
// called only from an accepted AppKit close notification or ordinary teardown.
bool EndRun(const std::filesystem::path& user_data_root);

// Saves the exact pre-Safe-Mode config (or records that none existed). The
// caller applies temporary cvar overrides only after this succeeds.
bool BeginSafeMode(const std::filesystem::path& user_data_root,
                   const std::filesystem::path& config_path, std::string* error);

// Rolls back a Safe Mode snapshot if launching the game could not begin.
bool CancelSafeMode(const std::filesystem::path& user_data_root,
                    const std::filesystem::path& config_path);

}  // namespace ge::launch_recovery
