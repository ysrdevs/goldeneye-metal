#include "ge_launch_recovery.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ge::launch_recovery {
namespace {

constexpr std::string_view kRunMarkerName = ".goldeneye-metal-run-active";
constexpr std::string_view kSafeModeMarkerName = ".goldeneye-metal-safe-mode-active";
constexpr std::string_view kSafeModeConfigBackupName =
    ".goldeneye-metal-safe-mode-config-backup";
constexpr std::string_view kRunMarkerHeader = "goldeneye-metal-run-v1\n";
constexpr std::string_view kSafeModeConfigPresent =
    "goldeneye-metal-safe-mode-v1\nconfig=present\n";
constexpr std::string_view kSafeModeConfigAbsent =
    "goldeneye-metal-safe-mode-v1\nconfig=absent\n";
constexpr uint64_t kMaximumConfigBytes = 4ull * 1024ull * 1024ull;

std::atomic<uint64_t> g_temporary_counter{0};

int ProcessId() {
  return getpid();
}

std::filesystem::path RunMarkerPath(const std::filesystem::path& root) {
  return root / kRunMarkerName;
}

std::filesystem::path SafeModeMarkerPath(const std::filesystem::path& root) {
  return root / kSafeModeMarkerName;
}

std::filesystem::path SafeModeBackupPath(const std::filesystem::path& root) {
  return root / kSafeModeConfigBackupName;
}

bool IsRegularFileNoFollow(const std::filesystem::path& path) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  return !ec && std::filesystem::is_regular_file(status);
}

bool ExistsNoFollow(const std::filesystem::path& path) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  return !ec && std::filesystem::exists(status);
}

void SyncDirectory(const std::filesystem::path& directory) {
  int descriptor = open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor >= 0) {
    (void)fsync(descriptor);
    (void)close(descriptor);
  }
}

bool RemoveRegularFile(const std::filesystem::path& path) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  if (ec || !std::filesystem::exists(status)) {
    return !ec || ec == std::errc::no_such_file_or_directory;
  }
  if (!std::filesystem::is_regular_file(status)) {
    return false;
  }
  if (!std::filesystem::remove(path, ec) || ec) {
    return false;
  }
  SyncDirectory(path.parent_path());
  return true;
}

std::optional<std::string> ReadBoundedRegularFile(const std::filesystem::path& path,
                                                  uint64_t maximum_bytes) {
  int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return std::nullopt;
  }

  struct stat status {};
  bool valid = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) && status.st_size >= 0 &&
               static_cast<uint64_t>(status.st_size) <= maximum_bytes;
  if (!valid) {
    close(descriptor);
    return std::nullopt;
  }

  std::string contents(static_cast<size_t>(status.st_size), '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    ssize_t count = read(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      contents.clear();
      break;
    }
    offset += static_cast<size_t>(count);
  }
  close(descriptor);
  if (offset != static_cast<size_t>(status.st_size)) {
    return std::nullopt;
  }
  return contents;
}

bool WriteAtomicPrivateFile(const std::filesystem::path& destination, std::string_view contents,
                            std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (error) {
      *error = "Could not create the private recovery-state directory.";
    }
    return false;
  }

  const uint64_t counter = g_temporary_counter.fetch_add(1, std::memory_order_relaxed);
  auto temporary = destination.parent_path() /
                   ("." + destination.filename().string() + ".tmp-" +
                    std::to_string(ProcessId()) + "-" + std::to_string(counter));
  int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                        S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    if (error) {
      *error = "Could not create a private recovery-state file.";
    }
    return false;
  }

  size_t offset = 0;
  bool success = true;
  while (offset < contents.size()) {
    ssize_t count = write(descriptor, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      success = false;
      break;
    }
    offset += static_cast<size_t>(count);
  }
  if (success && fsync(descriptor) != 0) {
    success = false;
  }
  if (close(descriptor) != 0) {
    success = false;
  }

  if (success) {
    // std::filesystem::rename atomically replaces files on POSIX, the only
    // platform where this launcher backend is built.
    std::filesystem::rename(temporary, destination, ec);
    success = !ec;
  }
  if (!success) {
    std::filesystem::remove(temporary, ec);
    if (error) {
      *error = "Could not publish the private recovery-state file.";
    }
    return false;
  }
  SyncDirectory(destination.parent_path());
  return true;
}

