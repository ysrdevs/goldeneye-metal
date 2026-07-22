#include "ge_save_manager.h"

#include <rex/crypto/sha256.h>
#include <rex/system/xam/content_manager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/stdio.h>
#endif

namespace ge::save {
namespace {

using rex::system::XContentType;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

constexpr uint32_t kArchiveVersion = 1;
constexpr uint32_t kQuarantineVersion = 1;
constexpr uint32_t kTransactionVersion = 1;
constexpr uint64_t kGoldenEyeXuid = 0xB13EBABEBABEBABEull;
constexpr uint32_t kGoldenEyeTitleId = 0x584108A9u;
constexpr uint64_t kGoldenEyeSaveBytes = 512;
constexpr uint64_t kMaximumProfileSettingBytes = 1024 * 1024;
constexpr uint64_t kMaximumThumbnailBytes = 2 * 1024 * 1024;
constexpr uint64_t kMaximumManagedBytes = 16 * 1024 * 1024;
constexpr uint64_t kMaximumArchiveBytes = kMaximumManagedBytes + 128 * 1024;
constexpr size_t kMaximumEntries = 128;
constexpr size_t kMaximumRelativePathBytes = 256;

constexpr std::array<uint8_t, 16> kArchiveMagic = {'G', 'E', 'M', 'E',  'T',  'A',  'L', 'S',
                                                   'A', 'V', 'E', 0x1A, '\r', '\n', 0,   0};
constexpr std::array<uint8_t, 16> kQuarantineMagic = {'G', 'E', 'Q', 'U', 'A',  'R',  'A', 'N',
                                                      'T', 'I', 'N', 'E', 0x1A, '\n', 0,   0};
constexpr std::array<uint8_t, 16> kTransactionMagic = {'G', 'E', 'T',  'R',  'A',  'N', 'S', 'A',
                                                       'C', 'T', 0x1A, '\r', '\n', 0,   0,   0};

const std::filesystem::path kSavePackageDirectory =
    "B13EBABEBABEBABE/584108A9/00000001/beansave.dat";
const std::filesystem::path kSavePayload = kSavePackageDirectory / "beansave.dat";
const std::filesystem::path kSaveThumbnail = kSavePackageDirectory / "__thumbnail.png";
const std::filesystem::path kSaveHeader =
    "B13EBABEBABEBABE/584108A9/Headers/00000001/beansave.dat.header";
const std::filesystem::path kProfileRoot = "584108A9/profile";
const std::filesystem::path kQuarantineBase = "Save Data Quarantine";
const std::filesystem::path kQuarantineManifest = ".goldeneye-quarantine";
const std::filesystem::path kTransactionJournal = ".goldeneye-save-transaction";
const std::filesystem::path kOperationLock = ".goldeneye-save-manager.lock";

constexpr std::array<std::string_view, 3> kProfileSettingNames = {"63E83FFD", "63E83FFE",
                                                                  "63E83FFF"};

enum class EntryKind {
  kUnknown,
  kSavePayload,
  kSaveThumbnail,
  kSaveHeader,
  kProfileSetting,
};

enum class QuarantineKind : uint32_t {
  kReset = 1,
  kRestore = 2,
};

enum class TransactionMode : uint32_t {
  kQuarantineToRoot = 1,
  kRestoreInstalling = 2,
  kUndoReset = 3,
  kUndoDisplacing = 4,
  kUndoRestoring = 5,
};

enum class TransactionState : uint32_t {
  kPrecommit = 1,
  kCommitted = 2,
};

struct Entry {
  std::filesystem::path relative_path;
  std::vector<uint8_t> data;
};

struct ParsedArchive {
  BackupInfo info;
  std::vector<Entry> entries;
};

struct QuarantineRecord {
  QuarantineKind kind = QuarantineKind::kReset;
  std::vector<std::filesystem::path> units;
};

struct TransactionRecord {
  TransactionMode mode = TransactionMode::kQuarantineToRoot;
  TransactionState state = TransactionState::kPrecommit;
  std::filesystem::path first_directory;
  std::filesystem::path second_directory;
  std::vector<std::filesystem::path> first_units;
  std::vector<std::filesystem::path> second_units;
};

Status Failure(Error error, std::string message) {
  return {error, std::move(message)};
}

Status Success(std::string message = {}) {
  return {Error::kNone, std::move(message)};
}

std::string PathText(const std::filesystem::path& path) {
  return path.generic_string();
}

bool IsMissing(const std::error_code& ec) {
  return ec == std::errc::no_such_file_or_directory;
}

bool IsPathInside(const std::filesystem::path& candidate, const std::filesystem::path& parent) {
  auto candidate_it = candidate.begin();
  auto parent_it = parent.begin();
  for (; parent_it != parent.end(); ++parent_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *candidate_it != *parent_it) {
      return false;
    }
  }
  return true;
}

Status AbsoluteNormalized(const std::filesystem::path& path, std::filesystem::path* normalized) {
  if (!normalized || path.empty()) {
    return Failure(Error::kInvalidArgument, "A filesystem path is required.");
  }
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    return Failure(Error::kIo, "Could not resolve path: " + ec.message());
  }
  *normalized = absolute.lexically_normal();
  return Success();
}

Status CanonicalExisting(const std::filesystem::path& path, std::filesystem::path* canonical) {
  if (!canonical) {
    return Failure(Error::kInvalidArgument, "A canonical path output is required.");
  }
  std::error_code ec;
  *canonical = std::filesystem::canonical(path, ec);
  if (ec) {
    return Failure(Error::kIo,
                   "Could not resolve the physical path " + PathText(path) + ": " + ec.message());
  }
  return Success();
}

Status CheckRoot(const std::filesystem::path& root, bool create_if_missing) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(root, ec);
  if (ec && !IsMissing(ec)) {
    return Failure(Error::kIo, "Could not inspect the GoldenEye user-data root: " + ec.message());
  }
  if (!ec && std::filesystem::exists(status)) {
    if (std::filesystem::is_symlink(status)) {
      return Failure(Error::kUnsafeLayout,
                     "The GoldenEye user-data root must not be a symbolic link.");
    }
    if (!std::filesystem::is_directory(status)) {
      return Failure(Error::kUnsafeLayout, "The GoldenEye user-data root is not a directory.");
    }
    return Success();
  }
  if (!create_if_missing) {
    return Success();
  }
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return Failure(Error::kIo, "Could not create the GoldenEye user-data root: " + ec.message());
  }
#if !defined(_WIN32)
  chmod(root.c_str(), 0700);
#endif
  return Success();
}

Status CheckExistingPathChain(const std::filesystem::path& root,
                              const std::filesystem::path& relative) {
  std::filesystem::path current = root;
  for (auto it = relative.begin(); it != relative.end(); ++it) {
    current /= *it;
    std::error_code ec;
    auto status = std::filesystem::symlink_status(current, ec);
    if (ec) {
      if (IsMissing(ec)) {
        return Success();
      }
      return Failure(Error::kIo, "Could not inspect managed save path " + PathText(relative) +
                                     ": " + ec.message());
    }
    if (!std::filesystem::exists(status)) {
      return Success();
    }
    if (std::filesystem::is_symlink(status)) {
      return Failure(Error::kUnsafeLayout, "Symbolic links are not allowed in managed save path " +
                                               PathText(relative) + ".");
    }
    if (std::next(it) != relative.end() && !std::filesystem::is_directory(status)) {
      return Failure(Error::kUnsafeLayout,
                     "A managed save path has a non-directory parent: " + PathText(relative) + ".");
    }
  }
  return Success();
}

bool IsValidProfileName(std::string_view name) {
  if (name.empty() || name.size() > 15 || name == "." || name == "..") {
    return false;
  }
  return name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

bool IsKnownProfileSetting(std::string_view name) {
  return std::find(kProfileSettingNames.begin(), kProfileSettingNames.end(), name) !=
         kProfileSettingNames.end();
}

EntryKind ClassifyRelativePath(const std::filesystem::path& path) {
  if (path == kSavePayload) {
    return EntryKind::kSavePayload;
  }
  if (path == kSaveThumbnail) {
    return EntryKind::kSaveThumbnail;
  }
  if (path == kSaveHeader) {
    return EntryKind::kSaveHeader;
  }

  std::vector<std::string> components;
  for (const auto& component : path) {
    components.push_back(component.string());
  }
  if (components.size() == 4 && components[0] == "584108A9" && components[1] == "profile" &&
      IsValidProfileName(components[2]) && IsKnownProfileSetting(components[3])) {
    return EntryKind::kProfileSetting;
  }
  return EntryKind::kUnknown;
}

Status ValidateContentHeader(const std::vector<uint8_t>& data) {
  if (data.size() != sizeof(XCONTENT_AGGREGATE_DATA) &&
      data.size() != sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(uint32_t)) {
    return Failure(Error::kUnsafeLayout,
                   "The GoldenEye save content header has an unexpected size.");
  }
  XCONTENT_AGGREGATE_DATA header{};
  std::memcpy(&header, data.data(), sizeof(header));
  const uint64_t header_xuid = header.xuid;
  constexpr std::string_view kExpectedFileName = "beansave.dat";
  size_t file_name_size = 0;
  while (file_name_size < sizeof(header.file_name_raw) &&
         header.file_name_raw[file_name_size] != '\0') {
    ++file_name_size;
  }
  const bool expected_file_name =
      file_name_size == kExpectedFileName.size() &&
      std::equal(kExpectedFileName.begin(), kExpectedFileName.end(), header.file_name_raw);
  if (static_cast<uint32_t>(header.device_id) != 1 ||
      header.content_type != XContentType::kSavedGame ||
      static_cast<uint32_t>(header.title_id) != kGoldenEyeTitleId || !expected_file_name ||
      (header_xuid != 0 && header_xuid != kGoldenEyeXuid &&
       header_xuid != std::numeric_limits<uint64_t>::max())) {
    return Failure(Error::kUnsafeLayout,
                   "The content header is not the supported GoldenEye save package.");
  }
  return Success();
}

Status ValidateEntryData(EntryKind kind, const std::vector<uint8_t>& data) {
  switch (kind) {
    case EntryKind::kSavePayload:
      if (data.size() != kGoldenEyeSaveBytes) {
        return Failure(Error::kUnsafeLayout,
                       "beansave.dat is not the 512-byte save format used by this build.");
      }
      return Success();
    case EntryKind::kSaveThumbnail: {
      static constexpr std::array<uint8_t, 8> kPngMagic = {0x89, 'P',  'N',  'G',
                                                           '\r', '\n', 0x1A, '\n'};
      if (data.size() < kPngMagic.size() || data.size() > kMaximumThumbnailBytes ||
          !std::equal(kPngMagic.begin(), kPngMagic.end(), data.begin())) {
        return Failure(Error::kUnsafeLayout,
                       "The GoldenEye save thumbnail is not a bounded PNG file.");
      }
      return Success();
    }
    case EntryKind::kSaveHeader:
      return ValidateContentHeader(data);
    case EntryKind::kProfileSetting:
      if (data.size() > kMaximumProfileSettingBytes) {
        return Failure(Error::kUnsafeLayout,
                       "A GoldenEye title-profile setting is unexpectedly large.");
      }
      return Success();
    case EntryKind::kUnknown:
      return Failure(Error::kUnsafeLayout, "The save layout contains an unknown managed path.");
  }
  return Failure(Error::kUnsafeLayout, "The save layout is unsupported.");
}

uint64_t MaximumBytesFor(EntryKind kind) {
  switch (kind) {
    case EntryKind::kSavePayload:
      return kGoldenEyeSaveBytes;
    case EntryKind::kSaveThumbnail:
      return kMaximumThumbnailBytes;
    case EntryKind::kSaveHeader:
      return sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(uint32_t);
    case EntryKind::kProfileSetting:
      return kMaximumProfileSettingBytes;
    case EntryKind::kUnknown:
      return 0;
  }
  return 0;
}

Status ReadStableManagedFile(const std::filesystem::path& root,
                             const std::filesystem::path& relative, Entry* entry) {
  if (!entry) {
    return Failure(Error::kInvalidArgument, "An output entry is required.");
  }
  const EntryKind kind = ClassifyRelativePath(relative);
  if (kind == EntryKind::kUnknown) {
    return Failure(Error::kUnsafeLayout,
                   "Refusing unknown GoldenEye save path " + PathText(relative) + ".");
  }
  auto chain_status = CheckExistingPathChain(root, relative);
  if (!chain_status) {
    return chain_status;
  }
  const auto path = root / relative;
  std::error_code ec;
  auto file_status = std::filesystem::symlink_status(path, ec);
  if (ec || !std::filesystem::is_regular_file(file_status) ||
      std::filesystem::is_symlink(file_status)) {
    return Failure(Error::kUnsafeLayout,
                   "Managed save entry is not a regular file: " + PathText(relative) + ".");
  }
  const uint64_t maximum = MaximumBytesFor(kind);
  const uint64_t size_before = std::filesystem::file_size(path, ec);
  if (ec || size_before > maximum || size_before > kMaximumManagedBytes) {
    return Failure(Error::kUnsafeLayout,
                   "Managed save entry has an unsafe size: " + PathText(relative) + ".");
  }
  const auto time_before = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return Failure(Error::kIo, "Could not inspect managed save entry: " + PathText(relative) + ".");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Failure(Error::kIo, "Could not read managed save entry: " + PathText(relative) + ".");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size_before));
  if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size()))) {
    return Failure(Error::kIo,
                   "Could not completely read managed save entry: " + PathText(relative) + ".");
  }
  input.close();
  const uint64_t size_after = std::filesystem::file_size(path, ec);
  if (ec) {
    return Failure(Error::kIo, "Could not recheck managed save entry: " + PathText(relative) + ".");
  }
  const auto time_after = std::filesystem::last_write_time(path, ec);
  if (ec || size_after != size_before || time_after != time_before) {
    return Failure(Error::kConflict,
                   "The save changed while it was being read. Quit the game and try again.");
  }
  auto validation = ValidateEntryData(kind, bytes);
  if (!validation) {
    return validation;
  }
  entry->relative_path = relative;
  entry->data = std::move(bytes);
  return Success();
}

