#include "ge_game_data.h"

#include <rex/crypto/sha256.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <queue>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ge::game_data {
namespace {

constexpr uint64_t kMaximumEntryCount = 5000;
constexpr uint64_t kMaximumExtractedBytes = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kFreeSpaceReserve = 128ull * 1024ull * 1024ull;
constexpr size_t kCopyBufferSize = 4ull * 1024ull * 1024ull;
constexpr uint64_t kExpectedGameFileCount = 1803;
constexpr uint64_t kSupportedPackageBytes = 739069952;
constexpr uint64_t kPackagedXexBytes = 7266304;
constexpr uint64_t kExpandedXexBytes = 15872000;
constexpr uint64_t kPackagedGameBytes = 730208338;
constexpr uint64_t kExpandedGameBytes = 738814034;
constexpr size_t kMaximumImportMarkerBytes = 1024;
constexpr std::string_view kImportMarkerName = ".goldeneye-metal-game-data";

struct DirectoryStats {
  uint64_t file_count = 0;
  uint64_t file_bytes = 0;
};

class ScopedPathCleanup {
 public:
  explicit ScopedPathCleanup(std::filesystem::path path) : path_(std::move(path)) {}
  ~ScopedPathCleanup() {
    if (active_) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  void Release() { active_ = false; }

 private:
  std::filesystem::path path_;
  bool active_ = true;
};

struct PlannedEntry {
  rex::filesystem::Entry* entry = nullptr;
  std::filesystem::path relative_path;
  bool directory = false;
};

struct ExtractionPlan {
  std::vector<PlannedEntry> entries;
  uint64_t file_bytes = 0;
};

void Report(const ProgressCallback& callback, std::string message, uint64_t completed = 0,
            uint64_t total = 0) {
  if (callback) {
    callback(Progress{std::move(message), completed, total});
  }
}

std::string Lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

ValidationResult CollectDirectoryStats(const std::filesystem::path& root, DirectoryStats* stats) {
  std::error_code ec;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  std::filesystem::recursive_directory_iterator end;
  if (ec) {
    return {false, "The selected game-data folder could not be inspected."};
  }

  while (iterator != end) {
    auto status = iterator->symlink_status(ec);
    if (ec) {
      return {false, "The selected game-data folder could not be inspected."};
    }
    if (std::filesystem::is_symlink(status)) {
      return {false, "The selected game-data folder contains an unsupported symbolic link."};
    }
    if (std::filesystem::is_regular_file(status)) {
      auto relative = std::filesystem::relative(iterator->path(), root, ec);
      if (ec) {
        return {false, "The selected game-data folder contains an invalid path."};
      }
      if (relative.generic_string() != kImportMarkerName &&
          iterator->path().filename() != ".DS_Store") {
        uint64_t size = iterator->file_size(ec);
        if (ec || size > std::numeric_limits<uint64_t>::max() - stats->file_bytes) {
          return {false, "The selected game-data folder contains an unreadable file."};
        }
        ++stats->file_count;
        stats->file_bytes += size;
        if (stats->file_count > kMaximumEntryCount || stats->file_bytes > kMaximumExtractedBytes) {
          return {false, "The selected game-data folder has an unexpected size or layout."};
        }
      }
    }
    iterator.increment(ec);
    if (ec) {
      return {false, "The selected game-data folder could not be inspected."};
    }
  }
  return {true, {}};
}

ValidationResult ValidateDirectoryImpl(const std::filesystem::path& root, DirectoryStats* stats) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    return {false, "Choose a folder that directly contains the GoldenEye game data."};
  }

  constexpr std::array<std::string_view, 7> required_directories = {
      "files",          "files/loc/english", "files/new",           "files/new/texture",
      "files/original", "files/texture",     "files/texture/level",
  };
  for (std::string_view relative : required_directories) {
    if (!std::filesystem::is_directory(root / relative, ec)) {
      return {false, "The selected folder is missing required GoldenEye game data."};
    }
  }

  constexpr std::array<std::pair<std::string_view, uint64_t>, 7> required_files = {{
      {"ArcadeInfo.xml", 2388},
      {"music.xgs", 547},
      {"music.xsb", 4645},
      {"music.xwb", 70778880},
      {"sfx.xgs", 554},
      {"sfx.xsb", 11372},
      {"sfx.xwb", 24065828},
  }};
  for (const auto& [relative, expected_size] : required_files) {
    auto path = root / relative;
    if (!std::filesystem::is_regular_file(path, ec) ||
        std::filesystem::file_size(path, ec) != expected_size || ec) {
      return {false, "The selected folder has missing or incomplete GoldenEye resource files."};
    }
  }
  auto xex_path = root / "default.xex";
  if (!std::filesystem::is_regular_file(xex_path, ec)) {
    return {false, "The selected folder must directly contain default.xex and files/."};
  }

