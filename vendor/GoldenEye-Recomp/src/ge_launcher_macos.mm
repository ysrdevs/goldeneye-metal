#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "ge_launcher.h"

#include "ge_game_data.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kMaximumPackageBytes = 800ull * 1024ull * 1024ull;
constexpr uint64_t kImportFreeSpace = 1700ull * 1024ull * 1024ull;
constexpr auto kZipListingTimeout = std::chrono::seconds(30);
constexpr auto kZipExtractionTimeout = std::chrono::minutes(5);
constexpr auto kTaskStopGrace = std::chrono::seconds(1);
constexpr auto kStaleImportAge = std::chrono::hours(6);
constexpr auto kDiagnosticArchiveTimeout = std::chrono::minutes(2);
constexpr uint64_t kMaximumDiagnosticFileBytes = 32ull * 1024ull * 1024ull;
constexpr uint64_t kMaximumDiagnosticBundleBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kMaximumDiagnosticArchiveBytes =
    kMaximumDiagnosticBundleBytes + 16ull * 1024ull * 1024ull;
constexpr size_t kMaximumRuntimeLogs = 64;
constexpr size_t kMaximumCrashReports = 64;

NSString* ToNSString(const std::filesystem::path& path) {
  return [NSString stringWithUTF8String:path.string().c_str()];
}

NSString* ToNSString(std::string_view text) {
  return [[[NSString alloc] initWithBytes:text.data()
                                   length:text.size()
                                 encoding:NSUTF8StringEncoding] autorelease];
}

std::filesystem::path ApplicationSupportRoot() {
  NSArray<NSURL*>* urls =
      [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                             inDomains:NSUserDomainMask];
  NSURL* url = [urls firstObject];
  if (!url) {
    const char* home = std::getenv("HOME");
    return (home ? std::filesystem::path(home) : std::filesystem::temp_directory_path()) /
           "Library/Application Support/GoldenEye Metal";
  }
  return std::filesystem::path([[url path] fileSystemRepresentation]) / "GoldenEye Metal";
}

bool HasProcessArgument(NSString* expected) {
  NSArray<NSString*>* arguments = [[NSProcessInfo processInfo] arguments];
  NSString* prefix = [expected stringByAppendingString:@"="];
  for (NSUInteger index = 1; index < [arguments count]; ++index) {
    NSString* argument = [arguments objectAtIndex:index];
    if ([argument isEqualToString:expected] || [argument hasPrefix:prefix]) {
      return true;
    }
  }
  return false;
}

bool IsExplicitNoninteractiveLaunch() {
  const char* bypass = std::getenv("GOLDENEYE_LAUNCHER_BYPASS_UI");
  return (bypass && std::string_view(bypass) == "1") || HasProcessArgument(@"--game_data_root") ||
         HasProcessArgument(@"--headless");
}

bool HasMagic(const std::filesystem::path& path, const std::array<uint8_t, 4>& expected) {
  std::ifstream input(path, std::ios::binary);
  std::array<uint8_t, 4> magic{};
  return input.read(reinterpret_cast<char*>(magic.data()), magic.size()) && magic == expected;
}

bool IsZip(const std::filesystem::path& path) {
  return HasMagic(path, {'P', 'K', 3, 4}) || HasMagic(path, {'P', 'K', 5, 6}) ||
         HasMagic(path, {'P', 'K', 7, 8});
}

bool IsStfs(const std::filesystem::path& path) {
  return HasMagic(path, {'L', 'I', 'V', 'E'}) || HasMagic(path, {'P', 'I', 'R', 'S'}) ||
         HasMagic(path, {'C', 'O', 'N', ' '});
}

bool IsHexPackageName(std::string_view path) {
  auto slash = path.find_last_of("/\\");
  std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (name.size() != 40) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isdigit(c) || (std::tolower(c) >= 'a' && std::tolower(c) <= 'f');
  });
}

void StopAndReapTask(NSTask* task) {
  @try {
    if (![task isRunning]) {
      [task waitUntilExit];
      return;
    }
    [task terminate];
    auto deadline = std::chrono::steady_clock::now() + kTaskStopGrace;
    while ([task isRunning] && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if ([task isRunning]) {
      kill([task processIdentifier], SIGKILL);
    }
    [task waitUntilExit];
  } @catch (NSException*) {
  }
}

bool RunTaskToFile(NSString* launch_path, NSArray<NSString*>* arguments,
                   const std::filesystem::path& output_path, uint64_t maximum_bytes,
                   std::chrono::steady_clock::duration timeout, const std::atomic_bool& cancelled,
                   std::string_view failure_message, std::string* error) {
  if (cancelled.load()) {
    *error = "Import cancelled.";
    return false;
  }

  std::error_code ec;
  std::filesystem::remove(output_path, ec);
  int descriptor = open(output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                        S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    *error = "Could not create a private temporary import file.";
    return false;
  }

  NSFileHandle* output = [[NSFileHandle alloc] initWithFileDescriptor:descriptor
                                                       closeOnDealloc:YES];
  NSTask* task = [[NSTask alloc] init];
  [task setLaunchPath:launch_path];
  [task setArguments:arguments];
  [task setStandardOutput:output];
  [task setStandardError:[NSFileHandle fileHandleWithNullDevice]];

  bool launched = false;
  bool success = true;
  @try {
    [task launch];
    launched = true;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while ([task isRunning]) {
      struct stat output_stat{};
      if (cancelled.load()) {
        *error = "Import cancelled.";
        success = false;
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        *error = "The selected backup took too long to inspect or extract.";
        success = false;
        break;
      }
      if (fstat(descriptor, &output_stat) != 0 || output_stat.st_size < 0) {
        *error = "The temporary import file could not be monitored.";
        success = false;
        break;
      }
      if (static_cast<uint64_t>(output_stat.st_size) > maximum_bytes) {
        *error = "The selected backup is larger than the supported game-data build.";
        success = false;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!success) {
      StopAndReapTask(task);
    } else {
      [task waitUntilExit];
      if ([task terminationStatus] != 0) {
        *error = std::string(failure_message);
        success = false;
      }
    }

    struct stat final_stat{};
    if (success && (fstat(descriptor, &final_stat) != 0 || final_stat.st_size < 0 ||
                    static_cast<uint64_t>(final_stat.st_size) > maximum_bytes)) {
      *error = "The selected backup is larger than the supported game-data build.";
      success = false;
    }
    [output synchronizeFile];
    [output closeFile];
  } @catch (NSException* exception) {
    if (launched) {
      StopAndReapTask(task);
    }
    *error = std::string("Could not run the macOS ZIP reader: ") + [[exception reason] UTF8String];
    success = false;
    @try {
      [output closeFile];
    } @catch (NSException*) {
    }
  }

  [task release];
  [output release];
  if (!success) {
    std::filesystem::remove(output_path, ec);
  }
  return success;
}

std::filesystem::path TemporaryListingPath() {
  auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         (".GoldenEye-zip-listing-" + std::to_string(getpid()) + "-" + std::to_string(tick));
}

std::optional<std::string> FindPackageMember(const std::filesystem::path& zip_path,
                                             const std::atomic_bool& cancelled,
                                             std::string* error) {
  auto listing_path = TemporaryListingPath();
  if (!RunTaskToFile(@"/usr/bin/unzip", @[ @"-Z1", ToNSString(zip_path) ], listing_path,
                     2 * 1024 * 1024, kZipListingTimeout, cancelled,
                     "The selected backup is not a readable ZIP archive.", error)) {
    return std::nullopt;
  }

  std::ifstream listing_input(listing_path, std::ios::binary);
  std::string listing((std::istreambuf_iterator<char>(listing_input)),
                      std::istreambuf_iterator<char>());
  std::error_code ec;
  std::filesystem::remove(listing_path, ec);
  if (!listing_input.eof() && listing_input.fail()) {
    *error = "The selected backup directory could not be read.";
    return std::nullopt;
  }

  std::vector<std::string> candidates;
  std::istringstream lines(listing);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.size() <= 4096 && IsHexPackageName(line)) {
      candidates.push_back(line);
    }
  }
  if (candidates.size() != 1) {
    *error =
        candidates.empty()
            ? "This ZIP does not contain a supported Xbox LIVE package."
            : "This ZIP contains multiple Xbox packages; choose the GoldenEye package directly.";
    return std::nullopt;
  }
  return candidates.front();
}