Status PathStatus(const std::filesystem::path& path, std::filesystem::file_status* status,
                  bool* exists) {
  if (!status || !exists) {
    return Failure(Error::kInvalidArgument, "Path status outputs are required.");
  }
  std::error_code ec;
  *status = std::filesystem::symlink_status(path, ec);
  if (ec) {
    if (IsMissing(ec)) {
      *exists = false;
      return Success();
    }
    return Failure(Error::kIo, "Could not inspect " + PathText(path) + ": " + ec.message());
  }
  *exists = std::filesystem::exists(*status);
  return Success();
}

Status DiscoverEntries(const std::filesystem::path& normalized_root, std::vector<Entry>* entries,
                       Snapshot* snapshot) {
  if (!entries || !snapshot) {
    return Failure(Error::kInvalidArgument, "Discovery outputs are required.");
  }
  entries->clear();
  *snapshot = {};

  auto root_status = CheckRoot(normalized_root, false);
  if (!root_status) {
    return root_status;
  }
  std::filesystem::file_status root_file_status;
  bool root_exists = false;
  auto status_result = PathStatus(normalized_root, &root_file_status, &root_exists);
  if (!status_result || !root_exists) {
    return status_result;
  }

  auto package_chain = CheckExistingPathChain(normalized_root, kSavePackageDirectory);
  if (!package_chain) {
    return package_chain;
  }
  std::filesystem::file_status package_status;
  bool package_exists = false;
  status_result =
      PathStatus(normalized_root / kSavePackageDirectory, &package_status, &package_exists);
  if (!status_result) {
    return status_result;
  }
  bool payload_seen = false;
  if (package_exists) {
    if (!std::filesystem::is_directory(package_status) ||
        std::filesystem::is_symlink(package_status)) {
      return Failure(Error::kUnsafeLayout, "The GoldenEye save package is not a normal directory.");
    }
    std::error_code ec;
    for (std::filesystem::directory_iterator it(normalized_root / kSavePackageDirectory, ec), end;
         !ec && it != end; it.increment(ec)) {
      const auto item_status = it->symlink_status(ec);
      if (ec) {
        break;
      }
      const auto name = it->path().filename();
      std::filesystem::path relative;
      if (name == "beansave.dat") {
        relative = kSavePayload;
        payload_seen = true;
      } else if (name == "__thumbnail.png") {
        relative = kSaveThumbnail;
      } else {
        return Failure(Error::kUnsafeLayout,
                       "Unknown file in the GoldenEye save package: " + name.string() + ".");
      }
      if (!std::filesystem::is_regular_file(item_status) ||
          std::filesystem::is_symlink(item_status)) {
        return Failure(Error::kUnsafeLayout,
                       "GoldenEye save-package entries must be regular files.");
      }
      Entry entry;
      auto read_status = ReadStableManagedFile(normalized_root, relative, &entry);
      if (!read_status) {
        return read_status;
      }
      entries->push_back(std::move(entry));
    }
    if (ec) {
      return Failure(Error::kIo, "Could not enumerate the GoldenEye save package: " + ec.message());
    }
    if (!payload_seen) {
      return Failure(Error::kUnsafeLayout, "The GoldenEye save package is missing beansave.dat.");
    }
  }

  auto header_chain = CheckExistingPathChain(normalized_root, kSaveHeader);
  if (!header_chain) {
    return header_chain;
  }
  std::filesystem::file_status header_status;
  bool header_exists = false;
  status_result = PathStatus(normalized_root / kSaveHeader, &header_status, &header_exists);
  if (!status_result) {
    return status_result;
  }
  if (package_exists != header_exists) {
    return Failure(Error::kUnsafeLayout,
                   "The GoldenEye save package and its content header are incomplete.");
  }
  if (header_exists) {
    Entry entry;
    auto read_status = ReadStableManagedFile(normalized_root, kSaveHeader, &entry);
    if (!read_status) {
      return read_status;
    }
    entries->push_back(std::move(entry));
    snapshot->has_save_game = true;
  }

  auto profile_chain = CheckExistingPathChain(normalized_root, kProfileRoot);
  if (!profile_chain) {
    return profile_chain;
  }
  std::filesystem::file_status profile_status;
  bool profile_exists = false;
  status_result = PathStatus(normalized_root / kProfileRoot, &profile_status, &profile_exists);
  if (!status_result) {
    return status_result;
  }
  if (profile_exists) {
    if (!std::filesystem::is_directory(profile_status) ||
        std::filesystem::is_symlink(profile_status)) {
      return Failure(Error::kUnsafeLayout,
                     "The GoldenEye title-profile path is not a normal directory.");
    }
    std::error_code ec;
    for (std::filesystem::directory_iterator profile_it(normalized_root / kProfileRoot, ec), end;
         !ec && profile_it != end; profile_it.increment(ec)) {
      const auto profile_item_status = profile_it->symlink_status(ec);
      if (ec) {
        break;
      }
      const std::string profile_name = profile_it->path().filename().string();
      if (!IsValidProfileName(profile_name) ||
          !std::filesystem::is_directory(profile_item_status) ||
          std::filesystem::is_symlink(profile_item_status)) {
        return Failure(Error::kUnsafeLayout,
                       "The GoldenEye profile tree contains an unknown or unsafe profile path.");
      }
      for (std::filesystem::directory_iterator setting_it(profile_it->path(), ec), setting_end;
           !ec && setting_it != setting_end; setting_it.increment(ec)) {
        const auto setting_status = setting_it->symlink_status(ec);
        if (ec) {
          break;
        }
        const std::string setting_name = setting_it->path().filename().string();
        if (!IsKnownProfileSetting(setting_name) ||
            !std::filesystem::is_regular_file(setting_status) ||
            std::filesystem::is_symlink(setting_status)) {
          return Failure(Error::kUnsafeLayout,
                         "The GoldenEye profile tree contains an unknown title setting.");
        }
        Entry entry;
        const auto relative = kProfileRoot / profile_name / setting_name;
        auto read_status = ReadStableManagedFile(normalized_root, relative, &entry);
        if (!read_status) {
          return read_status;
        }
        entries->push_back(std::move(entry));
        snapshot->has_profile_settings = true;
      }
      if (ec) {
        break;
      }
    }
    if (ec) {
      return Failure(Error::kIo,
                     "Could not enumerate GoldenEye title-profile settings: " + ec.message());
    }
  }

  std::sort(entries->begin(), entries->end(), [](const Entry& left, const Entry& right) {
    return PathText(left.relative_path) < PathText(right.relative_path);
  });
  if (entries->size() > kMaximumEntries) {
    return Failure(Error::kUnsafeLayout, "The GoldenEye save contains too many files.");
  }
  for (const auto& entry : *entries) {
    if (snapshot->byte_count > kMaximumManagedBytes - entry.data.size()) {
      return Failure(Error::kUnsafeLayout, "The GoldenEye save exceeds the safe backup limit.");
    }
    snapshot->byte_count += entry.data.size();
  }
  snapshot->file_count = entries->size();
  return Success();
}

template <typename Integer>
void AppendLittleEndian(std::vector<uint8_t>* output, Integer value) {
  static_assert(std::is_unsigned_v<Integer>);
  for (size_t index = 0; index < sizeof(Integer); ++index) {
    output->push_back(static_cast<uint8_t>((value >> (index * 8)) & 0xFF));
  }
}

void AppendBytes(std::vector<uint8_t>* output, const void* data, size_t size) {
  if (size == 0) {
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  output->insert(output->end(), bytes, bytes + size);
}

std::string EntryHash(const std::string& path, const std::vector<uint8_t>& data) {
  std::string material;
  material.reserve(path.size() + 1 + data.size());
  material.append(path);
  material.push_back('\0');
  if (!data.empty()) {
    material.append(reinterpret_cast<const char*>(data.data()), data.size());
  }
  return rex::crypto::sha256(material);
}

std::vector<uint8_t> SerializeArchive(const std::vector<Entry>& entries,
                                      uint64_t created_unix_seconds) {
  uint64_t total_bytes = 0;
  for (const auto& entry : entries) {
    total_bytes += entry.data.size();
  }
  std::vector<uint8_t> output;
  output.reserve(static_cast<size_t>(total_bytes) + entries.size() * 128 + 48);
  AppendBytes(&output, kArchiveMagic.data(), kArchiveMagic.size());
  AppendLittleEndian<uint32_t>(&output, kArchiveVersion);
  AppendLittleEndian<uint32_t>(&output, 0);
  AppendLittleEndian<uint64_t>(&output, created_unix_seconds);
  AppendLittleEndian<uint32_t>(&output, static_cast<uint32_t>(entries.size()));
  AppendLittleEndian<uint32_t>(&output, 0);
  AppendLittleEndian<uint64_t>(&output, total_bytes);
  for (const auto& entry : entries) {
    const std::string path = PathText(entry.relative_path);
    const std::string hash = EntryHash(path, entry.data);
    AppendLittleEndian<uint16_t>(&output, static_cast<uint16_t>(path.size()));
    AppendLittleEndian<uint16_t>(&output, 0);
    AppendLittleEndian<uint32_t>(&output, 0);
    AppendLittleEndian<uint64_t>(&output, static_cast<uint64_t>(entry.data.size()));
    AppendBytes(&output, hash.data(), hash.size());
    AppendBytes(&output, path.data(), path.size());
    AppendBytes(&output, entry.data.data(), entry.data.size());
  }
  return output;
}

class Cursor {
 public:
  explicit Cursor(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  template <typename Integer>
  bool ReadLittleEndian(Integer* value) {
    static_assert(std::is_unsigned_v<Integer>);
    if (!value || bytes_.size() - offset_ < sizeof(Integer)) {
      return false;
    }
    Integer result = 0;
    for (size_t index = 0; index < sizeof(Integer); ++index) {
      result |= static_cast<Integer>(bytes_[offset_ + index]) << (index * 8);
    }
    offset_ += sizeof(Integer);
    *value = result;
    return true;
  }

  bool Read(void* output, size_t size) {
    if (size > bytes_.size() - offset_) {
      return false;
    }
    if (size != 0 && output) {
      std::memcpy(output, bytes_.data() + offset_, size);
    }
    offset_ += size;
    return true;
  }

  bool ReadString(size_t size, std::string* output) {
    if (!output || size > bytes_.size() - offset_) {
      return false;
    }
    output->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }

  bool ReadVector(size_t size, std::vector<uint8_t>* output) {
    if (!output || size > bytes_.size() - offset_) {
      return false;
    }
    output->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                   bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += size;
    return true;
  }

  bool at_end() const { return offset_ == bytes_.size(); }

 private:
  const std::vector<uint8_t>& bytes_;
  size_t offset_ = 0;
};

Status ReadBoundedFile(const std::filesystem::path& path, uint64_t maximum,
                       std::vector<uint8_t>* bytes) {
  if (!bytes) {
    return Failure(Error::kInvalidArgument, "A file output is required.");
  }
  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  if (ec || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
    return Failure(Error::kInvalidArchive, "The selected backup is not a regular file.");
  }
  const uint64_t size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > maximum ||
      size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return Failure(Error::kInvalidArchive, "The selected backup has an unsafe size.");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Failure(Error::kIo, "The selected backup could not be opened.");
  }
  bytes->resize(static_cast<size_t>(size));
  if (!input.read(reinterpret_cast<char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()))) {
    bytes->clear();
    return Failure(Error::kIo, "The selected backup could not be completely read.");
  }
  return Success();
}