  uint64_t xex_size = std::filesystem::file_size(xex_path, ec);
  if (ec || (xex_size != kPackagedXexBytes && xex_size != kExpandedXexBytes)) {
    return {false, "This game-data folder contains a different or unsupported default.xex build."};
  }

  std::string xex_hash = rex::crypto::sha256_file(xex_path);
  uint64_t expected_bytes = 0;
  if (xex_hash == kPackagedXexSha256) {
    expected_bytes = kPackagedGameBytes;
  } else if (xex_hash == kExpandedXexSha256) {
    expected_bytes = kExpandedGameBytes;
  } else {
    return {false, "This game-data folder contains a different or unsupported default.xex build."};
  }

  auto stats_result = CollectDirectoryStats(root, stats);
  if (!stats_result.valid) {
    return stats_result;
  }
  if (stats->file_count < kExpectedGameFileCount || stats->file_bytes < expected_bytes) {
    return {false, "The selected game-data folder is incomplete."};
  }
  return {true, {}};
}

bool IsSafeComponent(std::string_view name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  return name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

std::filesystem::path TemporarySibling(const std::filesystem::path& destination,
                                       std::string_view suffix) {
  auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
  auto process = 0;
#else
  auto process = static_cast<long long>(getpid());
#endif
  return destination.parent_path() / (destination.filename().string() + "." + std::string(suffix) +
                                      "-" + std::to_string(process) + "-" + std::to_string(tick));
}

ValidationResult BuildPlan(rex::filesystem::Entry* root, ExtractionPlan* plan) {
  if (!root || !(root->attributes() & rex::filesystem::kFileAttributeDirectory)) {
    return {false, "The Xbox package does not contain a readable root directory."};
  }

  struct Pending {
    rex::filesystem::Entry* entry;
    std::filesystem::path parent;
    size_t depth;
  };
  std::queue<Pending> pending;
  for (const auto& child : root->children()) {
    pending.push({child.get(), {}, 1});
  }

  std::unordered_set<std::string> casefolded_paths;
  bool found_xex = false;
  bool found_files = false;
  while (!pending.empty()) {
    Pending item = std::move(pending.front());
    pending.pop();

    if (item.depth > 64 || !IsSafeComponent(item.entry->name())) {
      return {false, "The Xbox package contains an unsafe file path."};
    }

    auto relative = item.parent / item.entry->name();
    auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized.is_absolute()) {
      return {false, "The Xbox package contains an invalid file path."};
    }
    for (const auto& component : normalized) {
      if (component == "..") {
        return {false, "The Xbox package contains a path outside its game-data root."};
      }
    }

    std::string folded = Lowercase(normalized.generic_string());
    if (!casefolded_paths.emplace(folded).second) {
      return {false, "The Xbox package contains duplicate or case-colliding paths."};
    }

    bool directory = (item.entry->attributes() & rex::filesystem::kFileAttributeDirectory) != 0;
    plan->entries.push_back({item.entry, normalized, directory});
    if (plan->entries.size() > kMaximumEntryCount) {
      return {false, "The Xbox package contains an unexpected number of files."};
    }

    if (directory) {
      if (folded == "files") {
        found_files = true;
      }
      for (const auto& child : item.entry->children()) {
        pending.push({child.get(), normalized, item.depth + 1});
      }
    } else {
      if (folded == "default.xex") {
        found_xex = true;
      }
      if (item.entry->size() > kMaximumExtractedBytes - plan->file_bytes) {
        return {false, "The Xbox package is larger than the supported game-data build."};
      }
      plan->file_bytes += item.entry->size();
    }
  }

  if (!found_xex || !found_files) {
    return {false, "The Xbox package is missing default.xex or the files directory."};
  }
  return {true, {}};
}