bool ExtractPackageMember(const std::filesystem::path& zip_path, std::string_view member,
                          const std::filesystem::path& output_path,
                          const std::atomic_bool& cancelled, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(output_path.parent_path(), ec);
  if (ec) {
    *error = "Could not create the private Application Support directory.";
    return false;
  }
  NSString* member_string = ToNSString(member);
  if (!RunTaskToFile(@"/usr/bin/unzip", @[ @"-p", ToNSString(zip_path), member_string ],
                     output_path, kMaximumPackageBytes, kZipExtractionTimeout, cancelled,
                     "The Xbox package could not be extracted from this ZIP.", error)) {
    return false;
  }
  uint64_t size = std::filesystem::file_size(output_path, ec);
  if (ec || size == 0) {
    std::filesystem::remove(output_path, ec);
    *error = "The Xbox package in this ZIP was empty.";
    return false;
  }
  return true;
}

std::filesystem::path TemporaryPackagePath(const std::filesystem::path& user_root) {
  auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return user_root / (".GoldenEye-package-importing-" + std::to_string(tick));
}

void WriteRememberedRoot(const std::filesystem::path& user_root,
                         const std::filesystem::path& game_root) {
  std::error_code ec;
  std::filesystem::create_directories(user_root, ec);
  if (ec) {
    return;
  }

  auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  auto temporary =
      user_root / (".game-data-root.tmp-" + std::to_string(getpid()) + "-" + std::to_string(tick));
  int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                        S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    return;
  }

  std::string value = game_root.string();
  std::string_view remaining = value;
  bool success = true;
  while (!remaining.empty()) {
    ssize_t written = write(descriptor, remaining.data(), remaining.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      success = false;
      break;
    }
    remaining.remove_prefix(static_cast<size_t>(written));
  }
  if (success && fsync(descriptor) != 0) {
    success = false;
  }
  if (close(descriptor) != 0) {
    success = false;
  }
  if (!success) {
    std::filesystem::remove(temporary, ec);
    return;
  }

  std::filesystem::rename(temporary, user_root / "game-data-root", ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
  }
}

std::optional<std::filesystem::path> ReadRememberedRoot(const std::filesystem::path& user_root) {
  constexpr size_t kMaximumRememberedRootBytes = 32 * 1024;
  auto remembered_path = user_root / "game-data-root";
  std::error_code ec;
  auto status = std::filesystem::symlink_status(remembered_path, ec);
  if (ec || !std::filesystem::is_regular_file(status)) {
    return std::nullopt;
  }

  std::ifstream input(remembered_path, std::ios::binary);
  std::array<char, kMaximumRememberedRootBytes + 1> buffer{};
  input.read(buffer.data(), buffer.size());
  size_t size = static_cast<size_t>(input.gcount());
  if (input.bad() || size == 0 || size > kMaximumRememberedRootBytes) {
    return std::nullopt;
  }

  std::string value(buffer.data(), size);
  // Accept the newline-terminated format written by older launcher builds.
  if (!value.empty() && value.back() == '\n') {
    value.pop_back();
    if (!value.empty() && value.back() == '\r') {
      value.pop_back();
    }
  }
  if (value.empty() || value.find('\0') != std::string::npos) {
    return std::nullopt;
  }
  return std::filesystem::path(value);
}

bool HasGeneratedArtifactName(const std::filesystem::path& path, std::string_view prefix,
                              size_t numeric_fields) {
  std::string name = path.filename().string();
  if (!name.starts_with(prefix)) {
    return false;
  }
  std::string_view suffix(name.data() + prefix.size(), name.size() - prefix.size());
  for (size_t index = 0; index < numeric_fields; ++index) {
    size_t separator = suffix.find('-');
    std::string_view field = suffix.substr(0, separator);
    if (field.empty() ||
        !std::all_of(field.begin(), field.end(), [](unsigned char c) { return std::isdigit(c); })) {
      return false;
    }
    if (index + 1 == numeric_fields) {
      return separator == std::string_view::npos;
    }
    if (separator == std::string_view::npos) {
      return false;
    }
    suffix.remove_prefix(separator + 1);
  }
  return false;
}

bool IsStaleImportArtifact(const std::filesystem::path& path) {
  std::error_code ec;
  auto modified = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return false;
  }
  return std::filesystem::file_time_type::clock::now() - modified > kStaleImportAge;
}

void CleanupStaleImportArtifacts(const std::filesystem::path& user_root) {
  std::error_code ec;
  std::filesystem::directory_iterator iterator(user_root, ec);
  std::filesystem::directory_iterator end;
  while (!ec && iterator != end) {
    const auto path = iterator->path();
    if (IsStaleImportArtifact(path)) {
      if (HasGeneratedArtifactName(path, ".GoldenEye-package-importing-", 1) &&
          iterator->is_regular_file(ec)) {
        std::filesystem::remove(path, ec);
      } else if (HasGeneratedArtifactName(path, "Game Data.importing-", 2) &&
                 iterator->is_directory(ec)) {
        std::filesystem::remove_all(path, ec);
      } else if (HasGeneratedArtifactName(path, "Game Data.previous-", 2) &&
                 iterator->is_directory(ec)) {
        auto destination = user_root / "Game Data";
        if (!std::filesystem::exists(destination, ec) &&
            ge::game_data::ValidateImportedDirectory(path).valid) {
          std::filesystem::rename(path, destination, ec);
        } else {
          std::filesystem::remove_all(path, ec);
        }
      }
    }
    ec.clear();
    iterator.increment(ec);
  }
}