Status ValidateArchiveEntries(const std::vector<Entry>& entries, Snapshot* snapshot) {
  if (!snapshot) {
    return Failure(Error::kInvalidArgument, "A snapshot output is required.");
  }
  *snapshot = {};
  bool save_payload = false;
  bool save_header = false;
  bool save_thumbnail = false;
  std::set<std::string> paths;
  for (const auto& entry : entries) {
    const std::string path = PathText(entry.relative_path);
    if (!paths.insert(path).second) {
      return Failure(Error::kInvalidArchive, "The backup contains a duplicate path.");
    }
    const EntryKind kind = ClassifyRelativePath(entry.relative_path);
    if (kind == EntryKind::kUnknown) {
      return Failure(Error::kInvalidArchive,
                     "The backup contains a path outside GoldenEye save/profile data.");
    }
    auto entry_status = ValidateEntryData(kind, entry.data);
    if (!entry_status) {
      entry_status.error = Error::kInvalidArchive;
      return entry_status;
    }
    save_payload |= kind == EntryKind::kSavePayload;
    save_header |= kind == EntryKind::kSaveHeader;
    save_thumbnail |= kind == EntryKind::kSaveThumbnail;
    snapshot->has_profile_settings |= kind == EntryKind::kProfileSetting;
    if (snapshot->byte_count > kMaximumManagedBytes - entry.data.size()) {
      return Failure(Error::kInvalidArchive, "The backup payload is too large.");
    }
    snapshot->byte_count += entry.data.size();
  }
  if (save_payload != save_header || (save_thumbnail && !save_payload)) {
    return Failure(Error::kInvalidArchive,
                   "The backup contains an incomplete GoldenEye save package.");
  }
  snapshot->has_save_game = save_payload && save_header;
  snapshot->file_count = entries.size();
  if (!*snapshot) {
    return Failure(Error::kInvalidArchive, "The backup contains no GoldenEye save data.");
  }
  return Success();
}

Status ParseArchiveBytes(const std::vector<uint8_t>& bytes, ParsedArchive* parsed) {
  if (!parsed) {
    return Failure(Error::kInvalidArgument, "An archive output is required.");
  }
  *parsed = {};
  Cursor cursor(bytes);
  std::array<uint8_t, kArchiveMagic.size()> magic{};
  uint32_t version = 0;
  uint32_t flags = 0;
  uint64_t created = 0;
  uint32_t count = 0;
  uint32_t reserved = 0;
  uint64_t declared_total = 0;
  if (!cursor.Read(magic.data(), magic.size()) || magic != kArchiveMagic ||
      !cursor.ReadLittleEndian(&version) || !cursor.ReadLittleEndian(&flags) ||
      !cursor.ReadLittleEndian(&created) || !cursor.ReadLittleEndian(&count) ||
      !cursor.ReadLittleEndian(&reserved) || !cursor.ReadLittleEndian(&declared_total)) {
    return Failure(Error::kInvalidArchive, "This is not a GoldenEye Metal save backup.");
  }
  if (version != kArchiveVersion || flags != 0 || reserved != 0 || count == 0 ||
      count > kMaximumEntries || declared_total > kMaximumManagedBytes) {
    return Failure(Error::kInvalidArchive,
                   "The GoldenEye save-backup manifest is unsupported or malformed.");
  }
  std::set<std::string> paths;
  uint64_t actual_total = 0;
  for (uint32_t index = 0; index < count; ++index) {
    uint16_t path_size = 0;
    uint16_t entry_flags = 0;
    uint32_t entry_reserved = 0;
    uint64_t data_size = 0;
    std::array<char, 64> stored_hash{};
    if (!cursor.ReadLittleEndian(&path_size) || !cursor.ReadLittleEndian(&entry_flags) ||
        !cursor.ReadLittleEndian(&entry_reserved) || !cursor.ReadLittleEndian(&data_size) ||
        !cursor.Read(stored_hash.data(), stored_hash.size()) || path_size == 0 ||
        path_size > kMaximumRelativePathBytes || entry_flags != 0 || entry_reserved != 0 ||
        data_size > kMaximumManagedBytes ||
        data_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      return Failure(Error::kInvalidArchive, "A save-backup manifest entry is malformed.");
    }
    std::string path_text;
    std::vector<uint8_t> data;
    if (!cursor.ReadString(path_size, &path_text) ||
        !cursor.ReadVector(static_cast<size_t>(data_size), &data)) {
      return Failure(Error::kInvalidArchive, "The save backup is truncated.");
    }
    const std::filesystem::path relative(path_text);
    if (relative.empty() || relative.is_absolute() || relative.has_root_path() ||
        relative.lexically_normal() != relative || PathText(relative) != path_text ||
        ClassifyRelativePath(relative) == EntryKind::kUnknown || !paths.insert(path_text).second) {
      return Failure(Error::kInvalidArchive,
                     "The backup contains a traversal, duplicate, or foreign-title path.");
    }
    const std::string expected_hash = EntryHash(path_text, data);
    if (expected_hash.size() != stored_hash.size() ||
        !std::equal(expected_hash.begin(), expected_hash.end(), stored_hash.begin())) {
      return Failure(Error::kInvalidArchive, "The save backup failed its SHA-256 integrity check.");
    }
    if (actual_total > kMaximumManagedBytes - data.size()) {
      return Failure(Error::kInvalidArchive, "The save backup payload is too large.");
    }
    actual_total += data.size();
    parsed->entries.push_back({relative, std::move(data)});
  }
  if (!cursor.at_end() || actual_total != declared_total) {
    return Failure(Error::kInvalidArchive,
                   "The save-backup manifest length does not match its payload.");
  }
  Snapshot snapshot;
  auto validation = ValidateArchiveEntries(parsed->entries, &snapshot);
  if (!validation) {
    return validation;
  }
  static_cast<Snapshot&>(parsed->info) = snapshot;
  parsed->info.format_version = version;
  parsed->info.created_unix_seconds = created;
  return Success();
}

std::string UniqueToken(std::string_view prefix) {
  static std::atomic<uint64_t> sequence{0};
  const auto wall = std::chrono::system_clock::now().time_since_epoch();
  const uint64_t microseconds =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(wall).count());
  std::ostringstream stream;
  stream << prefix << '-' << microseconds << '-' << std::hex
         << sequence.fetch_add(1, std::memory_order_relaxed);
  return stream.str();
}

Status WriteFileDurably(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
#if !defined(_WIN32)
  int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return Failure(Error::kIo, "Could not create temporary save file.");
  }
  size_t completed = 0;
  while (completed < bytes.size()) {
    const ssize_t written = write(descriptor, bytes.data() + completed, bytes.size() - completed);
    if (written <= 0) {
      close(descriptor);
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      return Failure(Error::kIo, "Could not write temporary save file.");
    }
    completed += static_cast<size_t>(written);
  }
  const bool synced = fsync(descriptor) == 0;
  const bool closed = close(descriptor) == 0;
  if (!synced || !closed) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return Failure(Error::kIo, "Could not finish writing temporary save file.");
  }
#else
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || (!bytes.empty() &&
                  !output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
    return Failure(Error::kIo, "Could not write temporary save file.");
  }
  output.close();
  if (!output) {
    return Failure(Error::kIo, "Could not finish writing temporary save file.");
  }
#endif
  return Success();
}

Status SyncDirectory(const std::filesystem::path& directory) {
#if !defined(_WIN32)
  int flags = O_RDONLY | O_CLOEXEC;
#if defined(O_DIRECTORY)
  flags |= O_DIRECTORY;
#endif
  const int descriptor = open(directory.c_str(), flags);
  if (descriptor < 0) {
    return Failure(Error::kIo,
                   "Could not open the save destination directory for synchronization.");
  }
  const bool synced = fsync(descriptor) == 0;
  const bool closed = close(descriptor) == 0;
  if (!synced || !closed) {
    return Failure(Error::kIo, "Could not synchronize the save destination directory.");
  }
#else
  (void)directory;
#endif
  return Success();
}

#if defined(GOLDENEYE_SAVE_MANAGER_TESTING)
std::atomic<int> g_crash_after_checkpoint{-1};

void MaybeCrashAfterCheckpoint() {
  const int previous = g_crash_after_checkpoint.fetch_sub(1, std::memory_order_relaxed);
  if (previous == 0) {
#if !defined(_WIN32)
    _exit(86);
#else
    std::abort();
#endif
  }
  if (previous < 0) {
    g_crash_after_checkpoint.store(-1, std::memory_order_relaxed);
  }
}
#else
void MaybeCrashAfterCheckpoint() {}
#endif

class RootOperationLock {
 public:
  RootOperationLock() = default;
  RootOperationLock(const RootOperationLock&) = delete;
  RootOperationLock& operator=(const RootOperationLock&) = delete;

  ~RootOperationLock() {
#if !defined(_WIN32)
    if (descriptor_ >= 0) {
      flock(descriptor_, LOCK_UN);
      close(descriptor_);
    }
#endif
  }

  Status Acquire(const std::filesystem::path& root) {
    process_lock_ = std::unique_lock<std::mutex>(ProcessMutex());
#if !defined(_WIN32)
    int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    descriptor_ = open((root / kOperationLock).c_str(), flags, 0600);
    if (descriptor_ < 0) {
      return Failure(Error::kUnsafeLayout,
                     "Could not safely open the GoldenEye save-operation lock.");
    }
    struct stat info{};
    if (fstat(descriptor_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
      return Failure(Error::kUnsafeLayout,
                     "The GoldenEye save-operation lock is not a private regular file.");
    }
    if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      return Failure(errno == EWOULDBLOCK ? Error::kConflict : Error::kIo,
                     errno == EWOULDBLOCK
                         ? "Another GoldenEye instance is managing save data. Try again shortly."
                         : "Could not lock GoldenEye save data for this operation.");
    }
#else
    (void)root;
#endif
    return Success();
  }

 private:
  static std::mutex& ProcessMutex() {
    static std::mutex mutex;
    return mutex;
  }

  std::unique_lock<std::mutex> process_lock_;
#if !defined(_WIN32)
  int descriptor_ = -1;
#endif
};