bool WriteEntry(const PlannedEntry& planned, const std::filesystem::path& staging,
                std::vector<uint8_t>* buffer, uint64_t* completed, uint64_t total,
                const ProgressCallback& progress, const CancellationCallback& cancelled,
                std::string* error) {
  auto destination = staging / planned.relative_path;
  std::error_code ec;
  if (planned.directory) {
    std::filesystem::create_directories(destination, ec);
    if (ec) {
      *error = "Could not create a game-data directory: " + ec.message();
      return false;
    }
    return true;
  }

  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    *error = "Could not create a game-data directory: " + ec.message();
    return false;
  }

  rex::filesystem::File* source = nullptr;
  rex::X_STATUS status = planned.entry->Open(rex::filesystem::FileAccess::kFileReadData, &source);
  if (status != 0 || !source) {
    *error = "Could not read " + planned.relative_path.generic_string() + " from the Xbox package.";
    return false;
  }

  FILE* output = rex::filesystem::OpenFile(destination, "wb");
  if (!output) {
    source->Destroy();
    *error = "Could not write " + planned.relative_path.generic_string() + ".";
    return false;
  }

  bool success = true;
  size_t offset = 0;
  size_t remaining = planned.entry->size();
  while (remaining != 0) {
    if (cancelled && cancelled()) {
      *error = "Import cancelled.";
      success = false;
      break;
    }
    size_t requested = std::min(remaining, buffer->size());
    size_t bytes_read = 0;
    status = source->ReadSync(std::span<uint8_t>(buffer->data(), requested), offset, &bytes_read);
    if (status != 0 || bytes_read == 0 || bytes_read > requested ||
        std::fwrite(buffer->data(), 1, bytes_read, output) != bytes_read) {
      success = false;
      break;
    }
    offset += bytes_read;
    remaining -= bytes_read;
    *completed += bytes_read;
    Report(progress, "Extracting game data…", *completed, total);
  }

  if (std::fclose(output) != 0) {
    success = false;
  }
  source->Destroy();
  if (!success || remaining != 0) {
    std::filesystem::remove(destination, ec);
    if (error->empty()) {
      *error = "The Xbox package ended unexpectedly while reading " +
               planned.relative_path.generic_string() + ".";
    }
    return false;
  }
  return true;
}

bool PublishAtomically(const std::filesystem::path& staging,
                       const std::filesystem::path& destination, std::string* error) {
  std::error_code ec;
  std::filesystem::path previous;
  if (std::filesystem::exists(destination, ec)) {
    previous = TemporarySibling(destination, "previous");
    std::filesystem::rename(destination, previous, ec);
    if (ec) {
      *error = "Could not replace the existing game-data cache: " + ec.message();
      return false;
    }
  }

  std::filesystem::rename(staging, destination, ec);
  if (ec) {
    if (!previous.empty()) {
      std::error_code restore_error;
      std::filesystem::rename(previous, destination, restore_error);
    }
    *error = "Could not finish installing the game data: " + ec.message();
    return false;
  }
  if (!previous.empty()) {
    std::filesystem::remove_all(previous, ec);
  }
  return true;
}

}  // namespace

ValidationResult ValidateDirectory(const std::filesystem::path& root) {
  DirectoryStats stats;
  return ValidateDirectoryImpl(root, &stats);
}

ValidationResult ValidateImportedDirectory(const std::filesystem::path& root) {
  DirectoryStats stats;
  auto validation = ValidateDirectoryImpl(root, &stats);
  if (!validation.valid) {
    return validation;
  }
  if (stats.file_count != kExpectedGameFileCount || stats.file_bytes != kPackagedGameBytes) {
    return {false, "The imported game-data cache is incomplete or has been modified."};
  }

  std::ostringstream expected;
  expected << "format=1\n"
           << "title_id=584108A9\n"
           << "package_sha256=" << kSupportedPackageSha256 << "\n"
           << "xex_sha256=" << kPackagedXexSha256 << "\n"
           << "file_count=" << kExpectedGameFileCount << "\n"
           << "file_bytes=" << kPackagedGameBytes << "\n";
  auto marker_path = root / kImportMarkerName;
  std::error_code ec;
  auto marker_status = std::filesystem::symlink_status(marker_path, ec);
  if (ec || !std::filesystem::is_regular_file(marker_status)) {
    return {false, "The imported game-data cache is missing its validation record."};
  }

  std::ifstream marker(marker_path, std::ios::binary);
  std::array<char, kMaximumImportMarkerBytes + 1> marker_buffer{};
  marker.read(marker_buffer.data(), marker_buffer.size());
  size_t marker_size = static_cast<size_t>(marker.gcount());
  if (marker.bad() || marker_size > kMaximumImportMarkerBytes ||
      std::string_view(marker_buffer.data(), marker_size) != expected.str()) {
    return {false, "The imported game-data cache is missing its validation record."};
  }
  return {true, {}};
}