std::string LowerASCII(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::string EscapeJSONSlashes(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() * 2);
  for (char c : value) {
    if (c == '/') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

void ReplaceAll(std::string* value, std::string_view needle, std::string_view replacement) {
  if (needle.empty()) {
    return;
  }
  size_t position = 0;
  while ((position = value->find(needle, position)) != std::string::npos) {
    value->replace(position, needle.size(), replacement);
    position += replacement.size();
  }
}

bool IsSensitiveDiagnosticLine(std::string_view line) {
  // Runtime logs should never contain credentials, but fail closed if a future
  // subsystem writes an obvious credential-bearing line.
  if (line.size() > 16 * 1024) {
    // Modern .ips reports may be a single large JSON line. Do not discard the
    // whole crash report because a symbol happens to contain a matching word.
    return false;
  }
  std::string lower = LowerASCII(line);
  constexpr std::array<std::string_view, 9> markers = {
      "password",    "authorization:", "bearer ", "api_key",    "api key",
      "private_key", "private key",    "cookie:", "credential",
  };
  return std::any_of(markers.begin(), markers.end(), [&lower](std::string_view marker) {
    return lower.find(marker) != std::string::npos;
  });
}

void RedactJSONIdentifier(std::string* text, std::string_view key) {
  std::string quoted_key = "\"" + std::string(key) + "\"";
  size_t position = 0;
  while ((position = text->find(quoted_key, position)) != std::string::npos) {
    size_t colon = text->find(':', position + quoted_key.size());
    if (colon == std::string::npos) {
      return;
    }
    size_t opening_quote = colon + 1;
    while (opening_quote < text->size() &&
           std::isspace(static_cast<unsigned char>((*text)[opening_quote]))) {
      ++opening_quote;
    }
    if (opening_quote == text->size() || (*text)[opening_quote] != '"') {
      position += quoted_key.size();
      continue;
    }
    size_t closing_quote = opening_quote + 1;
    while ((closing_quote = text->find('"', closing_quote)) != std::string::npos) {
      size_t slash_count = 0;
      for (size_t check = closing_quote; check != 0 && (*text)[check - 1] == '\\'; --check) {
        ++slash_count;
      }
      if ((slash_count & 1u) == 0) {
        break;
      }
      ++closing_quote;
    }
    if (closing_quote == std::string::npos) {
      return;
    }
    text->replace(opening_quote + 1, closing_quote - opening_quote - 1, "<redacted>");
    position = opening_quote + sizeof("<redacted>");
  }
}

std::optional<std::string> RedactLegacyCrashIdentifierLine(std::string_view line) {
  std::string lower = LowerASCII(line);
  size_t start = lower.find_first_not_of(" \t");
  if (start == std::string::npos) {
    return std::nullopt;
  }
  constexpr std::array<std::string_view, 5> identifiers = {
      "anonymous uuid:",      "crashreporter key:", "hardware uuid:",
      "incident identifier:", "sleep/wake uuid:",
  };
  for (std::string_view identifier : identifiers) {
    if (std::string_view(lower).substr(start).starts_with(identifier)) {
      size_t colon = start + identifier.size() - 1;
      return std::string(line.substr(0, colon + 1)) + " <redacted>";
    }
  }
  return std::nullopt;
}

std::string SanitizeDiagnosticText(
    std::string text, const std::vector<std::pair<std::string, std::string>>& path_redactions) {
  for (const auto& [needle, replacement] : path_redactions) {
    ReplaceAll(&text, needle, replacement);
  }
  RedactJSONIdentifier(&text, "crashReporterKey");
  RedactJSONIdentifier(&text, "bootSessionUUID");
  RedactJSONIdentifier(&text, "deviceIdentifierForVendor");
  RedactJSONIdentifier(&text, "incident");
  RedactJSONIdentifier(&text, "incident_id");

  std::ostringstream sanitized;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::string lower = LowerASCII(line);
    if (auto redacted = RedactLegacyCrashIdentifierLine(line)) {
      sanitized << *redacted << '\n';
    } else if (IsSensitiveDiagnosticLine(line)) {
      sanitized << "[redacted potentially sensitive diagnostic line]\n";
    } else if (lower.find("[ge-online] server ") != std::string::npos ||
               lower.find("ge_username") != std::string::npos) {
      sanitized << "[redacted private online setting]\n";
    } else {
      sanitized << line << '\n';
    }
  }
  return sanitized.str();
}

bool WritePrivateTextFile(const std::filesystem::path& path, std::string_view contents,
                          std::string* error) {
  int descriptor =
      open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    *error = "Could not create a file in the private diagnostic workspace.";
    return false;
  }

  std::string_view remaining = contents;
  bool success = true;
  while (!remaining.empty()) {
    ssize_t written = write(descriptor, remaining.data(), remaining.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      success = false;
      break;
    }
    remaining.remove_prefix(static_cast<size_t>(written));
  }
  if (close(descriptor) != 0) {
    success = false;
  }
  if (!success) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    *error = "Could not write a file in the private diagnostic workspace.";
  }
  return success;
}

std::optional<std::string> ReadBoundedTextFile(const std::filesystem::path& path,
                                               uint64_t maximum_bytes) {
  int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return std::nullopt;
  }

  struct stat status{};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<uint64_t>(status.st_size) > maximum_bytes) {
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
      close(descriptor);
      return std::nullopt;
    }
    offset += static_cast<size_t>(count);
  }
  char extra = 0;
  ssize_t extra_count;
  do {
    extra_count = read(descriptor, &extra, 1);
  } while (extra_count < 0 && errno == EINTR);
  if (close(descriptor) != 0 || extra_count != 0) {
    return std::nullopt;
  }
  return contents;
}

std::filesystem::path UniqueDestination(const std::filesystem::path& directory,
                                        const std::filesystem::path& source) {
  std::filesystem::path candidate = directory / source.filename();
  std::error_code ec;
  for (unsigned suffix = 2; std::filesystem::exists(candidate, ec); ++suffix) {
    ec.clear();
    candidate = directory / (source.stem().string() + "-" + std::to_string(suffix) +
                             source.extension().string());
  }
  return candidate;
}

struct DiagnosticCopyStats {
  size_t copied = 0;
  size_t skipped = 0;
  uint64_t bytes = 0;
};

void CopyDiagnosticFiles(const std::vector<std::filesystem::path>& sources,
                         const std::filesystem::path& destination,
                         const std::vector<std::pair<std::string, std::string>>& path_redactions,
                         size_t maximum_files, uint64_t* shared_bytes, DiagnosticCopyStats* totals,
                         std::vector<std::string>* manifest) {
  std::error_code ec;
  std::filesystem::create_directories(destination, ec);
  if (ec) {
    totals->skipped += sources.size();
    return;
  }

  for (const auto& source : sources) {
    if (totals->copied >= maximum_files || *shared_bytes >= kMaximumDiagnosticBundleBytes) {
      ++totals->skipped;
      continue;
    }
    auto contents = ReadBoundedTextFile(source, kMaximumDiagnosticFileBytes);
    if (!contents || *shared_bytes + contents->size() > kMaximumDiagnosticBundleBytes) {
      ++totals->skipped;
      continue;
    }

    std::string sanitized = SanitizeDiagnosticText(std::move(*contents), path_redactions);
    if (sanitized.size() > kMaximumDiagnosticFileBytes ||
        *shared_bytes + sanitized.size() > kMaximumDiagnosticBundleBytes) {
      ++totals->skipped;
      continue;
    }
    auto target = UniqueDestination(destination, source);
    std::string error;
    if (!WritePrivateTextFile(target, sanitized, &error)) {
      ++totals->skipped;
      continue;
    }
    ++totals->copied;
    totals->bytes += sanitized.size();
    *shared_bytes += sanitized.size();
    manifest->push_back(target.filename().string());
  }
}

std::filesystem::file_time_type SafeModificationTime(const std::filesystem::path& path) {
  std::error_code ec;
  auto value = std::filesystem::last_write_time(path, ec);
  return ec ? std::filesystem::file_time_type::min() : value;
}

void SortNewestFirst(std::vector<std::filesystem::path>* paths) {
  std::sort(paths->begin(), paths->end(), [](const auto& left, const auto& right) {
    return SafeModificationTime(left) > SafeModificationTime(right);
  });
}

bool IsGoldenEyeRuntimeLog(const std::filesystem::path& path) {
  std::string name = path.filename().string();
  if (name.size() < 8 || !name.starts_with("ge_")) {
    return false;
  }
  size_t position = 3;
  size_t sequence_start = position;
  while (position < name.size() && std::isdigit(static_cast<unsigned char>(name[position]))) {
    ++position;
  }
  if (position == sequence_start) {
    return false;
  }
  std::string_view suffix(name.data() + position, name.size() - position);
  if (suffix == ".log") {
    return true;
  }
  if (suffix.size() < 6 || suffix.front() != '.') {
    return false;
  }
  suffix.remove_prefix(1);
  size_t log_extension = suffix.find(".log");
  return log_extension != std::string_view::npos && log_extension + 4 == suffix.size() &&
         log_extension != 0 &&
         std::all_of(suffix.begin(), suffix.begin() + log_extension,
                     [](unsigned char c) { return std::isdigit(c); });
}

std::vector<std::filesystem::path> FindRuntimeLogs(const std::filesystem::path& user_root) {
  std::vector<std::filesystem::path> result;
  std::error_code ec;
  auto root = user_root / "Logs";
  auto root_status = std::filesystem::symlink_status(root, ec);
  if (ec || !std::filesystem::is_directory(root_status)) {
    return result;
  }
  std::filesystem::directory_iterator iterator(root, ec);
  std::filesystem::directory_iterator end;
  while (!ec && iterator != end) {
    const auto path = iterator->path();
    auto status = iterator->symlink_status(ec);
    if (!ec && std::filesystem::is_regular_file(status) && IsGoldenEyeRuntimeLog(path)) {
      result.push_back(path);
    }
    ec.clear();
    iterator.increment(ec);
  }
  SortNewestFirst(&result);
  return result;
}

