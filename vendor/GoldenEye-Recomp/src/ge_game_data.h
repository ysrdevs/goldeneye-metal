// GoldenEye Metal local game-data validation and import.
//
// This code never downloads game content. It only validates and imports a
// backup explicitly selected by the user into their private Application
// Support directory.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace ge::game_data {

inline constexpr uint32_t kGoldenEyeTitleId = 0x584108A9;

// SHA-256 identities of the supported executable representations. The first
// is the retail-encrypted/LZX-compressed XEX stored in the LIVE package; the
// second is the equivalent expanded developer representation. Both decode to
// the exact image used to build this recompiled executable.
inline constexpr char kPackagedXexSha256[] =
    "00b9197180cb044142dd78f2591ff721b1884f1b87aca5b566a325770054b8fa";
inline constexpr char kExpandedXexSha256[] =
    "038a3808477da4a3644d634aacac63b9c19e913b77dffcc22960fb6780f93c04";

// Identity of the supported LIVE/STFS package. Validating this before mounting
// keeps the general-purpose container parser away from malformed input and
// guarantees that the package matches the recompiled executable.
inline constexpr char kSupportedPackageSha256[] =
    "5c1e2620b635eef456335ec85a28f33f1b4e7fed79f36886130285ed2468199f";

struct ValidationResult {
  bool valid = false;
  std::string error;
};

struct Progress {
  std::string message;
  uint64_t completed = 0;
  uint64_t total = 0;
};

using ProgressCallback = std::function<void(const Progress&)>;
using CancellationCallback = std::function<bool()>;

struct ImportResult {
  std::filesystem::path game_data_root;
  std::string error;

  explicit operator bool() const { return error.empty() && !game_data_root.empty(); }
};

ValidationResult ValidateDirectory(const std::filesystem::path& root);
ValidationResult ValidateImportedDirectory(const std::filesystem::path& root);
ValidationResult ValidatePackage(const std::filesystem::path& package_path,
                                 const ProgressCallback& progress = {},
                                 const CancellationCallback& cancelled = {});

// Extracts a validated LIVE/STFS package into a normal game-data directory.
// The destination is published atomically only after the complete import has
// passed validation; failed imports leave an existing working cache untouched.
ImportResult ImportPackage(const std::filesystem::path& package_path,
                           const std::filesystem::path& destination,
                           const ProgressCallback& progress = {},
                           const CancellationCallback& cancelled = {});

}  // namespace ge::game_data