ValidationResult ValidatePackage(const std::filesystem::path& package_path,
                                 const ProgressCallback& progress,
                                 const CancellationCallback& cancelled) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(package_path, ec)) {
    return {false, "The selected Xbox package is not a readable file."};
  }
  uint64_t package_size = std::filesystem::file_size(package_path, ec);
  if (ec || package_size != kSupportedPackageBytes) {
    return {false, "This is not the supported GoldenEye Xbox package revision."};
  }

  Report(progress, "Verifying Xbox package…");
  std::string package_hash = rex::crypto::sha256_file(package_path, cancelled);
  if (package_hash.empty()) {
    if (cancelled && cancelled()) {
      return {false, "Import cancelled."};
    }
    return {false, "The selected Xbox package could not be read."};
  }
  if (package_hash != kSupportedPackageSha256) {
    return {false, "This is not the supported GoldenEye Xbox package revision."};
  }

  auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(package_path);
  if (!header || !header->header.is_magic_valid()) {
    return {false, "The selected file is not a valid Xbox LIVE/STFS package."};
  }
  if (static_cast<uint32_t>(header->metadata.execution_info.title_id) != kGoldenEyeTitleId) {
    return {false, "The selected Xbox package is for a different title."};
  }
  return {true, {}};
}

ImportResult ImportPackage(const std::filesystem::path& package_path,
                           const std::filesystem::path& destination,
                           const ProgressCallback& progress,
                           const CancellationCallback& cancelled) {
  try {
    auto package_validation = ValidatePackage(package_path, progress, cancelled);
    if (!package_validation.valid) {
      return {{}, package_validation.error};
    }
    if (cancelled && cancelled()) {
      return {{}, "Import cancelled."};
    }

    auto existing = ValidateImportedDirectory(destination);
    if (existing.valid) {
      Report(progress, "Using existing game data.", 1, 1);
      return {destination, {}};
    }

    Report(progress, "Reading Xbox package…");
    auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package_path);
    if (!device->Initialize()) {
      return {{}, "The Xbox package could not be opened."};
    }

    ExtractionPlan plan;
    auto plan_validation = BuildPlan(device->ResolvePath(""), &plan);
    if (!plan_validation.valid) {
      return {{}, plan_validation.error};
    }
    if (cancelled && cancelled()) {
      return {{}, "Import cancelled."};
    }

    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
      return {{},
              "Could not create the GoldenEye Metal Application Support folder: " + ec.message()};
    }
    auto space = std::filesystem::space(destination.parent_path(), ec);
    if (!ec && space.available < plan.file_bytes + kFreeSpaceReserve) {
      return {{}, "There is not enough free disk space to import the game data."};
    }

    auto staging = TemporarySibling(destination, "importing");
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
      return {{}, "Could not create a temporary game-data directory: " + ec.message()};
    }
    ScopedPathCleanup staging_cleanup(staging);
#if !defined(_WIN32)
    chmod(staging.c_str(), 0700);
#endif

    std::vector<uint8_t> buffer(kCopyBufferSize);
    uint64_t completed = 0;
    std::string error;
    for (const auto& planned : plan.entries) {
      if (!WriteEntry(planned, staging, &buffer, &completed, plan.file_bytes, progress, cancelled,
                      &error)) {
        return {{}, error};
      }
    }

    Report(progress, "Validating imported game data…", completed, plan.file_bytes);
    auto imported_validation = ValidateDirectory(staging);
    if (!imported_validation.valid) {
      return {{}, imported_validation.error};
    }

    {
      std::ofstream marker(staging / kImportMarkerName, std::ios::binary | std::ios::trunc);
      marker << "format=1\n"
             << "title_id=584108A9\n"
             << "package_sha256=" << kSupportedPackageSha256 << "\n"
             << "xex_sha256=" << kPackagedXexSha256 << "\n"
             << "file_count=" << kExpectedGameFileCount << "\n"
             << "file_bytes=" << kPackagedGameBytes << "\n";
      marker.flush();
      marker.close();
      if (!marker) {
        return {{}, "Could not finish the local game-data installation."};
      }
    }

    imported_validation = ValidateImportedDirectory(staging);
    if (!imported_validation.valid) {
      return {{}, imported_validation.error};
    }
    if (cancelled && cancelled()) {
      return {{}, "Import cancelled."};
    }

    if (!PublishAtomically(staging, destination, &error)) {
      return {{}, error};
    }
    staging_cleanup.Release();

    Report(progress, "Game data is ready.", plan.file_bytes, plan.file_bytes);
    return {destination, {}};
  } catch (const std::exception& exception) {
    return {{}, std::string("The game-data import failed: ") + exception.what()};
  } catch (...) {
    return {{}, "The game-data import failed unexpectedly."};
  }
}

}  // namespace ge::game_data