Status CreateRelativeDirectoriesDurably(const std::filesystem::path& root,
                                        const std::filesystem::path& relative_directory) {
  std::filesystem::path current = root;
  for (const auto& component : relative_directory) {
    current /= component;
    std::error_code ec;
    auto status = std::filesystem::symlink_status(current, ec);
    if (!ec && std::filesystem::exists(status)) {
      if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
        return Failure(Error::kUnsafeLayout,
                       "A save transaction parent is not a normal directory.");
      }
      continue;
    }
    if (ec && !IsMissing(ec)) {
      return Failure(Error::kIo, "Could not inspect a save transaction parent: " + ec.message());
    }
    ec.clear();
    if (!std::filesystem::create_directory(current, ec) || ec) {
      return Failure(Error::kIo, "Could not create a save transaction directory: " + ec.message());
    }
#if !defined(_WIN32)
    chmod(current.c_str(), 0700);
#endif
    auto sync_status = SyncDirectory(current.parent_path());
    if (!sync_status) {
      return sync_status;
    }
  }
  return Success();
}

struct DurableRenameResult {
  Status status;
  bool renamed = false;
};

DurableRenameResult RenameUnitNoReplaceDurably(const std::filesystem::path& source_root,
                                               const std::filesystem::path& destination_root,
                                               const std::filesystem::path& unit) {
  auto source_chain = CheckExistingPathChain(source_root, unit);
  if (!source_chain) {
    return {source_chain, false};
  }
  auto destination_chain = CheckExistingPathChain(destination_root, unit);
  if (!destination_chain) {
    return {destination_chain, false};
  }
  auto directory_status = CreateRelativeDirectoriesDurably(destination_root, unit.parent_path());
  if (!directory_status) {
    return {directory_status, false};
  }

  const auto source = source_root / unit;
  const auto destination = destination_root / unit;
  std::error_code ec;
  auto source_status = std::filesystem::symlink_status(source, ec);
  if (ec || !std::filesystem::exists(source_status) || std::filesystem::is_symlink(source_status)) {
    return {Failure(Error::kUnsafeLayout,
                    "A transaction source unit is missing or unsafe: " + PathText(unit) + "."),
            false};
  }
  auto destination_status = std::filesystem::symlink_status(destination, ec);
  if (!ec && std::filesystem::exists(destination_status)) {
    return {Failure(Error::kConflict,
                    "A transaction destination already exists: " + PathText(unit) + "."),
            false};
  }
  if (ec && !IsMissing(ec)) {
    return {Failure(Error::kIo, "Could not inspect a transaction destination: " + ec.message()),
            false};
  }

  bool renamed = false;
#if defined(__APPLE__)
  renamed = renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0;
  if (!renamed) {
    ec = std::error_code(errno, std::generic_category());
  }
#else
  ec.clear();
  std::filesystem::rename(source, destination, ec);
  renamed = !ec;
#endif
  if (!renamed) {
    return {Failure(ec == std::errc::file_exists ? Error::kConflict : Error::kIo,
                    "Could not move a save transaction unit without replacement: " + ec.message()),
            false};
  }
  auto source_sync = SyncDirectory(source.parent_path());
  auto destination_sync = SyncDirectory(destination.parent_path());
  if (!source_sync || !destination_sync) {
    return {Failure(Error::kIo,
                    "A save unit moved, but its directory state could not be synchronized."),
            true};
  }
  MaybeCrashAfterCheckpoint();
  return {Success(), true};
}

Status AtomicWriteNew(const std::filesystem::path& destination, const std::vector<uint8_t>& bytes) {
  if (destination.empty() || destination.filename().empty()) {
    return Failure(Error::kInvalidArgument, "A destination filename is required.");
  }
  std::error_code ec;
  const auto parent = destination.parent_path();
  auto parent_status = std::filesystem::symlink_status(parent, ec);
  if (ec || !std::filesystem::is_directory(parent_status) ||
      std::filesystem::is_symlink(parent_status)) {
    return Failure(Error::kIo, "The destination folder is not a normal directory.");
  }
  auto destination_status = std::filesystem::symlink_status(destination, ec);
  if (!ec && std::filesystem::exists(destination_status)) {
    return Failure(Error::kConflict, "The destination already exists.");
  }
  if (ec && !IsMissing(ec)) {
    return Failure(Error::kIo, "Could not inspect the destination: " + ec.message());
  }

  std::filesystem::path temporary;
  Status write_status;
  bool created = false;
  for (int attempt = 0; attempt < 32 && !created; ++attempt) {
    temporary = parent / ("." + destination.filename().string() + "." + UniqueToken("writing"));
    write_status = WriteFileDurably(temporary, bytes);
    created = static_cast<bool>(write_status);
  }
  if (!created) {
    return write_status ? Failure(Error::kIo, "Could not create a unique temporary file.")
                        : write_status;
  }

  bool renamed = false;
#if defined(__APPLE__)
  renamed = renamex_np(temporary.c_str(), destination.c_str(), RENAME_EXCL) == 0;
#else
  auto final_status = std::filesystem::symlink_status(destination, ec);
  if ((ec && IsMissing(ec)) || (!ec && !std::filesystem::exists(final_status))) {
    ec.clear();
    std::filesystem::rename(temporary, destination, ec);
    renamed = !ec;
  }
#endif
  if (!renamed) {
    std::filesystem::remove(temporary, ec);
    auto existing = std::filesystem::symlink_status(destination, ec);
    if (!ec && std::filesystem::exists(existing)) {
      return Failure(Error::kConflict, "The destination was created by another operation.");
    }
    return Failure(Error::kIo, "Could not publish the save file atomically.");
  }
  return SyncDirectory(parent);
}

Status AtomicReplaceDurably(const std::filesystem::path& destination,
                            const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  auto existing = std::filesystem::symlink_status(destination, ec);
  if (ec || !std::filesystem::is_regular_file(existing) || std::filesystem::is_symlink(existing)) {
    return Failure(Error::kUnsafeLayout, "The save transaction journal is missing or unsafe.");
  }
  const auto parent = destination.parent_path();
  const auto temporary =
      parent / ("." + destination.filename().string() + "." + UniqueToken("replacing"));
  auto write_status = WriteFileDurably(temporary, bytes);
  if (!write_status) {
    return write_status;
  }
  ec.clear();
  std::filesystem::rename(temporary, destination, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Failure(Error::kIo,
                   "Could not atomically update the save transaction journal: " + ec.message());
  }
  return SyncDirectory(parent);
}

std::vector<std::filesystem::path> UnitsForSnapshot(const Snapshot& snapshot) {
  std::vector<std::filesystem::path> units;
  if (snapshot.has_save_game) {
    units.push_back(kSavePackageDirectory);
    units.push_back(kSaveHeader);
  }
  if (snapshot.has_profile_settings) {
    units.push_back(kProfileRoot);
  }
  return units;
}

bool SameUnits(std::vector<std::filesystem::path> left, std::vector<std::filesystem::path> right) {
  auto compare = [](const auto& a, const auto& b) { return PathText(a) < PathText(b); };
  std::sort(left.begin(), left.end(), compare);
  std::sort(right.begin(), right.end(), compare);
  return left == right;
}

bool SameSnapshot(const Snapshot& left, const Snapshot& right) {
  return left.has_save_game == right.has_save_game &&
         left.has_profile_settings == right.has_profile_settings &&
         left.file_count == right.file_count && left.byte_count == right.byte_count;
}

bool SameEntries(const std::vector<Entry>& left, const std::vector<Entry>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  std::vector<const Entry*> sorted_left;
  std::vector<const Entry*> sorted_right;
  sorted_left.reserve(left.size());
  sorted_right.reserve(right.size());
  for (const auto& entry : left) {
    sorted_left.push_back(&entry);
  }
  for (const auto& entry : right) {
    sorted_right.push_back(&entry);
  }
  const auto compare = [](const Entry* a, const Entry* b) {
    return PathText(a->relative_path) < PathText(b->relative_path);
  };
  std::sort(sorted_left.begin(), sorted_left.end(), compare);
  std::sort(sorted_right.begin(), sorted_right.end(), compare);
  for (size_t index = 0; index < sorted_left.size(); ++index) {
    if (sorted_left[index]->relative_path != sorted_right[index]->relative_path ||
        sorted_left[index]->data != sorted_right[index]->data) {
      return false;
    }
  }
  return true;
}

bool IsAllowedTransactionUnits(const std::vector<std::filesystem::path>& units, bool allow_empty) {
  if (units.empty()) {
    return allow_empty;
  }
  const std::array<std::filesystem::path, 3> allowed = {kSavePackageDirectory, kSaveHeader,
                                                        kProfileRoot};
  std::set<std::string> seen;
  for (const auto& unit : units) {
    if (std::find(allowed.begin(), allowed.end(), unit) == allowed.end() ||
        !seen.insert(PathText(unit)).second) {
      return false;
    }
  }
  const bool package = std::find(units.begin(), units.end(), kSavePackageDirectory) != units.end();
  const bool header = std::find(units.begin(), units.end(), kSaveHeader) != units.end();
  return package == header;
}

bool IsSafeInternalToken(std::string_view value) {
  if (value.empty() || value.size() > 96 || value == "." || value == "..") {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '.';
  });
}

bool IsNormalizedInternalPath(const std::filesystem::path& relative) {
  return !relative.empty() && !relative.is_absolute() && !relative.has_root_path() &&
         relative.lexically_normal() == relative &&
         std::none_of(relative.begin(), relative.end(),
                      [](const auto& component) { return component == "." || component == ".."; });
}

bool IsQuarantineDirectory(const std::filesystem::path& relative) {
  return IsNormalizedInternalPath(relative) && relative.parent_path() == kQuarantineBase &&
         IsSafeInternalToken(relative.filename().string());
}

bool IsStagingDirectory(const std::filesystem::path& relative) {
  const std::string name = relative.filename().string();
  return IsNormalizedInternalPath(relative) && relative.parent_path().empty() &&
         IsSafeInternalToken(name) && name.starts_with(".GoldenEye-save-restore-");
}

std::vector<uint8_t> SerializeTransactionRecord(const TransactionRecord& record) {
  const std::string first = PathText(record.first_directory);
  const std::string second = PathText(record.second_directory);
  std::vector<uint8_t> output;
  AppendBytes(&output, kTransactionMagic.data(), kTransactionMagic.size());
  AppendLittleEndian<uint32_t>(&output, kTransactionVersion);
  AppendLittleEndian<uint32_t>(&output, static_cast<uint32_t>(record.mode));
  AppendLittleEndian<uint32_t>(&output, static_cast<uint32_t>(record.state));
  AppendLittleEndian<uint32_t>(&output, 0);
  AppendLittleEndian<uint16_t>(&output, static_cast<uint16_t>(first.size()));
  AppendLittleEndian<uint16_t>(&output, static_cast<uint16_t>(second.size()));
  AppendLittleEndian<uint16_t>(&output, static_cast<uint16_t>(record.first_units.size()));
  AppendLittleEndian<uint16_t>(&output, static_cast<uint16_t>(record.second_units.size()));
  AppendBytes(&output, first.data(), first.size());
  AppendBytes(&output, second.data(), second.size());
  for (const auto& units : {&record.first_units, &record.second_units}) {
    for (const auto& unit : *units) {
      const std::string path = PathText(unit);
      AppendLittleEndian<uint16_t>(&output, static_cast<uint16_t>(path.size()));
      AppendLittleEndian<uint16_t>(&output, 0);
      AppendBytes(&output, path.data(), path.size());
    }
  }
  return output;
}