bool RestoreSafeModeConfig(const std::filesystem::path& root,
                           const std::filesystem::path& config_path, bool* restored,
                           std::string* warning) {
  *restored = false;
  const auto marker_path = SafeModeMarkerPath(root);
  if (!ExistsNoFollow(marker_path)) {
    // A backup without its commit marker was never activated and is safe to
    // discard. Never follow or replace an unexpected filesystem object.
    const auto backup_path = SafeModeBackupPath(root);
    if (IsRegularFileNoFollow(backup_path)) {
      RemoveRegularFile(backup_path);
    }
    return true;
  }

  auto marker = ReadBoundedRegularFile(marker_path, 256);
  if (!marker || (*marker != kSafeModeConfigPresent && *marker != kSafeModeConfigAbsent)) {
    *warning = "Safe Mode recovery state is damaged; the saved configuration was not changed.";
    return false;
  }

  if (*marker == kSafeModeConfigPresent) {
    auto backup = ReadBoundedRegularFile(SafeModeBackupPath(root), kMaximumConfigBytes);
    std::error_code config_ec;
    auto config_status = std::filesystem::symlink_status(config_path, config_ec);
    if (!config_ec && std::filesystem::exists(config_status) &&
        !std::filesystem::is_regular_file(config_status)) {
      *warning = "The settings path changed unexpectedly; the pre-Safe-Mode configuration was "
                 "not restored.";
      return false;
    }
    if (config_ec && config_ec != std::errc::no_such_file_or_directory) {
      *warning = "The settings path could not be safely inspected for restoration.";
      return false;
    }
    if (!backup || !WriteAtomicPrivateFile(config_path, *backup, nullptr)) {
      *warning = "The pre-Safe-Mode configuration could not be restored.";
      return false;
    }
  } else {
    std::error_code ec;
    auto status = std::filesystem::symlink_status(config_path, ec);
    if (!ec && std::filesystem::exists(status)) {
      if (!std::filesystem::is_regular_file(status) || !RemoveRegularFile(config_path)) {
        *warning = "The temporary Safe Mode configuration could not be removed.";
        return false;
      }
    } else if (ec && ec != std::errc::no_such_file_or_directory) {
      *warning = "The temporary Safe Mode configuration could not be inspected.";
      return false;
    }
  }

  if (!RemoveRegularFile(marker_path)) {
    *warning = "The Safe Mode configuration was restored, but its state marker remains.";
    return false;
  }
  const auto backup_path = SafeModeBackupPath(root);
  if (IsRegularFileNoFollow(backup_path)) {
    RemoveRegularFile(backup_path);
  }
  *restored = true;
  return true;
}

}  // namespace

StartupResult PrepareStartup(const std::filesystem::path& user_data_root,
                             const std::filesystem::path& config_path) {
  StartupResult result;
  std::error_code ec;
  std::filesystem::create_directories(user_data_root, ec);
  if (ec) {
    result.warning = "Crash recovery could not access the private Application Support folder.";
    return result;
  }

  RestoreSafeModeConfig(user_data_root, config_path, &result.restored_safe_mode_config,
                        &result.warning);
  result.interrupted_run = ExistsNoFollow(RunMarkerPath(user_data_root));
  return result;
}

bool BeginRun(const std::filesystem::path& user_data_root, std::string* error) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  std::string marker(kRunMarkerHeader);
  marker += "pid=" + std::to_string(ProcessId()) + "\n";
  marker += "started_unix=" + std::to_string(seconds) + "\n";
  return WriteAtomicPrivateFile(RunMarkerPath(user_data_root), marker, error);
}

bool EndRun(const std::filesystem::path& user_data_root) {
  return RemoveRegularFile(RunMarkerPath(user_data_root));
}

bool BeginSafeMode(const std::filesystem::path& user_data_root,
                   const std::filesystem::path& config_path, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(user_data_root, ec);
  if (ec) {
    if (error) {
      *error = "Could not prepare the private Safe Mode state.";
    }
    return false;
  }

  std::string_view marker = kSafeModeConfigAbsent;
  auto config_status = std::filesystem::symlink_status(config_path, ec);
  if (!ec && std::filesystem::exists(config_status)) {
    if (!std::filesystem::is_regular_file(config_status)) {
      if (error) {
        *error = "The settings path is not a regular private file; Safe Mode was not started.";
      }
      return false;
    }
    auto config = ReadBoundedRegularFile(config_path, kMaximumConfigBytes);
    if (!config) {
      if (error) {
        *error = "The settings file could not be safely backed up for Safe Mode.";
      }
      return false;
    }
    if (!WriteAtomicPrivateFile(SafeModeBackupPath(user_data_root), *config, error)) {
      return false;
    }
    marker = kSafeModeConfigPresent;
  } else if (ec && ec != std::errc::no_such_file_or_directory) {
    if (error) {
      *error = "The settings file could not be inspected for Safe Mode.";
    }
    return false;
  } else {
    const auto backup = SafeModeBackupPath(user_data_root);
    if (IsRegularFileNoFollow(backup)) {
      RemoveRegularFile(backup);
    }
  }

  if (!WriteAtomicPrivateFile(SafeModeMarkerPath(user_data_root), marker, error)) {
    return false;
  }
  return true;
}

bool CancelSafeMode(const std::filesystem::path& user_data_root,
                    const std::filesystem::path& config_path) {
  bool restored = false;
  std::string warning;
  return RestoreSafeModeConfig(user_data_root, config_path, &restored, &warning);
}

}  // namespace ge::launch_recovery