bool IsGoldenEyeCrashReport(const std::filesystem::path& path) {
  std::string name = LowerASCII(path.filename().string());
  bool matching_name = name.starts_with("goldeneye-") || name.starts_with("goldeneye metal-") ||
                       name.starts_with("goldeneye_");
  if (!matching_name) {
    return false;
  }
  std::string extension = LowerASCII(path.extension().string());
  constexpr std::array<std::string_view, 5> extensions = {
      ".ips", ".crash", ".spin", ".hang", ".diag",
  };
  return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool CrashReportIdentifiesGoldenEye(const std::filesystem::path& path) {
  auto contents = ReadBoundedTextFile(path, kMaximumDiagnosticFileBytes);
  if (!contents) {
    return false;
  }
  std::string lower = LowerASCII(*contents);
  constexpr std::array<std::string_view, 7> identifiers = {
      "io.github.ysrdevs.goldeneye-metal",
      "\"procname\":\"goldeneye\"",
      "\"procname\" : \"goldeneye\"",
      "\"process\":\"goldeneye\"",
      "process:               goldeneye [",
      "process: goldeneye [",
      "goldeneye metal.app/contents/macos/goldeneye",
  };
  return std::any_of(identifiers.begin(), identifiers.end(), [&lower](std::string_view identifier) {
    return lower.find(identifier) != std::string::npos;
  });
}

std::vector<std::filesystem::path> FindCrashReports(const std::filesystem::path& home) {
  std::vector<std::filesystem::path> result;
  if (home.empty()) {
    return result;
  }
  constexpr std::array<std::string_view, 2> relative_roots = {
      "Library/Logs/DiagnosticReports",
      "Library/Logs/CrashReporter",
  };
  for (std::string_view relative : relative_roots) {
    std::error_code ec;
    auto root = home / relative;
    auto root_status = std::filesystem::symlink_status(root, ec);
    if (ec || !std::filesystem::is_directory(root_status)) {
      continue;
    }
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    while (!ec && iterator != end) {
      const auto path = iterator->path();
      auto status = iterator->symlink_status(ec);
      if (!ec && std::filesystem::is_regular_file(status) && IsGoldenEyeCrashReport(path) &&
          CrashReportIdentifiesGoldenEye(path)) {
        result.push_back(path);
      }
      ec.clear();
      iterator.increment(ec);
    }
  }
  SortNewestFirst(&result);
  return result;
}

std::string SysctlString(const char* name) {
  size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0 || size > 64 * 1024) {
    return "Unavailable";
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return "Unavailable";
  }
  while (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value.empty() ? "Unavailable" : value;
}

uint64_t SysctlUInt64(const char* name) {
  uint64_t value = 0;
  size_t size = sizeof(value);
  return sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? value : 0;
}

std::string SmallTaskOutput(NSString* launch_path, NSArray<NSString*>* arguments) {
  NSTask* task = [[NSTask alloc] init];
  NSPipe* pipe = [[NSPipe alloc] init];
  [task setLaunchPath:launch_path];
  [task setArguments:arguments];
  [task setStandardOutput:pipe];
  [task setStandardError:[NSFileHandle fileHandleWithNullDevice]];

  NSData* data = nil;
  @try {
    [task launch];
    [task waitUntilExit];
    if ([task terminationStatus] == 0) {
      data = [[[pipe fileHandleForReading] readDataToEndOfFile] retain];
    }
  } @catch (NSException*) {
  }

  std::string output;
  if (data && [data length] <= 64 * 1024) {
    output.assign(static_cast<const char*>([data bytes]), [data length]);
  }
  [data release];
  [pipe release];
  [task release];
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  return output;
}

bool IsHexToken(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isdigit(c) || (std::tolower(c) >= 'a' && std::tolower(c) <= 'f');
  });
}