Status ValidateTransactionRecord(const TransactionRecord& record) {
  const bool first_units_valid = IsAllowedTransactionUnits(record.first_units, false);
  const bool second_units_valid = IsAllowedTransactionUnits(record.second_units, true);
  if (!first_units_valid || !second_units_valid || record.first_directory.empty()) {
    return Failure(Error::kUnsafeLayout, "The save transaction journal has an invalid unit list.");
  }
  switch (record.mode) {
    case TransactionMode::kQuarantineToRoot:
    case TransactionMode::kUndoReset:
      if (!IsQuarantineDirectory(record.first_directory) || !record.second_directory.empty() ||
          !record.second_units.empty()) {
        return Failure(Error::kUnsafeLayout,
                       "The save transaction journal has an invalid quarantine path.");
      }
      break;
    case TransactionMode::kRestoreInstalling:
      if (!IsStagingDirectory(record.first_directory) ||
          (!record.second_directory.empty() && !IsQuarantineDirectory(record.second_directory)) ||
          (record.second_directory.empty() != record.second_units.empty())) {
        return Failure(Error::kUnsafeLayout, "The restore journal has an invalid internal path.");
      }
      break;
    case TransactionMode::kUndoDisplacing:
    case TransactionMode::kUndoRestoring:
      if (!IsQuarantineDirectory(record.first_directory) ||
          !IsQuarantineDirectory(record.second_directory) || record.second_units.empty() ||
          record.first_directory == record.second_directory) {
        return Failure(Error::kUnsafeLayout, "The undo journal has an invalid internal path.");
      }
      break;
    default:
      return Failure(Error::kUnsafeLayout,
                     "The save transaction journal has an unknown operation.");
  }
  return Success();
}

Status ParseTransactionRecord(const std::vector<uint8_t>& bytes, TransactionRecord* record) {
  if (!record) {
    return Failure(Error::kInvalidArgument, "A save transaction output is required.");
  }
  Cursor cursor(bytes);
  std::array<uint8_t, kTransactionMagic.size()> magic{};
  uint32_t version = 0;
  uint32_t mode = 0;
  uint32_t state = 0;
  uint32_t reserved = 0;
  uint16_t first_size = 0;
  uint16_t second_size = 0;
  uint16_t first_count = 0;
  uint16_t second_count = 0;
  if (!cursor.Read(magic.data(), magic.size()) || magic != kTransactionMagic ||
      !cursor.ReadLittleEndian(&version) || !cursor.ReadLittleEndian(&mode) ||
      !cursor.ReadLittleEndian(&state) || !cursor.ReadLittleEndian(&reserved) ||
      !cursor.ReadLittleEndian(&first_size) || !cursor.ReadLittleEndian(&second_size) ||
      !cursor.ReadLittleEndian(&first_count) || !cursor.ReadLittleEndian(&second_count) ||
      version != kTransactionVersion || reserved != 0 || first_size == 0 ||
      first_size > kMaximumRelativePathBytes || second_size > kMaximumRelativePathBytes ||
      first_count == 0 || first_count > 3 || second_count > 3 ||
      state < static_cast<uint32_t>(TransactionState::kPrecommit) ||
      state > static_cast<uint32_t>(TransactionState::kCommitted)) {
    return Failure(Error::kUnsafeLayout, "The save transaction journal is malformed.");
  }
  std::string first;
  std::string second;
  if (!cursor.ReadString(first_size, &first) || !cursor.ReadString(second_size, &second)) {
    return Failure(Error::kUnsafeLayout, "The save transaction journal is truncated.");
  }
  *record = {};
  record->mode = static_cast<TransactionMode>(mode);
  record->state = static_cast<TransactionState>(state);
  record->first_directory = std::filesystem::path(first);
  record->second_directory = std::filesystem::path(second);
  if (PathText(record->first_directory) != first || PathText(record->second_directory) != second) {
    return Failure(Error::kUnsafeLayout,
                   "The save transaction contains a non-canonical internal path.");
  }
  for (auto* units : {&record->first_units, &record->second_units}) {
    const uint16_t count = units == &record->first_units ? first_count : second_count;
    for (uint16_t index = 0; index < count; ++index) {
      uint16_t path_size = 0;
      uint16_t entry_reserved = 0;
      std::string path;
      if (!cursor.ReadLittleEndian(&path_size) || !cursor.ReadLittleEndian(&entry_reserved) ||
          entry_reserved != 0 || path_size == 0 || path_size > kMaximumRelativePathBytes ||
          !cursor.ReadString(path_size, &path)) {
        return Failure(Error::kUnsafeLayout, "The save transaction unit list is truncated.");
      }
      const std::filesystem::path unit(path);
      if (unit.is_absolute() || unit.lexically_normal() != unit || PathText(unit) != path) {
        return Failure(Error::kUnsafeLayout, "The save transaction contains a traversal path.");
      }
      units->push_back(unit);
    }
  }
  if (!cursor.at_end()) {
    return Failure(Error::kUnsafeLayout, "The save transaction journal has trailing data.");
  }
  return ValidateTransactionRecord(*record);
}

std::vector<uint8_t> SerializeQuarantineRecord(const QuarantineRecord& record) {
  std::vector<uint8_t> output;
  AppendBytes(&output, kQuarantineMagic.data(), kQuarantineMagic.size());
  AppendLittleEndian<uint32_t>(&output, kQuarantineVersion);
  AppendLittleEndian<uint32_t>(&output, static_cast<uint32_t>(record.kind));
  AppendLittleEndian<uint32_t>(&output, static_cast<uint32_t>(record.units.size()));
  AppendLittleEndian<uint32_t>(&output, 0);
  for (const auto& unit : record.units) {
    const std::string path = PathText(unit);
    AppendLittleEndian<uint16_t>(&output, static_cast<uint16_t>(path.size()));
    AppendLittleEndian<uint16_t>(&output, 0);
    AppendBytes(&output, path.data(), path.size());
  }
  return output;
}

Status ParseQuarantineRecord(const std::filesystem::path& quarantine, QuarantineRecord* record) {
  if (!record) {
    return Failure(Error::kInvalidArgument, "A quarantine output is required.");
  }
  std::vector<uint8_t> bytes;
  auto read_status = ReadBoundedFile(quarantine / kQuarantineManifest, 4096, &bytes);
  if (!read_status) {
    return Failure(Error::kUnsafeLayout, "The save quarantine has no valid manifest.");
  }
  Cursor cursor(bytes);
  std::array<uint8_t, kQuarantineMagic.size()> magic{};
  uint32_t version = 0;
  uint32_t kind = 0;
  uint32_t count = 0;
  uint32_t reserved = 0;
  if (!cursor.Read(magic.data(), magic.size()) || magic != kQuarantineMagic ||
      !cursor.ReadLittleEndian(&version) || !cursor.ReadLittleEndian(&kind) ||
      !cursor.ReadLittleEndian(&count) || !cursor.ReadLittleEndian(&reserved) ||
      version != kQuarantineVersion || reserved != 0 || count == 0 || count > 3 ||
      (kind != static_cast<uint32_t>(QuarantineKind::kReset) &&
       kind != static_cast<uint32_t>(QuarantineKind::kRestore))) {
    return Failure(Error::kUnsafeLayout, "The save quarantine manifest is malformed.");
  }
  std::set<std::string> seen;
  record->kind = static_cast<QuarantineKind>(kind);
  record->units.clear();
  const std::array<std::filesystem::path, 3> allowed = {kSavePackageDirectory, kSaveHeader,
                                                        kProfileRoot};
  for (uint32_t index = 0; index < count; ++index) {
    uint16_t path_size = 0;
    uint16_t entry_reserved = 0;
    std::string path_text;
    if (!cursor.ReadLittleEndian(&path_size) || !cursor.ReadLittleEndian(&entry_reserved) ||
        entry_reserved != 0 || path_size == 0 || path_size > kMaximumRelativePathBytes ||
        !cursor.ReadString(path_size, &path_text)) {
      return Failure(Error::kUnsafeLayout, "The save quarantine manifest is truncated.");
    }
    const std::filesystem::path path(path_text);
    if (PathText(path) != path_text ||
        std::find(allowed.begin(), allowed.end(), path) == allowed.end() ||
        !seen.insert(path_text).second) {
      return Failure(Error::kUnsafeLayout,
                     "The save quarantine manifest contains an unknown path.");
    }
    record->units.push_back(path);
  }
  if (!cursor.at_end()) {
    return Failure(Error::kUnsafeLayout, "The save quarantine manifest has trailing data.");
  }
  const bool package = std::find(record->units.begin(), record->units.end(),
                                 kSavePackageDirectory) != record->units.end();
  const bool header =
      std::find(record->units.begin(), record->units.end(), kSaveHeader) != record->units.end();
  if (package != header) {
    return Failure(Error::kUnsafeLayout,
                   "The save quarantine manifest contains an incomplete package.");
  }
  return Success();
}

bool IsPathPrefix(const std::filesystem::path& prefix, const std::filesystem::path& path) {
  return IsPathInside(path, prefix);
}

Status ValidateQuarantineTree(const std::filesystem::path& quarantine,
                              const QuarantineRecord& record) {
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(quarantine, ec), end; !ec && it != end;
       it.increment(ec)) {
    const auto status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (std::filesystem::is_symlink(status)) {
      return Failure(Error::kUnsafeLayout, "A save quarantine contains a symbolic link.");
    }
    const auto relative = it->path().lexically_relative(quarantine);
    if (relative == kQuarantineManifest) {
      if (!std::filesystem::is_regular_file(status)) {
        return Failure(Error::kUnsafeLayout, "The save quarantine manifest is not a file.");
      }
      continue;
    }
    bool allowed = false;
    for (const auto& unit : record.units) {
      if (IsPathPrefix(relative, unit) || IsPathPrefix(unit, relative)) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      return Failure(Error::kUnsafeLayout, "The save quarantine contains an unknown path.");
    }
  }
  if (ec) {
    return Failure(Error::kIo, "Could not validate the save quarantine: " + ec.message());
  }
  return Success();
}

Status ValidateRecordedDirectory(const std::filesystem::path& root,
                                 const std::filesystem::path& relative) {
  if (!IsNormalizedInternalPath(relative)) {
    return Failure(Error::kUnsafeLayout, "A save transaction directory path is not canonical.");
  }
  auto chain_status = CheckExistingPathChain(root, relative);
  if (!chain_status) {
    return chain_status;
  }
  std::filesystem::file_status status;
  bool exists = false;
  auto path_status = PathStatus(root / relative, &status, &exists);
  if (!path_status) {
    return path_status;
  }
  if (!exists || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
    return Failure(Error::kUnsafeLayout, "A save transaction directory is missing or unsafe.");
  }
  return Success();
}

Status ValidateRecordedDirectories(const std::filesystem::path& root,
                                   const TransactionRecord& record) {
  auto first_status = ValidateRecordedDirectory(root, record.first_directory);
  if (!first_status) {
    return first_status;
  }
  if (!record.second_directory.empty()) {
    return ValidateRecordedDirectory(root, record.second_directory);
  }
  return Success();
}

Status BeginTransaction(const std::filesystem::path& root, const TransactionRecord& record) {
  auto validation = ValidateTransactionRecord(record);
  if (!validation) {
    return validation;
  }
  validation = ValidateRecordedDirectories(root, record);
  if (!validation) {
    return validation;
  }
  auto status = AtomicWriteNew(root / kTransactionJournal, SerializeTransactionRecord(record));
  if (status) {
    MaybeCrashAfterCheckpoint();
  }
  return status;
}

Status UpdateTransaction(const std::filesystem::path& root, const TransactionRecord& record) {
  auto validation = ValidateTransactionRecord(record);
  if (!validation) {
    return validation;
  }
  validation = ValidateRecordedDirectories(root, record);
  if (!validation) {
    return validation;
  }
  auto status =
      AtomicReplaceDurably(root / kTransactionJournal, SerializeTransactionRecord(record));
  if (status) {
    MaybeCrashAfterCheckpoint();
  }
  return status;
}

Status CommitTransaction(const std::filesystem::path& root, TransactionRecord* record) {
  if (!record) {
    return Failure(Error::kInvalidArgument, "A save transaction record is required.");
  }
  record->state = TransactionState::kCommitted;
  return UpdateTransaction(root, *record);
}

Status ClearTransaction(const std::filesystem::path& root) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(root / kTransactionJournal, ec);
  if (ec) {
    if (IsMissing(ec)) {
      return Success();
    }
    return Failure(Error::kIo, "Could not inspect the save transaction journal: " + ec.message());
  }
  if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
    return Failure(Error::kUnsafeLayout, "The save transaction journal is not a regular file.");
  }
  if (!std::filesystem::remove(root / kTransactionJournal, ec) || ec) {
    return Failure(Error::kIo, "Could not remove the completed save transaction journal.");
  }
  auto sync_status = SyncDirectory(root);
  if (sync_status) {
    MaybeCrashAfterCheckpoint();
  }
  return sync_status;
}

