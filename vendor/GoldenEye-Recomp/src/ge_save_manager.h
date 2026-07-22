// GoldenEye Metal macOS save backup, restore, and reset backend.
//
// This API deliberately knows only the save/profile layout produced by this
// app's ReXGlue runtime. It never scans or copies Game Data, Cache, Logs, the
// launcher config, other title IDs, or arbitrary paths below Application
// Support. Callers must only invoke mutating operations while the game runtime
// is stopped.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ge::save {

inline constexpr std::string_view kBackupExtension = ".gesave";

enum class Error {
  kNone,
  kInvalidArgument,
  kNoData,
  kUnsafeLayout,
  kInvalidArchive,
  kConflict,
  kIo,
};

struct Status {
  Error error = Error::kNone;
  std::string message;

  explicit operator bool() const { return error == Error::kNone; }
};

struct Snapshot {
  bool has_save_game = false;
  bool has_profile_settings = false;
  size_t file_count = 0;
  uint64_t byte_count = 0;

  explicit operator bool() const { return has_save_game || has_profile_settings; }
};

struct BackupInfo : Snapshot {
  uint32_t format_version = 0;
  uint64_t created_unix_seconds = 0;
};

struct MutationResult {
  Status status;

  // Restore preserves the replaced data here; reset moves the reset data here.
  // A successful undo of a restore preserves the displaced post-restore data
  // here, which also permits a transactional redo. Empty means the operation
  // replaced no prior data. The directory is inside the app's user-data root
  // and may be passed to UndoQuarantine.
  std::filesystem::path quarantine_path;

  explicit operator bool() const { return static_cast<bool>(status); }
};

struct RecoveryResult {
  Status status;
  bool recovered = false;

  explicit operator bool() const { return static_cast<bool>(status); }
};

// Reconciles an interrupted save transaction before gameplay can observe a
// split live/quarantine snapshot. It is idempotent and serialized across app
// instances. On failure, the journal and every recoverable data copy remain in
// place; callers must not start the game until recovery succeeds.
RecoveryResult RecoverInterruptedTransaction(const std::filesystem::path& user_data_root);

// Enumerates and validates only the known GoldenEye save/profile files under
// user_data_root. Unknown entries within those managed directories, symlinks,
// partial save packages, and malformed content headers are rejected.
Status Discover(const std::filesystem::path& user_data_root, Snapshot* snapshot);

// Writes a versioned, path-bound SHA-256 archive using a temporary sibling and
// atomic rename. destination must not already exist and must be outside
// user_data_root so a reset cannot move or hide the backup.
Status CreateBackup(const std::filesystem::path& user_data_root,
                    const std::filesystem::path& destination, BackupInfo* info = nullptr);

// Fully validates an archive without changing live data.
Status InspectBackup(const std::filesystem::path& archive, BackupInfo* info = nullptr);

// Restores an already validated archive through a staging directory. Existing
// managed data is moved to quarantine first and every completed rename is
// rolled back if installation fails.
MutationResult RestoreBackup(const std::filesystem::path& user_data_root,
                             const std::filesystem::path& archive);

// Reset is deletion-free: managed data is atomically renamed into a versioned
// quarantine directory. Unknown or unsafe layouts are refused.
MutationResult ResetToFresh(const std::filesystem::path& user_data_root);

// Restores a quarantine created by ResetToFresh or RestoreBackup. Reset undo
// refuses to overwrite a current unit. Restore undo transactionally swaps the
// current snapshot into a new quarantine, allowing immediate undo/redo.
MutationResult UndoQuarantine(const std::filesystem::path& user_data_root,
                              const std::filesystem::path& quarantine_path);

#if defined(GOLDENEYE_SAVE_MANAGER_TESTING)
namespace testing {
void SetCrashAfterCheckpoint(int checkpoints);
}
#endif

}  // namespace ge::save