std::optional<std::string> ParseMachOUUID(std::string_view output) {
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string label;
    std::string uuid;
    if (!(fields >> label >> uuid) || label != "UUID:" || uuid.size() != 36 || uuid[8] != '-' ||
        uuid[13] != '-' || uuid[18] != '-' || uuid[23] != '-') {
      continue;
    }
    std::string compact = uuid;
    std::erase(compact, '-');
    if (IsHexToken(compact)) {
      return uuid;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ParseSHA256(std::string_view output) {
  std::istringstream fields{std::string(output)};
  std::string hash;
  if ((fields >> hash) && hash.size() == 64 && IsHexToken(hash)) {
    return hash;
  }
  return std::nullopt;
}

std::string BundleString(NSString* key, std::string_view fallback) {
  id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:key];
  if (![value isKindOfClass:[NSString class]]) {
    return std::string(fallback);
  }
  const char* utf8 = [static_cast<NSString*>(value) UTF8String];
  return utf8 ? std::string(utf8) : std::string(fallback);
}

std::string BuildSystemReport() {
  NSProcessInfo* process = [NSProcessInfo processInfo];
  std::ostringstream report;
  report << "GoldenEye Metal diagnostic system report\n"
         << "Generated: " << [[[NSDate date] descriptionWithLocale:nil] UTF8String] << "\n"
         << "macOS: " << [[process operatingSystemVersionString] UTF8String] << "\n"
         << "Hardware model: " << SysctlString("hw.model") << "\n"
         << "CPU: " << SysctlString("machdep.cpu.brand_string") << "\n"
         << "Architecture: arm64\n"
         << "Logical processors: " << [process processorCount] << "\n";
  uint64_t memory = SysctlUInt64("hw.memsize");
  if (memory != 0) {
    report << "Physical memory: " << std::fixed << std::setprecision(1)
           << static_cast<double>(memory) / (1024.0 * 1024.0 * 1024.0) << " GiB\n";
  }
  return report.str();
}

std::string BuildApplicationReport() {
  std::filesystem::path executable(
      [[[[NSBundle mainBundle] executableURL] path] fileSystemRepresentation]);
  std::error_code ec;
  uint64_t executable_size = std::filesystem::file_size(executable, ec);
  auto uuid = ParseMachOUUID(
      SmallTaskOutput(@"/usr/bin/dwarfdump", @[ @"--uuid", ToNSString(executable) ]));
  auto sha =
      ParseSHA256(SmallTaskOutput(@"/usr/bin/shasum", @[ @"-a", @"256", ToNSString(executable) ]));

  std::ostringstream report;
  report << "Application: GoldenEye Metal\n"
         << "Version: " << BundleString(@"CFBundleShortVersionString", "Unknown") << "\n"
         << "Build: " << BundleString(@"CFBundleVersion", "Unknown") << "\n"
         << "Bundle identifier: "
         << BundleString(@"CFBundleIdentifier", "io.github.ysrdevs.goldeneye-metal") << "\n";
  if (uuid) {
    report << "Executable UUID: " << *uuid << "\n";
  } else {
    report << "Executable UUID: Unavailable\n";
  }
  if (sha) {
    report << "Executable SHA-256: " << *sha << "\n";
  }
  if (!ec) {
    report << "Executable size: " << executable_size << " bytes\n";
  }
  return report.str();
}

bool RunArchiveTask(const std::filesystem::path& source, const std::filesystem::path& destination,
                    std::string* error) {
  NSTask* task = [[NSTask alloc] init];
  [task setLaunchPath:@"/usr/bin/ditto"];
  [task setArguments:@[
    @"-c", @"-k", @"--norsrc", @"--noextattr", @"--noqtn", @"--noacl", @"--keepParent",
    ToNSString(source), ToNSString(destination)
  ]];
  [task setStandardOutput:[NSFileHandle fileHandleWithNullDevice]];
  [task setStandardError:[NSFileHandle fileHandleWithNullDevice]];

  bool success = true;
  bool launched = false;
  @try {
    [task launch];
    launched = true;
    auto deadline = std::chrono::steady_clock::now() + kDiagnosticArchiveTimeout;
    while ([task isRunning] && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if ([task isRunning]) {
      *error = "Creating the diagnostic ZIP took too long.";
      success = false;
      StopAndReapTask(task);
    } else {
      [task waitUntilExit];
      if ([task terminationStatus] != 0) {
        *error = "macOS could not create the diagnostic ZIP.";
        success = false;
      }
    }
  } @catch (NSException* exception) {
    if (launched) {
      StopAndReapTask(task);
    }
    const char* reason = [[exception reason] UTF8String];
    *error = std::string("Could not start the macOS ZIP tool") +
             (reason ? std::string(": ") + reason : std::string("."));
    success = false;
  }
  [task release];
  return success;
}

bool CreatePrivateTemporaryDirectory(std::filesystem::path* result, std::string* error) {
  auto pattern_path = std::filesystem::temp_directory_path() / "GoldenEye-Metal-Diagnostics-XXXXXX";
  std::string pattern = pattern_path.string();
  std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
  mutable_pattern.push_back('\0');
  char* created = mkdtemp(mutable_pattern.data());
  if (!created) {
    *error = "Could not create a private temporary diagnostic workspace.";
    return false;
  }
  *result = created;
  chmod(result->c_str(), S_IRWXU);
  return true;
}

bool PublishPrivateFileAtomically(const std::filesystem::path& source,
                                  const std::filesystem::path& destination, std::string* error) {
  int source_descriptor = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat source_status{};
  if (source_descriptor < 0 || fstat(source_descriptor, &source_status) != 0 ||
      !S_ISREG(source_status.st_mode) || source_status.st_size < 0 ||
      static_cast<uint64_t>(source_status.st_size) > kMaximumDiagnosticArchiveBytes) {
    if (source_descriptor >= 0) {
      close(source_descriptor);
    }
    *error = "The private diagnostic ZIP could not be read safely.";
    return false;
  }

  auto destination_directory = destination.parent_path();
  if (destination_directory.empty()) {
    destination_directory = ".";
  }
  auto template_path =
      destination_directory / ("." + destination.filename().string() + ".partial-XXXXXX");
  std::string template_string = template_path.string();
  std::vector<char> mutable_template(template_string.begin(), template_string.end());
  mutable_template.push_back('\0');
  int destination_descriptor = mkstemp(mutable_template.data());
  if (destination_descriptor < 0) {
    close(source_descriptor);
    *error = "A private temporary ZIP could not be created at the selected location.";
    return false;
  }
  fcntl(destination_descriptor, F_SETFD, FD_CLOEXEC);
  std::filesystem::path temporary_destination(mutable_template.data());

  bool success = true;
  uint64_t copied = 0;
  std::array<char, 64 * 1024> buffer{};
  while (success) {
    ssize_t read_count = read(source_descriptor, buffer.data(), buffer.size());
    if (read_count < 0 && errno == EINTR) {
      continue;
    }
    if (read_count < 0) {
      success = false;
      break;
    }
    if (read_count == 0) {
      break;
    }
    copied += static_cast<uint64_t>(read_count);
    if (copied > kMaximumDiagnosticArchiveBytes) {
      success = false;
      break;
    }

    size_t offset = 0;
    while (offset < static_cast<size_t>(read_count)) {
      ssize_t write_count = write(destination_descriptor, buffer.data() + offset,
                                  static_cast<size_t>(read_count) - offset);
      if (write_count < 0 && errno == EINTR) {
        continue;
      }
      if (write_count <= 0) {
        success = false;
        break;
      }
      offset += static_cast<size_t>(write_count);
    }
  }
  if (copied != static_cast<uint64_t>(source_status.st_size)) {
    success = false;
  }
  if (success && fsync(destination_descriptor) != 0) {
    success = false;
  }
  if (close(source_descriptor) != 0) {
    success = false;
  }
  if (close(destination_descriptor) != 0) {
    success = false;
  }

  if (success && rename(temporary_destination.c_str(), destination.c_str()) != 0) {
    success = false;
  }
  if (!success) {
    unlink(temporary_destination.c_str());
    *error = "The diagnostic ZIP was created, but could not be saved at the selected location.";
  }
  return success;
}

bool ExportDiagnosticBundle(const rex::PathConfig& paths, const std::filesystem::path& destination,
                            std::string* error) {
  std::filesystem::path temporary_root;
  if (!CreatePrivateTemporaryDirectory(&temporary_root, error)) {
    return false;
  }

  std::error_code ec;
  auto cleanup = [&] {
    std::filesystem::remove_all(temporary_root, ec);
    ec.clear();
  };
  auto bundle_root = temporary_root / "GoldenEye Metal Diagnostics";
  std::filesystem::create_directory(bundle_root, ec);
  if (ec) {
    cleanup();
    *error = "Could not prepare the diagnostic bundle.";
    return false;
  }

  NSURL* home_url = [[NSFileManager defaultManager] homeDirectoryForCurrentUser];
  const char* home_value = home_url ? [[home_url path] fileSystemRepresentation] : nullptr;
  std::filesystem::path home =
      home_value ? std::filesystem::path(home_value) : std::filesystem::path();
  std::vector<std::pair<std::string, std::string>> path_redactions;
  if (!paths.game_data_root.empty()) {
    path_redactions.emplace_back(paths.game_data_root.string(), "<GAME_DATA_ROOT>");
  }
  if (!paths.user_data_root.empty()) {
    path_redactions.emplace_back(paths.user_data_root.string(), "<APP_SUPPORT>");
  }
  if (!home.empty()) {
    path_redactions.emplace_back(home.string(), "~");
  }
  size_t plain_redaction_count = path_redactions.size();
  for (size_t index = 0; index < plain_redaction_count; ++index) {
    std::string escaped = EscapeJSONSlashes(path_redactions[index].first);
    if (escaped != path_redactions[index].first) {
      path_redactions.emplace_back(std::move(escaped), path_redactions[index].second);
    }
  }
  std::sort(
      path_redactions.begin(), path_redactions.end(),
      [](const auto& left, const auto& right) { return left.first.size() > right.first.size(); });

  DiagnosticCopyStats log_stats;
  DiagnosticCopyStats crash_stats;
  uint64_t copied_diagnostic_bytes = 0;
  std::vector<std::string> log_manifest;
  std::vector<std::string> crash_manifest;
  CopyDiagnosticFiles(FindRuntimeLogs(paths.user_data_root), bundle_root / "Runtime Logs",
                      path_redactions, kMaximumRuntimeLogs, &copied_diagnostic_bytes, &log_stats,
                      &log_manifest);
  CopyDiagnosticFiles(FindCrashReports(home), bundle_root / "macOS Crash Reports", path_redactions,
                      kMaximumCrashReports, &copied_diagnostic_bytes, &crash_stats,
                      &crash_manifest);

  std::string write_error;
  if (!WritePrivateTextFile(bundle_root / "System.txt", BuildSystemReport(), &write_error) ||
      !WritePrivateTextFile(bundle_root / "Application.txt", BuildApplicationReport(),
                            &write_error)) {
    cleanup();
    *error = write_error;
    return false;
  }

  std::ostringstream readme;
  readme
      << "GoldenEye Metal Diagnostics\n\n"
      << "Send this ZIP with your bug report. It was created by the GoldenEye Metal launcher.\n\n"
      << "Included:\n"
      << "- " << log_stats.copied << " runtime log(s)\n"
      << "- " << crash_stats.copied << " matching macOS crash report(s)\n"
      << "- app build identity and non-identifying system information\n\n"
      << "Privacy:\n"
      << "- No game data, saves, Xbox package, cache, remembered paths, or settings file is "
         "included.\n"
      << "- Home, game-data, and Application Support paths are redacted from copied reports.\n"
      << "- Credential-like log lines and macOS crash-report identifiers are redacted.\n"
      << "- Hardware serial numbers and Apple Account information are not collected.\n";
  if (log_stats.skipped != 0 || crash_stats.skipped != 0) {
    readme << "\nSafety limits skipped " << log_stats.skipped << " runtime log(s) and "
           << crash_stats.skipped << " crash report(s).\n";
  }
  if (!log_manifest.empty()) {
    readme << "\nRuntime logs:\n";
    for (const auto& name : log_manifest) {
      readme << "- " << name << "\n";
    }
  }
  if (!crash_manifest.empty()) {
    readme << "\nmacOS crash reports:\n";
    for (const auto& name : crash_manifest) {
      readme << "- " << name << "\n";
    }
  }
  if (!WritePrivateTextFile(bundle_root / "README.txt", readme.str(), &write_error)) {
    cleanup();
    *error = write_error;
    return false;
  }

  auto private_archive = temporary_root / "GoldenEye-Metal-Diagnostics.zip";
  if (!RunArchiveTask(bundle_root, private_archive, error)) {
    cleanup();
    return false;
  }
  if (!PublishPrivateFileAtomically(private_archive, destination, error)) {
    cleanup();
    return false;
  }
  cleanup();
  return true;
}

}  // namespace

@interface GoldenEyeLauncherController : NSObject <NSWindowDelegate> {
 @private
  NSWindow* window_;
  NSTextField* explanation_;
  NSTextField* status_;
  NSProgressIndicator* progress_;
  NSButton* play_;
  NSButton* choose_backup_;
  NSButton* choose_folder_;
  NSButton* export_diagnostics_;
  NSButton* quit_;
  rex::PathConfig paths_;
  std::function<void(rex::PathConfig)> resume_;
  rex::ui::WindowedAppContext* app_context_;
  std::atomic_bool busy_;
  std::atomic_bool cancel_requested_;
  std::atomic_bool exporting_;
}

- (instancetype)initWithPaths:(const rex::PathConfig&)paths
                       resume:(std::function<void(rex::PathConfig)>)resume
                   appContext:(rex::ui::WindowedAppContext*)appContext;
- (void)show;
@end

namespace {
GoldenEyeLauncherController* g_launcher = nil;
}

@implementation GoldenEyeLauncherController

- (instancetype)initWithPaths:(const rex::PathConfig&)paths
                       resume:(std::function<void(rex::PathConfig)>)resume
                   appContext:(rex::ui::WindowedAppContext*)appContext {
  self = [super init];
  if (self) {
    paths_ = paths;
    resume_ = std::move(resume);
    app_context_ = appContext;
    busy_.store(false);
    cancel_requested_.store(false);
    exporting_.store(false);
  }
  return self;
}

- (void)dealloc {
  [window_ setDelegate:nil];
  [window_ release];
  [super dealloc];
}

- (NSTextField*)labelWithFrame:(NSRect)frame
                          text:(NSString*)text
                          font:(NSFont*)font
                         color:(NSColor*)color {
  NSTextField* label = [[[NSTextField alloc] initWithFrame:frame] autorelease];
  [label setStringValue:text];
  [label setEditable:NO];
  [label setSelectable:NO];
  [label setBezeled:NO];
  [label setDrawsBackground:NO];
  [label setFont:font];
  [label setTextColor:color];
  return label;
}

- (void)show {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

  NSRect frame = NSMakeRect(0, 0, 680, 560);
  NSUInteger style =
      NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
  window_ = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  [window_ setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
  [window_ setTitle:@"GoldenEye Metal"];
  [window_ setReleasedWhenClosed:NO];
  [window_ setDelegate:self];
  [window_ center];

  NSView* content = [window_ contentView];
  [content setWantsLayer:YES];
  [content layer].backgroundColor = [[NSColor colorWithCalibratedRed:0.055
                                                               green:0.06
                                                                blue:0.07
                                                               alpha:1.0] CGColor];

  NSImageView* icon = [[[NSImageView alloc] initWithFrame:NSMakeRect(46, 436, 82, 82)] autorelease];
  [icon setImage:[NSApp applicationIconImage]];
  [icon setImageScaling:NSImageScaleProportionallyUpOrDown];
  [content addSubview:icon];

  NSTextField* title = [self labelWithFrame:NSMakeRect(148, 468, 480, 42)
                                       text:@"GoldenEye Metal"
                                       font:[NSFont systemFontOfSize:30 weight:NSFontWeightBold]
                                      color:[NSColor colorWithCalibratedRed:0.88
                                                                      green:0.70
                                                                       blue:0.22
                                                                      alpha:1.0]];
  [content addSubview:title];
  NSTextField* subtitle = [self labelWithFrame:NSMakeRect(150, 437, 475, 30)
                                          text:@"Native Apple Silicon • Metal • 60 FPS target"
                                          font:[NSFont systemFontOfSize:15
                                                                 weight:NSFontWeightMedium]
                                         color:[NSColor secondaryLabelColor]];
  [content addSubview:subtitle];

  explanation_ = [self
      labelWithFrame:NSMakeRect(48, 350, 584, 66)
                text:@"Choose your local GoldenEye game backup. The app will verify it, import the "
                     @"required game data privately, and remember it for future launches."
                font:[NSFont systemFontOfSize:16]
               color:[NSColor labelColor]];
  [explanation_ setLineBreakMode:NSLineBreakByWordWrapping];
  [explanation_ setMaximumNumberOfLines:3];
  [content addSubview:explanation_];

  play_ = [[[NSButton alloc] initWithFrame:NSMakeRect(48, 288, 584, 48)] autorelease];
  [play_ setTitle:@"Play GoldenEye"];
  [play_ setButtonType:NSButtonTypeMomentaryPushIn];
  [play_ setBezelStyle:NSBezelStyleRounded];
  [play_ setTarget:self];
  [play_ setAction:@selector(play:)];
  [content addSubview:play_];

  choose_backup_ = [[[NSButton alloc] initWithFrame:NSMakeRect(48, 232, 285, 42)] autorelease];
  [choose_backup_ setTitle:@"Choose Backup ZIP or Package…"];
  [choose_backup_ setButtonType:NSButtonTypeMomentaryPushIn];
  [choose_backup_ setBezelStyle:NSBezelStyleRounded];
  [choose_backup_ setTarget:self];
  [choose_backup_ setAction:@selector(chooseBackup:)];
  [content addSubview:choose_backup_];

  choose_folder_ = [[[NSButton alloc] initWithFrame:NSMakeRect(347, 232, 285, 42)] autorelease];
  [choose_folder_ setTitle:@"Use Extracted Game Folder…"];
  [choose_folder_ setButtonType:NSButtonTypeMomentaryPushIn];
  [choose_folder_ setBezelStyle:NSBezelStyleRounded];
  [choose_folder_ setTarget:self];
  [choose_folder_ setAction:@selector(chooseFolder:)];
  [content addSubview:choose_folder_];

  export_diagnostics_ = [[[NSButton alloc] initWithFrame:NSMakeRect(48, 174, 584, 42)] autorelease];
  [export_diagnostics_ setTitle:@"Export Diagnostic Bundle…"];
  [export_diagnostics_ setButtonType:NSButtonTypeMomentaryPushIn];
  [export_diagnostics_ setBezelStyle:NSBezelStyleRounded];
  [export_diagnostics_ setTarget:self];
  [export_diagnostics_ setAction:@selector(exportDiagnostics:)];
  [content addSubview:export_diagnostics_];

  progress_ =
      [[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(48, 142, 584, 18)] autorelease];
  [progress_ setStyle:NSProgressIndicatorStyleBar];
  [progress_ setIndeterminate:YES];
  [progress_ setDisplayedWhenStopped:NO];
  [content addSubview:progress_];

  status_ = [self labelWithFrame:NSMakeRect(48, 112, 584, 24)
                            text:@"Ready"
                            font:[NSFont systemFontOfSize:13]
                           color:[NSColor secondaryLabelColor]];
  [content addSubview:status_];

  NSTextField* notice = [self
      labelWithFrame:NSMakeRect(48, 32, 500, 58)
                text:@"Game files are not included or downloaded. Diagnostic exports contain "
                     @"logs and matching crash reports, never game data, saves, cache or settings."
                font:[NSFont systemFontOfSize:12]
               color:[NSColor tertiaryLabelColor]];
  [notice setLineBreakMode:NSLineBreakByWordWrapping];
  [notice setMaximumNumberOfLines:3];
  [content addSubview:notice];

  quit_ = [[[NSButton alloc] initWithFrame:NSMakeRect(558, 47, 74, 32)] autorelease];
  [quit_ setTitle:@"Quit"];
  [quit_ setBezelStyle:NSBezelStyleRounded];
  [quit_ setTarget:self];
  [quit_ setAction:@selector(quit:)];
  [content addSubview:quit_];

  BOOL ready = !paths_.game_data_root.empty();
  [play_ setEnabled:ready];
  [play_ setKeyEquivalent:ready ? @"\r" : @""];
  [choose_backup_ setKeyEquivalent:ready ? @"" : @"\r"];
  if (ready) {
    [explanation_
        setStringValue:@"Your game data is ready. Choose Play when you want to start, or export "
                       @"one easy-to-send diagnostic ZIP if you need help."];
    [status_ setStringValue:@"Ready to play."];
  } else {
    [status_ setStringValue:@"Choose your game data to continue."];
  }

  [window_ makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (void)setBusy:(BOOL)busy message:(NSString*)message {
  busy_.store(busy);
  BOOL exporting = exporting_.load();
  [play_ setEnabled:!busy && !paths_.game_data_root.empty()];
  [choose_backup_ setEnabled:!busy];
  [choose_folder_ setEnabled:!busy];
  [export_diagnostics_ setEnabled:!busy];
  [quit_ setEnabled:!busy || !exporting];
  [quit_ setTitle:(busy && !exporting) ? @"Cancel" : @"Quit"];
  [status_ setStringValue:message ?: @""];
  if (busy) {
    [progress_ setIndeterminate:YES];
    [progress_ setDoubleValue:0];
    [progress_ startAnimation:nil];
  } else {
    [progress_ stopAnimation:nil];
  }
}

- (void)showError:(std::string)message {
  cancel_requested_.store(false);
  exporting_.store(false);
  [self setBusy:NO message:@"Choose a different backup and try again."];
  NSAlert* alert = [[[NSAlert alloc] init] autorelease];
  [alert setAlertStyle:NSAlertStyleCritical];
  [alert setMessageText:@"Game data could not be imported"];
  [alert setInformativeText:ToNSString(std::string_view(message))];
  [alert addButtonWithTitle:@"OK"];
  [alert beginSheetModalForWindow:window_ completionHandler:nil];
}

- (void)showCancelled {
  cancel_requested_.store(false);
  exporting_.store(false);
  [self setBusy:NO message:@"Import cancelled. Your existing game data was not changed."];
}

- (void)updateProgress:(const ge::game_data::Progress&)value {
  [status_ setStringValue:ToNSString(std::string_view(value.message))];
  if (value.total != 0) {
    [progress_ setIndeterminate:NO];
    [progress_ setMinValue:0];
    [progress_ setMaxValue:static_cast<double>(value.total)];
    [progress_ setDoubleValue:static_cast<double>(value.completed)];
  }
}

- (void)configureWithRoot:(const std::filesystem::path&)root {
  paths_.game_data_root = root;
  WriteRememberedRoot(paths_.user_data_root, root);
  cancel_requested_.store(false);
  exporting_.store(false);
  [self setBusy:NO message:@"Game data is ready. Choose Play to start."];
  [explanation_
      setStringValue:@"Your game data is ready. Choose Play when you want to start, or export one "
                     @"easy-to-send diagnostic ZIP if you need help."];
  [choose_backup_ setKeyEquivalent:@""];
  [play_ setKeyEquivalent:@"\r"];
}

- (void)play:(id)sender {
  (void)sender;
  if (busy_.load() || paths_.game_data_root.empty() || !resume_) {
    return;
  }

  auto cached = paths_.user_data_root / "Game Data";
  auto validation = std::filesystem::is_directory(paths_.game_data_root)
                        ? (paths_.game_data_root.lexically_normal() == cached.lexically_normal()
                               ? ge::game_data::ValidateImportedDirectory(paths_.game_data_root)
                               : ge::game_data::ValidateDirectory(paths_.game_data_root))
                        : ge::game_data::ValidatePackage(paths_.game_data_root);
  if (!validation.valid) {
    paths_.game_data_root.clear();
    [play_ setEnabled:NO];
    [play_ setKeyEquivalent:@""];
    [choose_backup_ setKeyEquivalent:@"\r"];
    [explanation_
        setStringValue:@"The previously selected game data is no longer available. Choose your "
                       @"backup or extracted game folder again."];
    [self showError:validation.error];
    return;
  }

  [status_ setStringValue:@"Starting GoldenEye Metal…"];
  [window_ displayIfNeeded];

  auto resume = std::move(resume_);
  auto paths = paths_;
  resume(std::move(paths));

  [window_ orderOut:nil];
  [window_ setDelegate:nil];
  [window_ close];
  g_launcher = nil;
  [self release];
}

- (void)beginImport:(std::filesystem::path)source {
  if (busy_.exchange(true)) {
    return;
  }
  cancel_requested_.store(false);
  exporting_.store(false);
  [self setBusy:YES message:@"Inspecting backup…"];

  GoldenEyeLauncherController* controller = [self retain];
  rex::PathConfig paths = paths_;
  std::thread([controller, source = std::move(source), paths = std::move(paths)]() mutable {
    @autoreleasepool {
      std::filesystem::path temporary_package;
      ge::game_data::ImportResult result;
      std::string error;
      std::error_code ec;
      try {
        auto space = std::filesystem::space(paths.user_data_root, ec);
        if (ec) {
          std::filesystem::create_directories(paths.user_data_root, ec);
          space = std::filesystem::space(paths.user_data_root, ec);
        }
        if (!ec && space.available < kImportFreeSpace) {
          error = "At least 1.7 GB of free disk space is required for import.";
        }

        std::filesystem::path package = source;
        if (error.empty() && IsZip(source)) {
          dispatch_async(dispatch_get_main_queue(), ^{
            [controller->status_ setStringValue:@"Opening backup ZIP…"];
          });
          auto member = FindPackageMember(source, controller->cancel_requested_, &error);
          if (member) {
            temporary_package = TemporaryPackagePath(paths.user_data_root);
            if (ExtractPackageMember(source, *member, temporary_package,
                                     controller->cancel_requested_, &error)) {
              package = temporary_package;
            }
          }
        } else if (error.empty() && !IsStfs(source)) {
          error = "Choose the original backup ZIP or its Xbox LIVE package.";
        }

        if (error.empty()) {
          std::atomic<uint64_t> last_reported{0};
          std::string last_message;
          result = ge::game_data::ImportPackage(
              package, paths.user_data_root / "Game Data",
              [controller, &last_reported, &last_message](const ge::game_data::Progress& update) {
                bool message_changed = update.message != last_message;
                if (!message_changed && update.completed < last_reported.load() + 8 * 1024 * 1024 &&
                    update.completed != update.total) {
                  return;
                }
                last_message = update.message;
                last_reported.store(update.completed);
                ge::game_data::Progress copy = update;
                [controller retain];
                dispatch_async(dispatch_get_main_queue(), ^{
                  [controller updateProgress:copy];
                  [controller release];
                });
              },
              [controller] { return controller->cancel_requested_.load(); });
          if (!result) {
            error = result.error;
          }
        }
      } catch (const std::exception& exception) {
        error = std::string("The import failed unexpectedly: ") + exception.what();
      } catch (...) {
        error = "The import failed unexpectedly.";
      }

      if (!temporary_package.empty()) {
        std::filesystem::remove(temporary_package, ec);
      }

      bool was_cancelled =
          !result && (controller->cancel_requested_.load() || error == "Import cancelled.");
      if (was_cancelled) {
        dispatch_async(dispatch_get_main_queue(), ^{
          [controller showCancelled];
          [controller release];
        });
      } else if (!error.empty()) {
        dispatch_async(dispatch_get_main_queue(), ^{
          [controller showError:error];
          [controller release];
        });
      } else {
        auto root = result.game_data_root;
        dispatch_async(dispatch_get_main_queue(), ^{
          [controller configureWithRoot:root];
          [controller release];
        });
      }
    }
  }).detach();
}

- (void)chooseBackup:(id)sender {
  (void)sender;
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  [panel setTitle:@"Choose your GoldenEye game backup"];
  [panel setMessage:@"Select the backup ZIP or Xbox LIVE package stored on your Mac."];
  [panel setPrompt:@"Choose Backup"];
  [panel setCanChooseFiles:YES];
  [panel setCanChooseDirectories:NO];
  [panel setAllowsMultipleSelection:NO];
  [panel
      beginSheetModalForWindow:window_
             completionHandler:^(NSModalResponse response) {
               if (response == NSModalResponseOK) {
                 NSURL* url = [[panel URLs] firstObject];
                 if (url) {
                   [self beginImport:std::filesystem::path([[url path] fileSystemRepresentation])];
                 }
               }
             }];
}

- (void)chooseFolder:(id)sender {
  (void)sender;
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  [panel setTitle:@"Choose extracted GoldenEye game data"];
  [panel setMessage:@"Choose the folder that directly contains default.xex and files/."];
  [panel setPrompt:@"Use Folder"];
  [panel setCanChooseFiles:NO];
  [panel setCanChooseDirectories:YES];
  [panel setAllowsMultipleSelection:NO];
  [panel beginSheetModalForWindow:window_
                completionHandler:^(NSModalResponse response) {
                  if (response != NSModalResponseOK) {
                    return;
                  }
                  NSURL* url = [[panel URLs] firstObject];
                  if (!url) {
                    return;
                  }
                  std::filesystem::path root([[url path] fileSystemRepresentation]);
                  auto validation = ge::game_data::ValidateDirectory(root);
                  if (!validation.valid) {
                    [self showError:validation.error];
                    return;
                  }
                  [self configureWithRoot:root];
                }];
}

- (void)showDiagnosticError:(const std::string&)message {
  exporting_.store(false);
  [self setBusy:NO message:@"Diagnostic export failed. Try another save location."];
  NSAlert* alert = [[[NSAlert alloc] init] autorelease];
  [alert setAlertStyle:NSAlertStyleCritical];
  [alert setMessageText:@"Diagnostic bundle could not be exported"];
  [alert setInformativeText:ToNSString(std::string_view(message))];
  [alert addButtonWithTitle:@"OK"];
  [alert beginSheetModalForWindow:window_ completionHandler:nil];
}

- (void)beginDiagnosticExport:(std::filesystem::path)destination {
  if (busy_.exchange(true)) {
    return;
  }
  exporting_.store(true);
  cancel_requested_.store(false);
  [self setBusy:YES message:@"Collecting and protecting diagnostic logs…"];
  if (auto* logger = rex::GetLoggerRaw(rex::log::core())) {
    logger->flush();
  }

  GoldenEyeLauncherController* controller = [self retain];
  rex::PathConfig paths = paths_;
  std::thread([controller, paths = std::move(paths),
               destination = std::move(destination)]() mutable {
    @autoreleasepool {
      std::string error;
      bool success = false;
      try {
        success = ExportDiagnosticBundle(paths, destination, &error);
      } catch (const std::exception& exception) {
        error = std::string("Diagnostic export failed unexpectedly: ") + exception.what();
      } catch (...) {
        error = "Diagnostic export failed unexpectedly.";
      }

      dispatch_async(dispatch_get_main_queue(), ^{
        controller->exporting_.store(false);
        if (!success) {
          [controller showDiagnosticError:error.empty() ? "The diagnostic ZIP could not be created."
                                                        : error];
        } else {
          [controller setBusy:NO message:@"Diagnostic bundle saved and shown in Finder."];
          NSURL* saved_url = [NSURL fileURLWithPath:ToNSString(destination)];
          if (saved_url) {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ saved_url ]];
          }
        }
        [controller release];
      });
    }
  }).detach();
}

- (void)exportDiagnostics:(id)sender {
  (void)sender;
  NSDateFormatter* formatter = [[[NSDateFormatter alloc] init] autorelease];
  [formatter setLocale:[[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"] autorelease]];
  [formatter setDateFormat:@"yyyyMMdd-HHmmss"];
  NSString* filename = [NSString stringWithFormat:@"GoldenEye-Metal-Diagnostics-%@.zip",
                                                  [formatter stringFromDate:[NSDate date]]];

  NSSavePanel* panel = [NSSavePanel savePanel];
  [panel setTitle:@"Export GoldenEye Metal diagnostics"];
  [panel setMessage:@"Creates one easy-to-send ZIP with runtime logs, matching macOS crash "
                    @"reports and build information. Game data, saves, cache and settings are "
                    @"never included."];
  [panel setPrompt:@"Export"];
  [panel setNameFieldStringValue:filename];
  [panel setAllowedContentTypes:@[ UTTypeZIP ]];
  [panel setCanCreateDirectories:YES];
  [panel setExtensionHidden:NO];
  [panel beginSheetModalForWindow:window_
                completionHandler:^(NSModalResponse response) {
                  if (response != NSModalResponseOK || ![panel URL]) {
                    return;
                  }
                  std::filesystem::path destination([[[panel URL] path] fileSystemRepresentation]);
                  [self beginDiagnosticExport:std::move(destination)];
                }];
}

- (void)quit:(id)sender {
  (void)sender;
  if (busy_.load()) {
    if (exporting_.load()) {
      [status_ setStringValue:@"Please wait for the diagnostic export to finish…"];
      return;
    }
    cancel_requested_.store(true);
    [status_ setStringValue:@"Cancelling import…"];
    [quit_ setEnabled:NO];
    return;
  }
  app_context_->QuitFromUIThread();
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
  (void)sender;
  if (busy_.load()) {
    if (exporting_.load()) {
      [status_ setStringValue:@"Please wait for the diagnostic export to finish…"];
      return NO;
    }
    cancel_requested_.store(true);
    [status_ setStringValue:@"Cancelling import…"];
    [quit_ setEnabled:NO];
    return NO;
  }
  app_context_->QuitFromUIThread();
  return YES;
}

@end

namespace ge {

void ConfigureLauncherPaths(rex::PathConfig& paths) {
  auto support = ApplicationSupportRoot();
  if (REXCVAR_GET(user_data_root).empty()) {
    paths.user_data_root = support;
  }
  if (REXCVAR_GET(cache_path).empty()) {
    paths.cache_root = paths.user_data_root / "Cache";
  }
  paths.config_path = paths.user_data_root / "GoldenEyeMetal.toml";
  std::error_code ec;
  std::filesystem::create_directories(paths.user_data_root, ec);
  if (!ec) {
    CleanupStaleImportArtifacts(paths.user_data_root);
  }
}

std::optional<rex::PathConfig> PrepareLauncherPaths(const rex::PathConfig& defaults,
                                                    std::function<void(rex::PathConfig)> resume,
                                                    rex::ui::WindowedAppContext& app_context) {
  rex::PathConfig paths = defaults;
  bool force_setup = ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0;
  bool explicit_noninteractive = IsExplicitNoninteractiveLaunch();

  if (!paths.game_data_root.empty()) {
    bool valid = false;
    if (std::filesystem::is_directory(paths.game_data_root)) {
      valid = game_data::ValidateDirectory(paths.game_data_root).valid;
    } else {
      valid = game_data::ValidatePackage(paths.game_data_root).valid;
    }
    if (valid && explicit_noninteractive) {
      return paths;
    }
    if (!valid || force_setup) {
      paths.game_data_root.clear();
    }
  }
  if (explicit_noninteractive && paths.game_data_root.empty() &&
      (HasProcessArgument(@"--headless") ||
       (std::getenv("GOLDENEYE_LAUNCHER_BYPASS_UI") &&
        std::string_view(std::getenv("GOLDENEYE_LAUNCHER_BYPASS_UI")) == "1"))) {
    return paths;
  }

  if (!force_setup) {
    auto cached = paths.user_data_root / "Game Data";
    if (auto remembered = ReadRememberedRoot(paths.user_data_root)) {
      auto validation = remembered->lexically_normal() == cached.lexically_normal()
                            ? game_data::ValidateImportedDirectory(*remembered)
                            : game_data::ValidateDirectory(*remembered);
      if (validation.valid) {
        paths.game_data_root = *remembered;
      }
    }
    if (paths.game_data_root.empty() && game_data::ValidateImportedDirectory(cached).valid) {
      paths.game_data_root = cached;
    }
  }

  if (g_launcher) {
    return std::nullopt;
  }
  g_launcher = [[GoldenEyeLauncherController alloc] initWithPaths:paths
                                                           resume:std::move(resume)
                                                       appContext:&app_context];
  [g_launcher show];
  return std::nullopt;
}

}  // namespace ge