Status ReadTransaction(const std::filesystem::path& root, TransactionRecord* record, bool* exists) {
  if (!record || !exists) {
    return Failure(Error::kInvalidArgument, "Save transaction outputs are required.");
  }
  *exists = false;
  std::error_code ec;
  auto status = std::filesystem::symlink_status(root / kTransactionJournal, ec);
  if (ec) {
    if (IsMissing(ec)) {
      return Success();
    }
    return Failure(Error::kIo, "Could not inspect the save transaction journal: " + ec.message());
  }
  if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
    return Failure(Error::kUnsafeLayout, "The save transaction journal is not a regular file.");
  }
  std::vector<uint8_t> bytes;
  auto read_status = ReadBoundedFile(root / kTransactionJournal, 4096, &bytes);
  if (!read_status) {
    return Failure(Error::kUnsafeLayout, "The save transaction journal could not be read safely.");
  }
  auto parse_status = ParseTransactionRecord(bytes, record);
  if (!parse_status) {
    return parse_status;
  }
  *exists = true;
  return Success();
}

Status UnitExistsSafely(const std::filesystem::path& directory, const std::filesystem::path& unit,
                        bool* exists) {
  if (!exists) {
    return Failure(Error::kInvalidArgument, "A save-unit presence output is required.");
  }
  auto chain_status = CheckExistingPathChain(directory, unit);
  if (!chain_status) {
    return chain_status;
  }
  std::filesystem::file_status status;
  return PathStatus(directory / unit, &status, exists);
}

Status ReconcileUnits(const std::filesystem::path& source, const std::filesystem::path& destination,
                      const std::vector<std::filesystem::path>& units) {
  for (const auto& unit : units) {
    bool at_source = false;
    bool at_destination = false;
    auto source_status = UnitExistsSafely(source, unit, &at_source);
    if (!source_status) {
      return source_status;
    }
    auto destination_status = UnitExistsSafely(destination, unit, &at_destination);
    if (!destination_status) {
      return destination_status;
    }
    if (at_source == at_destination) {
      return Failure(Error::kUnsafeLayout,
                     at_source
                         ? "Interrupted save recovery found duplicate managed data; both copies "
                           "were preserved."
                         : "Interrupted save recovery could not find every managed save unit.");
    }
    if (!at_source) {
      continue;
    }
    auto rename_result = RenameUnitNoReplaceDurably(source, destination, unit);
    if (!rename_result.status) {
      return rename_result.status;
    }
  }
  return Success();
}

Status ValidateUnitsAt(const std::filesystem::path& directory,
                       const std::vector<std::filesystem::path>& expected) {
  std::vector<Entry> entries;
  Snapshot snapshot;
  auto discovery = DiscoverEntries(directory, &entries, &snapshot);
  if (!discovery) {
    return discovery;
  }
  if (!SameUnits(UnitsForSnapshot(snapshot), expected)) {
    return Failure(Error::kUnsafeLayout,
                   "Recovered save units do not match the transaction journal.");
  }
  return Success();
}

Status ValidatePretransactionState(const std::filesystem::path& root,
                                   const TransactionRecord& record) {
  const auto first = root / record.first_directory;
  const auto second = root / record.second_directory;
  switch (record.mode) {
    case TransactionMode::kQuarantineToRoot:
      if (auto status = ValidateUnitsAt(root, record.first_units); !status) {
        return status;
      }
      return ValidateUnitsAt(first, {});
    case TransactionMode::kRestoreInstalling:
      if (auto status = ValidateUnitsAt(root, record.second_units); !status) {
        return status;
      }
      if (auto status = ValidateUnitsAt(first, record.first_units); !status) {
        return status;
      }
      return record.second_directory.empty() ? Success() : ValidateUnitsAt(second, {});
    case TransactionMode::kUndoReset:
      if (auto status = ValidateUnitsAt(root, {}); !status) {
        return status;
      }
      return ValidateUnitsAt(first, record.first_units);
    case TransactionMode::kUndoDisplacing:
    case TransactionMode::kUndoRestoring:
      if (auto status = ValidateUnitsAt(root, record.first_units); !status) {
        return status;
      }
      if (auto status = ValidateUnitsAt(first, {}); !status) {
        return status;
      }
      return ValidateUnitsAt(second, record.second_units);
  }
  return Failure(Error::kUnsafeLayout, "The save transaction operation is unsupported.");
}

Status ValidateCommittedState(const std::filesystem::path& root, const TransactionRecord& record) {
  const auto first = root / record.first_directory;
  const auto second = root / record.second_directory;
  switch (record.mode) {
    case TransactionMode::kQuarantineToRoot:
      if (auto status = ValidateUnitsAt(root, {}); !status) {
        return status;
      }
      return ValidateUnitsAt(first, record.first_units);
    case TransactionMode::kRestoreInstalling:
      if (auto status = ValidateUnitsAt(root, record.first_units); !status) {
        return status;
      }
      if (auto status = ValidateUnitsAt(first, {}); !status) {
        return status;
      }
      return record.second_directory.empty() ? Success()
                                             : ValidateUnitsAt(second, record.second_units);
    case TransactionMode::kUndoReset:
      if (auto status = ValidateUnitsAt(root, record.first_units); !status) {
        return status;
      }
      return ValidateUnitsAt(first, {});
    case TransactionMode::kUndoRestoring:
      if (auto status = ValidateUnitsAt(root, record.second_units); !status) {
        return status;
      }
      if (auto status = ValidateUnitsAt(first, record.first_units); !status) {
        return status;
      }
      return ValidateUnitsAt(second, {});
    case TransactionMode::kUndoDisplacing:
      return Failure(Error::kUnsafeLayout, "An incomplete undo transaction was marked committed.");
  }
  return Failure(Error::kUnsafeLayout, "The save transaction operation is unsupported.");
}

Status RollBackTransaction(const std::filesystem::path& root, const TransactionRecord& record) {
  const auto first = root / record.first_directory;
  const auto second = root / record.second_directory;
  Status status;
  switch (record.mode) {
    case TransactionMode::kQuarantineToRoot:
      status = ReconcileUnits(first, root, record.first_units);
      break;
    case TransactionMode::kRestoreInstalling:
      status = ReconcileUnits(root, first, record.first_units);
      if (status && !record.second_units.empty()) {
        status = ReconcileUnits(second, root, record.second_units);
      }
      break;
    case TransactionMode::kUndoReset:
      status = ReconcileUnits(root, first, record.first_units);
      break;
    case TransactionMode::kUndoDisplacing:
      status = ReconcileUnits(first, root, record.first_units);
      break;
    case TransactionMode::kUndoRestoring:
      status = ReconcileUnits(root, second, record.second_units);
      if (status) {
        status = ReconcileUnits(first, root, record.first_units);
      }
      break;
  }
  if (!status) {
    return status;
  }
  return ValidatePretransactionState(root, record);
}

RecoveryResult RecoverInterruptedTransactionUnlocked(const std::filesystem::path& root) {
  TransactionRecord record;
  bool exists = false;
  auto read_status = ReadTransaction(root, &record, &exists);
  if (!read_status || !exists) {
    return {read_status, false};
  }
  auto directory_status = ValidateRecordedDirectories(root, record);
  if (!directory_status) {
    return {directory_status, false};
  }
  const auto reconciliation = record.state == TransactionState::kCommitted
                                  ? ValidateCommittedState(root, record)
                                  : RollBackTransaction(root, record);
  if (!reconciliation) {
    return {Failure(Error::kUnsafeLayout, "Interrupted GoldenEye save recovery stopped safely: " +
                                              reconciliation.message),
            false};
  }
  auto clear_status = ClearTransaction(root);
  if (!clear_status) {
    return {clear_status, false};
  }
  return {Success(), true};
}

Status FinishTransaction(const std::filesystem::path& root) {
  TransactionRecord record;
  bool exists = false;
  auto read_status = ReadTransaction(root, &record, &exists);
  if (!read_status || !exists) {
    return read_status ? Failure(Error::kIo, "The active save transaction journal disappeared.")
                       : read_status;
  }
  auto commit_status = CommitTransaction(root, &record);
  if (!commit_status) {
    TransactionRecord observed;
    bool observed_exists = false;
    auto observed_status = ReadTransaction(root, &observed, &observed_exists);
    if (!observed_status) {
      return Failure(Error::kIo, commit_status.message + " " + observed_status.message);
    }
    if (observed_exists && observed.state == TransactionState::kCommitted) {
      auto recovery = RecoverInterruptedTransactionUnlocked(root);
      return recovery ? Success(commit_status.message) : recovery.status;
    }
    if (!observed_exists) {
      // Re-establish the durable rollback intent before touching the already
      // moved units. Without this, an externally removed journal could make a
      // failed commit look safe to the launcher's follow-up recovery check.
      record.state = TransactionState::kPrecommit;
      auto recreate_status = BeginTransaction(root, record);
      if (!recreate_status) {
        return Failure(Error::kIo, commit_status.message + " " + recreate_status.message);
      }
    }
    auto rollback = RecoverInterruptedTransactionUnlocked(root);
    return rollback ? commit_status
                    : Failure(Error::kIo, commit_status.message + " " + rollback.status.message);
  }
  auto clear_status = ClearTransaction(root);
  if (clear_status) {
    return Success();
  }
  auto recovery = RecoverInterruptedTransactionUnlocked(root);
  return recovery ? Success(clear_status.message) : recovery.status;
}

Status MoveUnitsRollbackSafe(const std::filesystem::path& source_root,
                             const std::filesystem::path& destination_root,
                             const std::vector<std::filesystem::path>& units) {
  auto source_root_status = CheckRoot(source_root, false);
  if (!source_root_status) {
    return source_root_status;
  }
  auto destination_root_status = CheckRoot(destination_root, true);
  if (!destination_root_status) {
    return destination_root_status;
  }
  for (const auto& unit : units) {
    auto source_chain = CheckExistingPathChain(source_root, unit);
    if (!source_chain) {
      return source_chain;
    }
    auto destination_chain = CheckExistingPathChain(destination_root, unit);
    if (!destination_chain) {
      return destination_chain;
    }
  }

  std::vector<std::filesystem::path> moved;
  for (const auto& unit : units) {
    auto rename_result = RenameUnitNoReplaceDurably(source_root, destination_root, unit);
    if (rename_result.renamed) {
      moved.push_back(unit);
    }
    if (rename_result.status) {
      continue;
    }

    std::string rollback_error;
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
      auto rollback_result = RenameUnitNoReplaceDurably(destination_root, source_root, *it);
      if (!rollback_result.status && rollback_error.empty()) {
        rollback_error = rollback_result.status.message;
      }
    }
    if (!rollback_error.empty()) {
      return Failure(Error::kIo, "A save move failed and rollback also failed: " + rollback_error +
                                     ". Data remains in " + PathText(destination_root) + ".");
    }
    return Failure(Error::kIo,
                   "A save move failed and was rolled back: " + rename_result.status.message);
  }
  return Success();
}

Status CreateUniqueDirectory(const std::filesystem::path& parent, std::string_view prefix,
                             std::filesystem::path* result) {
  if (!result) {
    return Failure(Error::kInvalidArgument, "A directory output is required.");
  }
  std::error_code ec;
  const bool parent_created = std::filesystem::create_directories(parent, ec);
  if (ec) {
    return Failure(Error::kIo, "Could not create save-management directory: " + ec.message());
  }
  auto parent_status = std::filesystem::symlink_status(parent, ec);
  if (ec || !std::filesystem::is_directory(parent_status) ||
      std::filesystem::is_symlink(parent_status)) {
    return Failure(Error::kUnsafeLayout,
                   "The save-management directory must not be a symbolic link.");
  }
#if !defined(_WIN32)
  chmod(parent.c_str(), 0700);
#endif
  if (parent_created) {
    auto sync_status = SyncDirectory(parent.parent_path());
    if (!sync_status) {
      return sync_status;
    }
  }
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto candidate = parent / UniqueToken(prefix);
    ec.clear();
    if (std::filesystem::create_directory(candidate, ec)) {
#if !defined(_WIN32)
      chmod(candidate.c_str(), 0700);
#endif
      auto sync_status = SyncDirectory(parent);
      if (!sync_status) {
        return sync_status;
      }
      *result = candidate;
      return Success();
    }
    if (ec && ec != std::errc::file_exists) {
      return Failure(Error::kIo, "Could not create a save-management directory: " + ec.message());
    }
  }
  return Failure(Error::kIo, "Could not allocate a unique save-management directory.");
}

Status QuarantineCurrent(const std::filesystem::path& normalized_root, QuarantineKind kind,
                         std::filesystem::path* quarantine_path) {
  if (!quarantine_path) {
    return Failure(Error::kInvalidArgument, "A quarantine output is required.");
  }
  quarantine_path->clear();
  std::vector<Entry> entries;
  Snapshot snapshot;
  auto discovery = DiscoverEntries(normalized_root, &entries, &snapshot);
  if (!discovery) {
    return discovery;
  }
  if (!snapshot) {
    return Failure(Error::kNoData, "No GoldenEye save or title-profile data exists.");
  }
  const auto units = UnitsForSnapshot(snapshot);
  std::filesystem::path quarantine;
  auto directory_status =
      CreateUniqueDirectory(normalized_root / kQuarantineBase,
                            kind == QuarantineKind::kReset ? "reset" : "restore", &quarantine);
  if (!directory_status) {
    return directory_status;
  }
  const QuarantineRecord record{kind, units};
  auto manifest_status =
      AtomicWriteNew(quarantine / kQuarantineManifest, SerializeQuarantineRecord(record));
  if (!manifest_status) {
    std::error_code ignored;
    std::filesystem::remove_all(quarantine, ignored);
    return manifest_status;
  }
  TransactionRecord transaction;
  transaction.mode = TransactionMode::kQuarantineToRoot;
  transaction.first_directory = quarantine.lexically_relative(normalized_root);
  transaction.first_units = units;
  auto journal_status = BeginTransaction(normalized_root, transaction);
  if (!journal_status) {
    // Atomic journal publication can succeed even if its final directory
    // synchronization reports an error. Reconcile that possible journal before
    // deleting the directory it references.
    auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
    if (rollback) {
      std::error_code ignored;
      std::filesystem::remove_all(quarantine, ignored);
      return journal_status;
    }
    return Failure(Error::kIo, journal_status.message + " " + rollback.status.message);
  }
  auto move_status = MoveUnitsRollbackSafe(normalized_root, quarantine, units);
  if (!move_status) {
    *quarantine_path = quarantine;
    auto recovery = RecoverInterruptedTransactionUnlocked(normalized_root);
    if (!recovery) {
      return Failure(Error::kIo, move_status.message + " " + recovery.status.message);
    }
    return move_status;
  }
  *quarantine_path = quarantine;
  return Success();
}

Status ValidateQuarantineLocation(const std::filesystem::path& normalized_root,
                                  const std::filesystem::path& requested,
                                  std::filesystem::path* normalized_quarantine) {
  auto normalize_status = AbsoluteNormalized(requested, normalized_quarantine);
  if (!normalize_status) {
    return normalize_status;
  }
  const auto base = (normalized_root / kQuarantineBase).lexically_normal();
  if (normalized_quarantine->parent_path() != base || normalized_quarantine->filename().empty()) {
    return Failure(Error::kUnsafeLayout, "The selected folder is not a GoldenEye save quarantine.");
  }
  auto chain_status = CheckExistingPathChain(
      normalized_root, normalized_quarantine->lexically_relative(normalized_root));
  if (!chain_status) {
    return chain_status;
  }
  std::error_code ec;
  auto status = std::filesystem::symlink_status(*normalized_quarantine, ec);
  if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
    return Failure(Error::kUnsafeLayout, "The selected save quarantine is not a normal directory.");
  }
  return Success();
}

Status RemoveEmptyQuarantine(const std::filesystem::path& quarantine) {
  std::error_code ec;
  std::vector<std::filesystem::path> directories;
  for (std::filesystem::recursive_directory_iterator it(quarantine, ec), end; !ec && it != end;
       it.increment(ec)) {
    const auto status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (it->path() == quarantine / kQuarantineManifest) {
      if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        return Failure(Error::kUnsafeLayout, "The consumed save quarantine has an unsafe marker.");
      }
    } else if (std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status)) {
      directories.push_back(it->path());
    } else {
      return Failure(Error::kUnsafeLayout,
                     "The consumed save quarantine contains unexpected data and was preserved.");
    }
  }
  if (ec) {
    return Failure(Error::kIo, "Could not inspect the consumed save quarantine: " + ec.message());
  }

  const bool marker_removed = std::filesystem::remove(quarantine / kQuarantineManifest, ec);
  if (ec || !marker_removed) {
    return Failure(Error::kIo, "Save data was restored, but its quarantine marker remains.");
  }
  std::sort(directories.begin(), directories.end(), [](const auto& left, const auto& right) {
    return std::distance(left.begin(), left.end()) > std::distance(right.begin(), right.end());
  });
  for (const auto& directory : directories) {
    ec.clear();
    const bool directory_removed = std::filesystem::remove(directory, ec);
    if (ec || !directory_removed) {
      return Failure(Error::kIo, "Save data was restored, but its empty quarantine remains.");
    }
  }
  ec.clear();
  const bool quarantine_removed = std::filesystem::remove(quarantine, ec);
  if (ec || !quarantine_removed) {
    return Failure(Error::kIo, "Save data was restored, but its empty quarantine remains.");
  }
  return Success();
}

MutationResult UndoQuarantineInternal(const std::filesystem::path& normalized_root,
                                      const std::filesystem::path& normalized_quarantine) {
  QuarantineRecord record;
  auto manifest_status = ParseQuarantineRecord(normalized_quarantine, &record);
  if (!manifest_status) {
    return {manifest_status, normalized_quarantine};
  }
  auto tree_status = ValidateQuarantineTree(normalized_quarantine, record);
  if (!tree_status) {
    return {tree_status, normalized_quarantine};
  }
  std::vector<Entry> entries;
  Snapshot snapshot;
  auto discovery = DiscoverEntries(normalized_quarantine, &entries, &snapshot);
  if (!discovery) {
    return {discovery, normalized_quarantine};
  }
  if (!snapshot || !SameUnits(record.units, UnitsForSnapshot(snapshot))) {
    return {
        Failure(Error::kUnsafeLayout, "The save quarantine does not match its versioned manifest."),
        normalized_quarantine};
  }

  if (record.kind == QuarantineKind::kReset) {
    for (const auto& unit : record.units) {
      std::error_code ec;
      auto current = std::filesystem::symlink_status(normalized_root / unit, ec);
      if ((!ec && std::filesystem::exists(current)) || (ec && !IsMissing(ec))) {
        return {
            Failure(Error::kConflict,
                    "Current GoldenEye save data exists; refusing to overwrite it during undo."),
            normalized_quarantine};
      }
    }
    TransactionRecord transaction;
    transaction.mode = TransactionMode::kUndoReset;
    transaction.first_directory = normalized_quarantine.lexically_relative(normalized_root);
    transaction.first_units = record.units;
    auto journal_status = BeginTransaction(normalized_root, transaction);
    if (!journal_status) {
      return {journal_status, normalized_quarantine};
    }
    auto move_status = MoveUnitsRollbackSafe(normalized_quarantine, normalized_root, record.units);
    if (!move_status) {
      auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
      return {rollback ? move_status
                       : Failure(Error::kIo, move_status.message + " " + rollback.status.message),
              normalized_quarantine};
    }
    auto finish_status = FinishTransaction(normalized_root);
    if (!finish_status) {
      auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
      return {rollback ? finish_status
                       : Failure(Error::kIo, finish_status.message + " " + rollback.status.message),
              normalized_quarantine};
    }
    auto cleanup_status = RemoveEmptyQuarantine(normalized_quarantine);
    return {cleanup_status ? Success() : Success(cleanup_status.message), {}};
  }

  std::vector<Entry> current_entries;
  Snapshot current_snapshot;
  auto current_discovery = DiscoverEntries(normalized_root, &current_entries, &current_snapshot);
  if (!current_discovery) {
    return {current_discovery, normalized_quarantine};
  }

  if (!current_snapshot) {
    TransactionRecord transaction;
    transaction.mode = TransactionMode::kUndoReset;
    transaction.first_directory = normalized_quarantine.lexically_relative(normalized_root);
    transaction.first_units = record.units;
    auto journal_status = BeginTransaction(normalized_root, transaction);
    if (!journal_status) {
      return {journal_status, normalized_quarantine};
    }
    auto move_status = MoveUnitsRollbackSafe(normalized_quarantine, normalized_root, record.units);
    if (!move_status) {
      auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
      return {rollback ? move_status
                       : Failure(Error::kIo, move_status.message + " " + rollback.status.message),
              normalized_quarantine};
    }
    auto finish_status = FinishTransaction(normalized_root);
    if (!finish_status) {
      auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
      return {rollback ? finish_status
                       : Failure(Error::kIo, finish_status.message + " " + rollback.status.message),
              normalized_quarantine};
    }
    auto cleanup_status = RemoveEmptyQuarantine(normalized_quarantine);
    return {cleanup_status ? Success() : Success(cleanup_status.message), {}};
  }

  std::filesystem::path displaced;
  auto directory_status =
      CreateUniqueDirectory(normalized_root / kQuarantineBase, "undo-replaced", &displaced);
  if (!directory_status) {
    return {directory_status, normalized_quarantine};
  }
  const auto current_units = UnitsForSnapshot(current_snapshot);
  const QuarantineRecord displaced_record{QuarantineKind::kRestore, current_units};
  auto marker_status =
      AtomicWriteNew(displaced / kQuarantineManifest, SerializeQuarantineRecord(displaced_record));
  if (!marker_status) {
    std::error_code ignored;
    std::filesystem::remove(displaced, ignored);
    return {marker_status, normalized_quarantine};
  }

  TransactionRecord transaction;
  transaction.mode = TransactionMode::kUndoDisplacing;
  transaction.first_directory = displaced.lexically_relative(normalized_root);
  transaction.second_directory = normalized_quarantine.lexically_relative(normalized_root);
  transaction.first_units = current_units;
  transaction.second_units = record.units;
  auto journal_status = BeginTransaction(normalized_root, transaction);
  if (!journal_status) {
    return {journal_status, normalized_quarantine};
  }
  auto displace_status = MoveUnitsRollbackSafe(normalized_root, displaced, current_units);
  if (!displace_status) {
    auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
    return {rollback ? displace_status
                     : Failure(Error::kIo, displace_status.message + " " + rollback.status.message),
            rollback ? normalized_quarantine : displaced};
  }

  std::vector<Entry> displaced_entries;
  Snapshot displaced_snapshot;
  auto displaced_validation = DiscoverEntries(displaced, &displaced_entries, &displaced_snapshot);
  if (displaced_validation && (!SameSnapshot(displaced_snapshot, current_snapshot) ||
                               !SameEntries(displaced_entries, current_entries))) {
    displaced_validation =
        Failure(Error::kIo, "The temporary save snapshot did not preserve the current data.");
  }
  if (!displaced_validation) {
    auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
    return {rollback
                ? displaced_validation
                : Failure(Error::kIo, displaced_validation.message + " " + rollback.status.message),
            rollback ? normalized_quarantine : displaced};
  }

  transaction.mode = TransactionMode::kUndoRestoring;
  auto phase_status = UpdateTransaction(normalized_root, transaction);
  if (!phase_status) {
    auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
    return {rollback ? phase_status
                     : Failure(Error::kIo, phase_status.message + " " + rollback.status.message),
            rollback ? normalized_quarantine : displaced};
  }
  auto restore_status = MoveUnitsRollbackSafe(normalized_quarantine, normalized_root, record.units);
  if (!restore_status) {
    auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
    return {rollback ? restore_status
                     : Failure(Error::kIo, restore_status.message + " " + rollback.status.message),
            rollback ? normalized_quarantine : displaced};
  }
  auto finish_status = FinishTransaction(normalized_root);
  if (!finish_status) {
    auto rollback = RecoverInterruptedTransactionUnlocked(normalized_root);
    return {rollback ? finish_status
                     : Failure(Error::kIo, finish_status.message + " " + rollback.status.message),
            rollback ? normalized_quarantine : displaced};
  }
  auto cleanup_status = RemoveEmptyQuarantine(normalized_quarantine);
  return {cleanup_status ? Success() : Success(cleanup_status.message), displaced};
}

Status WriteStagingFiles(const std::filesystem::path& staging, const std::vector<Entry>& entries,
                         const Snapshot& expected_snapshot) {
  for (const auto& entry : entries) {
    const auto destination = staging / entry.relative_path;
    auto directory_status =
        CreateRelativeDirectoriesDurably(staging, entry.relative_path.parent_path());
    if (!directory_status) {
      return directory_status;
    }
    auto write_status = AtomicWriteNew(destination, entry.data);
    if (!write_status) {
      return write_status;
    }
  }
  std::vector<Entry> staged_entries;
  Snapshot staged_snapshot;
  auto discovery = DiscoverEntries(staging, &staged_entries, &staged_snapshot);
  if (!discovery) {
    return discovery;
  }
  if (!SameSnapshot(staged_snapshot, expected_snapshot) || !SameEntries(staged_entries, entries)) {
    return Failure(Error::kIo, "Restore staging did not reproduce the validated save backup.");
  }
  return Success();
}

Status PrepareManagedRoot(const std::filesystem::path& requested, std::filesystem::path* root) {
  auto normalize_status = AbsoluteNormalized(requested, root);
  if (!normalize_status) {
    return normalize_status;
  }
  auto root_status = CheckRoot(*root, true);
  if (!root_status) {
    return root_status;
  }
  std::filesystem::path physical;
  auto canonical_status = CanonicalExisting(*root, &physical);
  if (!canonical_status) {
    return canonical_status;
  }
  *root = std::move(physical);
  return Success();
}

}  // namespace

RecoveryResult RecoverInterruptedTransaction(const std::filesystem::path& user_data_root) {
  std::filesystem::path root;
  auto root_status = PrepareManagedRoot(user_data_root, &root);
  if (!root_status) {
    return {root_status, false};
  }
  RootOperationLock lock;
  auto lock_status = lock.Acquire(root);
  if (!lock_status) {
    return {lock_status, false};
  }
  return RecoverInterruptedTransactionUnlocked(root);
}

Status Discover(const std::filesystem::path& user_data_root, Snapshot* snapshot) {
  if (!snapshot) {
    return Failure(Error::kInvalidArgument, "A snapshot output is required.");
  }
  std::filesystem::path root;
  auto root_status = PrepareManagedRoot(user_data_root, &root);
  if (!root_status) {
    return root_status;
  }
  RootOperationLock lock;
  auto lock_status = lock.Acquire(root);
  if (!lock_status) {
    return lock_status;
  }
  auto recovery = RecoverInterruptedTransactionUnlocked(root);
  if (!recovery) {
    return recovery.status;
  }
  std::vector<Entry> entries;
  return DiscoverEntries(root, &entries, snapshot);
}

Status CreateBackup(const std::filesystem::path& user_data_root,
                    const std::filesystem::path& destination, BackupInfo* info) {
  std::filesystem::path root;
  auto root_status = PrepareManagedRoot(user_data_root, &root);
  if (!root_status) {
    return root_status;
  }
  RootOperationLock lock;
  auto lock_status = lock.Acquire(root);
  if (!lock_status) {
    return lock_status;
  }
  auto recovery = RecoverInterruptedTransactionUnlocked(root);
  if (!recovery) {
    return recovery.status;
  }
  std::filesystem::path output;
  auto output_status = AbsoluteNormalized(destination, &output);
  if (!output_status) {
    return output_status;
  }
  if (IsPathInside(output, root)) {
    return Failure(Error::kInvalidArgument,
                   "Choose a backup destination outside GoldenEye Application Support.");
  }
  std::vector<Entry> entries;
  Snapshot snapshot;
  auto discovery = DiscoverEntries(root, &entries, &snapshot);
  if (!discovery) {
    return discovery;
  }
  if (!snapshot) {
    return Failure(Error::kNoData, "No GoldenEye save or title-profile data exists.");
  }
  std::filesystem::path physical_root;
  auto physical_root_status = CanonicalExisting(root, &physical_root);
  if (!physical_root_status) {
    return physical_root_status;
  }
  std::filesystem::path physical_parent;
  auto physical_parent_status = CanonicalExisting(output.parent_path(), &physical_parent);
  if (!physical_parent_status) {
    return physical_parent_status;
  }
  const auto physical_output = (physical_parent / output.filename()).lexically_normal();
  if (IsPathInside(physical_output, physical_root)) {
    return Failure(Error::kInvalidArgument,
                   "Choose a backup destination physically outside GoldenEye Application Support.");
  }
  const uint64_t created =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
  auto bytes = SerializeArchive(entries, created);
  if (bytes.size() > kMaximumArchiveBytes) {
    return Failure(Error::kUnsafeLayout, "The generated save backup is unexpectedly large.");
  }
  auto write_status = AtomicWriteNew(physical_output, bytes);
  if (!write_status) {
    return write_status;
  }
  BackupInfo verified;
  auto verification = InspectBackup(physical_output, &verified);
  if (!verification) {
    std::error_code ignored;
    std::filesystem::remove(physical_output, ignored);
    return Failure(Error::kIo, "The generated save backup failed self-verification.");
  }
  if (info) {
    *info = verified;
  }
  return Success();
}

Status InspectBackup(const std::filesystem::path& archive, BackupInfo* info) {
  std::vector<uint8_t> bytes;
  auto read_status = ReadBoundedFile(archive, kMaximumArchiveBytes, &bytes);
  if (!read_status) {
    return read_status;
  }
  ParsedArchive parsed;
  auto parse_status = ParseArchiveBytes(bytes, &parsed);
  if (!parse_status) {
    return parse_status;
  }
  if (info) {
    *info = parsed.info;
  }
  return Success();
}

MutationResult RestoreBackup(const std::filesystem::path& user_data_root,
                             const std::filesystem::path& archive) {
  std::filesystem::path root;
  auto root_status = PrepareManagedRoot(user_data_root, &root);
  if (!root_status) {
    return {root_status, {}};
  }
  RootOperationLock lock;
  auto lock_status = lock.Acquire(root);
  if (!lock_status) {
    return {lock_status, {}};
  }
  auto recovery = RecoverInterruptedTransactionUnlocked(root);
  if (!recovery) {
    return {recovery.status, {}};
  }
  std::vector<uint8_t> bytes;
  auto read_status = ReadBoundedFile(archive, kMaximumArchiveBytes, &bytes);
  if (!read_status) {
    return {read_status, {}};
  }
  ParsedArchive parsed;
  auto parse_status = ParseArchiveBytes(bytes, &parsed);
  if (!parse_status) {
    return {parse_status, {}};
  }
  std::filesystem::path staging;
  auto staging_status = CreateUniqueDirectory(root, ".GoldenEye-save-restore", &staging);
  if (!staging_status) {
    return {staging_status, {}};
  }
  auto cleanup_staging = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
  };
  auto write_status = WriteStagingFiles(staging, parsed.entries, parsed.info);
  if (!write_status) {
    cleanup_staging();
    return {write_status, {}};
  }
  const auto new_units = UnitsForSnapshot(parsed.info);

  std::filesystem::path quarantine;
  auto quarantine_status = QuarantineCurrent(root, QuarantineKind::kRestore, &quarantine);
  if (!quarantine_status && quarantine_status.error != Error::kNoData) {
    cleanup_staging();
    return {quarantine_status, quarantine};
  }
  if (quarantine_status.error == Error::kNoData) {
    quarantine.clear();
  }

  TransactionRecord transaction;
  transaction.mode = TransactionMode::kRestoreInstalling;
  transaction.first_directory = staging.lexically_relative(root);
  transaction.first_units = new_units;
  Status journal_status;
  if (!quarantine.empty()) {
    TransactionRecord quarantine_transaction;
    bool journal_exists = false;
    journal_status = ReadTransaction(root, &quarantine_transaction, &journal_exists);
    if (journal_status &&
        (!journal_exists || quarantine_transaction.mode != TransactionMode::kQuarantineToRoot)) {
      journal_status =
          Failure(Error::kIo, "The restore quarantine transaction was not recorded correctly.");
    }
    if (journal_status) {
      transaction.second_directory = quarantine.lexically_relative(root);
      transaction.second_units = quarantine_transaction.first_units;
      journal_status = UpdateTransaction(root, transaction);
    }
  } else {
    journal_status = BeginTransaction(root, transaction);
  }
  if (!journal_status) {
    auto rollback = RecoverInterruptedTransactionUnlocked(root);
    if (rollback) {
      cleanup_staging();
    }
    return {rollback ? journal_status
                     : Failure(Error::kIo, journal_status.message + " " + rollback.status.message),
            quarantine};
  }

  auto install_status = MoveUnitsRollbackSafe(staging, root, new_units);
  if (!install_status) {
    auto rollback = RecoverInterruptedTransactionUnlocked(root);
    if (rollback) {
      cleanup_staging();
    }
    return {rollback ? install_status
                     : Failure(Error::kIo, install_status.message + " " + rollback.status.message),
            rollback ? std::filesystem::path{} : quarantine};
  }
  auto finish_status = FinishTransaction(root);
  if (!finish_status) {
    return {finish_status, quarantine};
  }
  cleanup_staging();
  return {Success(), quarantine};
}

MutationResult ResetToFresh(const std::filesystem::path& user_data_root) {
  std::filesystem::path root;
  auto root_status = PrepareManagedRoot(user_data_root, &root);
  if (!root_status) {
    return {root_status, {}};
  }
  RootOperationLock lock;
  auto lock_status = lock.Acquire(root);
  if (!lock_status) {
    return {lock_status, {}};
  }
  auto recovery = RecoverInterruptedTransactionUnlocked(root);
  if (!recovery) {
    return {recovery.status, {}};
  }
  std::filesystem::path quarantine;
  auto quarantine_status = QuarantineCurrent(root, QuarantineKind::kReset, &quarantine);
  if (!quarantine_status) {
    return {quarantine_status, quarantine};
  }
  auto finish_status = FinishTransaction(root);
  return {finish_status, quarantine};
}

MutationResult UndoQuarantine(const std::filesystem::path& user_data_root,
                              const std::filesystem::path& quarantine_path) {
  std::filesystem::path root;
  auto root_status = PrepareManagedRoot(user_data_root, &root);
  if (!root_status) {
    return {root_status, {}};
  }
  RootOperationLock lock;
  auto lock_status = lock.Acquire(root);
  if (!lock_status) {
    return {lock_status, {}};
  }
  auto recovery = RecoverInterruptedTransactionUnlocked(root);
  if (!recovery) {
    return {recovery.status, {}};
  }
  std::filesystem::path quarantine;
  auto location_status = ValidateQuarantineLocation(root, quarantine_path, &quarantine);
  if (!location_status) {
    return {location_status, {}};
  }
  return UndoQuarantineInternal(root, quarantine);
}

#if defined(GOLDENEYE_SAVE_MANAGER_TESTING)
namespace testing {

void SetCrashAfterCheckpoint(int checkpoints) {
  g_crash_after_checkpoint.store(checkpoints, std::memory_order_relaxed);
}

}  // namespace testing
#endif

}  // namespace ge::save
