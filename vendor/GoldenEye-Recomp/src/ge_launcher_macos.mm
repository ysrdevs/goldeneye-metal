#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "ge_launcher.h"

#include "ge_game_data.h"
#include "ge_launch_recovery.h"
#include "ge_save_manager.h"

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

ge::launch_recovery::StartupResult g_startup_recovery;
bool g_save_reconciliation_ok = true;
bool g_save_reconciliation_repaired = false;
std::string g_save_reconciliation_warning;

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

void ApplySafeModeOverrides() {
  // These are deliberately in-memory overrides applied after the saved config
  // has loaded. BeginSafeMode snapshots that config before this function runs,
  // and PrepareStartup restores it before config loading in the next process.
  // Safe Mode therefore lasts exactly one game run even if another settings
  // change happens to flush all current cvars while it is active.
  constexpr std::array<std::pair<std::string_view, std::string_view>, 10> overrides = {{
      {"gpu", "metal"},
      {"fullscreen", "false"},
      {"vsync", "true"},
      {"max_fps", "60"},
      {"metal_output_scaler", "bilinear"},
      {"anisotropic_override", "2"},
      {"swap_post_effect", "none"},
      {"postfx_enabled", "false"},
      {"metal_show_fps", "false"},
      {"metal_show_detailed_performance", "false"},
  }};
  for (const auto& [name, value] : overrides) {
    rex::cvar::SetFlagByName(name, value);
  }
  // The supported player-facing stability control. Its maximum trades a small
  // amount of latency for the most conservative GPU command pacing.
  rex::cvar::SetFlagByName("ge_gpu_throttle_us", "500");
  REXLOG_WARN(
      "GoldenEye Safe Mode active for this run: windowed, V-Sync, bilinear output, "
      "reduced filtering, no AA/Post-FX/overlay, maximum stability throttle");
}

NSWindow* FindGameplayWindow(NSWindow* launcher_window) {
  Class gameplay_view_class = NSClassFromString(@"RexMacContentView");
  if (!gameplay_view_class) {
    return nil;
  }
  for (NSWindow* candidate in [NSApp windows]) {
    if (candidate != launcher_window &&
        [[candidate contentView] isKindOfClass:gameplay_view_class]) {
      return candidate;
    }
  }
  return nil;
}

id g_game_window_close_observer = nil;

void ArmCleanRunMarkerRemoval(NSWindow* game_window,
                              const std::filesystem::path& user_data_root,
                              const std::filesystem::path& config_path, bool safe_mode) {
  if (!game_window || g_game_window_close_observer) {
    return;
  }
  // Observe this exact RexMac gameplay window. The launcher, diagnostic panels
  // and host settings windows cannot match the object filter and therefore
  // cannot incorrectly turn a forced game termination into a clean exit.
  id observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowWillCloseNotification
                  object:game_window
                   queue:nil
              usingBlock:^(NSNotification* notification) {
                (void)notification;
                if (safe_mode &&
                    !ge::launch_recovery::CancelSafeMode(user_data_root, config_path)) {
                  REXLOG_ERROR(
                      "GoldenEye Safe Mode ended cleanly, but the original settings could not "
                      "be restored yet; restoration will retry before the next config load");
                }
                if (ge::launch_recovery::EndRun(user_data_root)) {
                  REXLOG_INFO(
                      "GoldenEye game window accepted a clean close; recovery marker cleared");
                } else {
                  REXLOG_ERROR(
                      "GoldenEye game window closed cleanly, but its recovery marker could not "
                      "be cleared");
                }
                if (auto* logger = rex::GetLoggerRaw(rex::log::core())) {
                  logger->flush();
                }
              }];
  g_game_window_close_observer = [observer retain];
}

UTType* GoldenEyeSaveBackupType() {
  UTType* type = [UTType typeWithFilenameExtension:@"gesave" conformingToType:UTTypeData];
  return type ?: UTTypeData;
}

NSString* FormatSaveSnapshot(const ge::save::Snapshot& snapshot) {
  NSString* save_state = snapshot.has_save_game ? @"Save game: Found" : @"Save game: Not found";
  NSString* profile_state =
      snapshot.has_profile_settings ? @"Profile settings: Found" : @"Profile settings: Not found";
  NSString* size = [NSByteCountFormatter stringFromByteCount:static_cast<long long>(snapshot.byte_count)
                                                   countStyle:NSByteCountFormatterCountStyleFile];
  return [NSString stringWithFormat:@"%@\n%@  •  %zu file%@  •  %@", save_state, profile_state,
                                    snapshot.file_count, snapshot.file_count == 1 ? @"" : @"s",
                                    size];
}

NSString* FormatBackupInfo(const ge::save::BackupInfo& info) {
  NSMutableArray<NSString*>* contents = [NSMutableArray array];
  if (info.has_save_game) {
    [contents addObject:@"save game"];
  }
  if (info.has_profile_settings) {
    [contents addObject:@"profile settings"];
  }
  NSString* description = [contents count] ? [contents componentsJoinedByString:@" and "]
                                            : @"no managed data";
  NSString* size = [NSByteCountFormatter stringFromByteCount:static_cast<long long>(info.byte_count)
                                                   countStyle:NSByteCountFormatterCountStyleFile];
  return [NSString stringWithFormat:@"This verified backup contains %@ (%zu file%@, %@).",
                                    description, info.file_count, info.file_count == 1 ? @"" : @"s",
                                    size];
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
  RedactJSONIdentifier(&text, "sleepWakeUUID");
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

std::optional<std::string> FindLatestMetalPerformanceReport(
    const std::vector<std::filesystem::path>& newest_first_logs) {
  constexpr std::string_view marker = "Metal performance report:";
  for (const auto& log : newest_first_logs) {
    auto contents = ReadBoundedTextFile(log, kMaximumDiagnosticFileBytes);
    if (!contents) {
      continue;
    }
    const size_t marker_position = contents->rfind(marker);
    if (marker_position == std::string::npos) {
      continue;
    }
    size_t line_start = contents->rfind('\n', marker_position);
    line_start = line_start == std::string::npos ? 0 : line_start + 1;
    size_t line_end = contents->find('\n', marker_position);
    line_end = line_end == std::string::npos ? contents->size() : line_end;
    std::string line = contents->substr(line_start, line_end - line_start);
    // spdlog prefixes the message with time/category fields. Keep the stable,
    // human-readable payload only so diagnostics are easy to compare.
    const size_t payload = line.find(marker);
    if (payload != std::string::npos) {
      line.erase(0, payload);
    }
    line.push_back('\n');
    return line;
  }
  return std::nullopt;
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

std::optional<std::string> CurrentExecutableUUID() {
  const mach_header* header = _dyld_get_image_header(0);
  if (!header || header->magic != MH_MAGIC_64) {
    return std::nullopt;
  }

  const auto* header64 = reinterpret_cast<const mach_header_64*>(header);
  const auto* command_bytes = reinterpret_cast<const uint8_t*>(header64 + 1);
  size_t remaining = header64->sizeofcmds;
  for (uint32_t index = 0; index < header64->ncmds; ++index) {
    if (remaining < sizeof(load_command)) {
      return std::nullopt;
    }
    const auto* command = reinterpret_cast<const load_command*>(command_bytes);
    if (command->cmdsize < sizeof(load_command) || command->cmdsize > remaining) {
      return std::nullopt;
    }
    if (command->cmd == LC_UUID && command->cmdsize >= sizeof(uuid_command)) {
      const auto* uuid = reinterpret_cast<const uuid_command*>(command)->uuid;
      char formatted[37] = {};
      std::snprintf(
          formatted, sizeof(formatted),
          "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
          static_cast<unsigned>(uuid[0]), static_cast<unsigned>(uuid[1]),
          static_cast<unsigned>(uuid[2]), static_cast<unsigned>(uuid[3]),
          static_cast<unsigned>(uuid[4]), static_cast<unsigned>(uuid[5]),
          static_cast<unsigned>(uuid[6]), static_cast<unsigned>(uuid[7]),
          static_cast<unsigned>(uuid[8]), static_cast<unsigned>(uuid[9]),
          static_cast<unsigned>(uuid[10]), static_cast<unsigned>(uuid[11]),
          static_cast<unsigned>(uuid[12]), static_cast<unsigned>(uuid[13]),
          static_cast<unsigned>(uuid[14]), static_cast<unsigned>(uuid[15]));
      return std::string(formatted);
    }
    command_bytes += command->cmdsize;
    remaining -= command->cmdsize;
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
  auto uuid = CurrentExecutableUUID();
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

std::string BuildRecoveryReport() {
  std::ostringstream report;
  report << "GoldenEye Metal launcher recovery report\n"
         << "Previous game run interrupted: "
         << (g_startup_recovery.interrupted_run ? "yes" : "no") << "\n"
         << "Safe Mode config restored before loading: "
         << (g_startup_recovery.restored_safe_mode_config ? "yes" : "no") << "\n"
         << "Save transaction state safe: " << (g_save_reconciliation_ok ? "yes" : "no")
         << "\n"
         << "Interrupted save transaction repaired: "
         << (g_save_reconciliation_repaired ? "yes" : "no") << "\n";
  if (!g_startup_recovery.warning.empty()) {
    report << "Recovery warning: " << g_startup_recovery.warning << "\n";
  }
  if (!g_save_reconciliation_warning.empty()) {
    report << "Save recovery warning: " << g_save_reconciliation_warning << "\n";
  }
  report << "\nA run is considered clean only after the gameplay window accepts its close.\n"
         << "This report contains recovery status only; it never copies game data, save "
            "contents or settings values.\n";
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
  const auto runtime_logs = FindRuntimeLogs(paths.user_data_root);
  CopyDiagnosticFiles(runtime_logs, bundle_root / "Runtime Logs",
                      path_redactions, kMaximumRuntimeLogs, &copied_diagnostic_bytes, &log_stats,
                      &log_manifest);
  CopyDiagnosticFiles(FindCrashReports(home), bundle_root / "macOS Crash Reports", path_redactions,
                      kMaximumCrashReports, &copied_diagnostic_bytes, &crash_stats,
                      &crash_manifest);

  std::string write_error;
  if (!WritePrivateTextFile(bundle_root / "System.txt", BuildSystemReport(), &write_error) ||
      !WritePrivateTextFile(bundle_root / "Application.txt", BuildApplicationReport(),
                            &write_error) ||
      !WritePrivateTextFile(bundle_root / "Recovery.txt", BuildRecoveryReport(), &write_error)) {
    cleanup();
    *error = write_error;
    return false;
  }
  const auto performance_report = FindLatestMetalPerformanceReport(runtime_logs);
  if (performance_report &&
      !WritePrivateTextFile(bundle_root / "Metal Performance.txt", *performance_report,
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
      << "- app build identity, launcher recovery state and non-identifying system information\n";
  if (performance_report) {
    readme << "- the latest completed 60-second Metal performance report\n";
  }
  readme
      << "\n"
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
  NSButton* safe_mode_;
  NSButton* choose_backup_;
  NSButton* choose_folder_;
  NSButton* manage_saves_;
  NSButton* export_diagnostics_;
  NSButton* quit_;
  NSWindow* save_window_;
  NSTextField* save_summary_;
  NSTextField* save_status_;
  NSProgressIndicator* save_progress_;
  NSButton* save_backup_;
  NSButton* save_restore_;
  NSButton* save_reset_;
  NSButton* save_undo_;
  NSButton* save_done_;
  rex::PathConfig paths_;
  std::function<void(rex::PathConfig)> resume_;
  rex::ui::WindowedAppContext* app_context_;
  std::atomic_bool busy_;
  std::atomic_bool cancel_requested_;
  std::atomic_bool exporting_;
  std::atomic_bool save_work_active_;
  BOOL recovery_needed_;
  BOOL restored_safe_mode_config_;
  BOOL save_reconciliation_ok_;
  BOOL save_reconciliation_repaired_;
  BOOL save_snapshot_valid_;
  BOOL save_undo_is_redo_;
  ge::save::Snapshot save_snapshot_;
  std::filesystem::path save_undo_quarantine_;
  std::string startup_warning_;
}

- (instancetype)initWithPaths:(const rex::PathConfig&)paths
                       resume:(std::function<void(rex::PathConfig)>)resume
                   appContext:(rex::ui::WindowedAppContext*)appContext
                      recovery:(const ge::launch_recovery::StartupResult&)recovery;
- (void)show;
@end

namespace {
GoldenEyeLauncherController* g_launcher = nil;
}

@implementation GoldenEyeLauncherController

- (instancetype)initWithPaths:(const rex::PathConfig&)paths
                       resume:(std::function<void(rex::PathConfig)>)resume
                   appContext:(rex::ui::WindowedAppContext*)appContext
                      recovery:(const ge::launch_recovery::StartupResult&)recovery {
  self = [super init];
  if (self) {
    paths_ = paths;
    resume_ = std::move(resume);
    app_context_ = appContext;
    busy_.store(false);
    cancel_requested_.store(false);
    exporting_.store(false);
    save_work_active_.store(false);
    recovery_needed_ = recovery.interrupted_run ? YES : NO;
    restored_safe_mode_config_ = recovery.restored_safe_mode_config ? YES : NO;
    save_reconciliation_ok_ = g_save_reconciliation_ok ? YES : NO;
    save_reconciliation_repaired_ = g_save_reconciliation_repaired ? YES : NO;
    save_snapshot_valid_ = NO;
    save_undo_is_redo_ = NO;
    startup_warning_ = recovery.warning;
  }
  return self;
}

- (void)dealloc {
  [save_window_ setDelegate:nil];
  [save_window_ close];
  [save_window_ release];
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

  safe_mode_ = [[[NSButton alloc] initWithFrame:NSMakeRect(48, 288, 285, 48)] autorelease];
  [safe_mode_ setTitle:@"Start in Safe Mode"];
  [safe_mode_ setButtonType:NSButtonTypeMomentaryPushIn];
  [safe_mode_ setBezelStyle:NSBezelStyleRounded];
  [safe_mode_ setTarget:self];
  [safe_mode_ setAction:@selector(safeMode:)];
  [safe_mode_ setHidden:!recovery_needed_];
  [content addSubview:safe_mode_];

  if (recovery_needed_) {
    [play_ setFrame:NSMakeRect(347, 288, 285, 48)];
    [play_ setTitle:@"Play Normally"];
  } else if (!startup_warning_.empty()) {
    [play_ setTitle:@"Play Normally"];
  }

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

  manage_saves_ = [[[NSButton alloc] initWithFrame:NSMakeRect(48, 174, 285, 42)] autorelease];
  [manage_saves_ setTitle:@"Manage Saves…"];
  [manage_saves_ setButtonType:NSButtonTypeMomentaryPushIn];
  [manage_saves_ setBezelStyle:NSBezelStyleRounded];
  [manage_saves_ setTarget:self];
  [manage_saves_ setAction:@selector(manageSaves:)];
  [content addSubview:manage_saves_];

  export_diagnostics_ = [[[NSButton alloc] initWithFrame:NSMakeRect(347, 174, 285, 42)] autorelease];
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

  BOOL game_data_ready = !paths_.game_data_root.empty();
  BOOL ready = game_data_ready && save_reconciliation_ok_;
  [play_ setEnabled:ready];
  [manage_saves_ setEnabled:save_reconciliation_ok_];
  [safe_mode_ setEnabled:ready && recovery_needed_ && startup_warning_.empty()];
  [safe_mode_ setKeyEquivalent:(ready && recovery_needed_ && startup_warning_.empty()) ? @"\r"
                                                                                       : @""];
  [play_ setKeyEquivalent:(ready && (!recovery_needed_ || !startup_warning_.empty())) ? @"\r"
                                                                                       : @""];
  [choose_backup_ setKeyEquivalent:game_data_ready ? @"" : @"\r"];
  if (!save_reconciliation_ok_) {
    [explanation_ setTextColor:[NSColor systemRedColor]];
    [explanation_
        setStringValue:@"An interrupted save change could not be recovered safely. Play is "
                        "disabled so the game cannot open a partial save. Export diagnostics, "
                        "then quit and reopen GoldenEye Metal to retry recovery."];
    [status_ setStringValue:@"Save recovery is required before play can start."];
  } else if (!startup_warning_.empty()) {
    [explanation_ setTextColor:[NSColor systemOrangeColor]];
    std::string warning = startup_warning_;
    warning += " Play Normally or export diagnostics; Safe Mode is unavailable until this state "
               "can be repaired.";
    [explanation_ setStringValue:ToNSString(std::string_view(warning))];
    [status_ setStringValue:ready ? @"Play Normally remains available."
                                  : @"Choose your game data to continue."];
  } else if (recovery_needed_) {
    [explanation_ setTextColor:[NSColor systemOrangeColor]];
    [explanation_
        setStringValue:@"The previous game session did not close cleanly. Your files are safe. "
                       @"Use Safe Mode once, play normally, or export diagnostics for help."];
    [status_ setStringValue:ready ? @"Recovery options ready."
                                  : @"Choose your game data to use recovery options."];
  } else if (game_data_ready) {
    [explanation_
        setStringValue:@"Your game data is ready. Choose Play when you want to start, or export "
                       @"one easy-to-send diagnostic ZIP if you need help."];
    if (save_reconciliation_repaired_) {
      [status_ setStringValue:@"An interrupted save change was recovered. Ready to play."];
    } else {
      [status_ setStringValue:restored_safe_mode_config_
                                  ? @"Safe Mode finished; your original settings were restored."
                                  : @"Ready to play."];
    }
  } else {
    [status_ setStringValue:@"Choose your game data to continue."];
  }

  [window_ makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (void)updateSaveManagerControls {
  BOOL visible = save_window_ && [save_window_ isVisible];
  BOOL available = visible && !busy_.load();
  BOOL operations_available = available && save_reconciliation_ok_;
  BOOL has_managed_data = save_snapshot_valid_ && static_cast<bool>(save_snapshot_);
  [save_backup_ setEnabled:operations_available && has_managed_data];
  [save_restore_ setEnabled:operations_available];
  [save_reset_ setEnabled:operations_available && has_managed_data];
  [save_undo_ setHidden:save_undo_quarantine_.empty()];
  [save_undo_ setTitle:save_undo_is_redo_ ? @"Redo Last Change" : @"Undo Last Change"];
  [save_undo_ setEnabled:operations_available && !save_undo_quarantine_.empty()];
  [save_done_ setEnabled:available];
  if (save_work_active_.load()) {
    [save_progress_ startAnimation:nil];
  } else {
    [save_progress_ stopAnimation:nil];
  }
}

- (void)failClosedForSaveRecovery:(const std::string&)warning {
  save_reconciliation_ok_ = NO;
  save_reconciliation_repaired_ = NO;
  g_save_reconciliation_ok = false;
  g_save_reconciliation_repaired = false;
  g_save_reconciliation_warning =
      warning.empty() ? "An interrupted save transaction could not be reconciled." : warning;
  [play_ setEnabled:NO];
  [play_ setKeyEquivalent:@""];
  [safe_mode_ setEnabled:NO];
  [safe_mode_ setKeyEquivalent:@""];
  [manage_saves_ setEnabled:NO];
  [explanation_ setTextColor:[NSColor systemRedColor]];
  [explanation_
      setStringValue:@"Save recovery could not finish safely, so Play is disabled for this "
                      "session. Close Save Management, export diagnostics, then quit and reopen "
                      "GoldenEye Metal to retry recovery."];
  [status_ setStringValue:@"Save recovery failed safely — gameplay has not started."];
  [save_summary_ setTextColor:[NSColor systemRedColor]];
  [save_summary_ setStringValue:@"Save state is protected pending recovery."];
  [save_status_ setTextColor:[NSColor systemRedColor]];
  [save_status_ setStringValue:@"Close this window and export diagnostics."];
  [self updateSaveManagerControls];
}

- (void)setBusy:(BOOL)busy message:(NSString*)message {
  busy_.store(busy);
  BOOL exporting = exporting_.load();
  BOOL save_work = save_work_active_.load();
  BOOL save_manager_visible = save_window_ && [save_window_ isVisible];
  BOOL launcher_available = !busy && !save_manager_visible;
  BOOL launch_ready = !paths_.game_data_root.empty() && save_reconciliation_ok_;
  [play_ setEnabled:launcher_available && launch_ready];
  [safe_mode_ setEnabled:launcher_available && recovery_needed_ && launch_ready &&
                              startup_warning_.empty()];
  [choose_backup_ setEnabled:launcher_available];
  [choose_folder_ setEnabled:launcher_available];
  [manage_saves_ setEnabled:launcher_available && save_reconciliation_ok_];
  [export_diagnostics_ setEnabled:launcher_available];
  [quit_ setEnabled:!save_manager_visible && !save_work && (!busy || !exporting)];
  [quit_ setTitle:(busy && !exporting && !save_work) ? @"Cancel" : @"Quit"];
  [status_ setStringValue:message ?: @""];
  if (busy) {
    [progress_ setIndeterminate:YES];
    [progress_ setDoubleValue:0];
    [progress_ startAnimation:nil];
  } else {
    [progress_ stopAnimation:nil];
  }
  [self updateSaveManagerControls];
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
  [self setBusy:NO
         message:recovery_needed_ ? @"Game data is ready. Choose a recovery launch option."
                                  : @"Game data is ready. Choose Play to start."];
  if (!startup_warning_.empty()) {
    [explanation_ setTextColor:[NSColor systemOrangeColor]];
    std::string warning = startup_warning_;
    warning += " Play Normally or export diagnostics; Safe Mode is unavailable until this state "
               "can be repaired.";
    [explanation_ setStringValue:ToNSString(std::string_view(warning))];
  } else if (recovery_needed_) {
    [explanation_ setTextColor:[NSColor systemOrangeColor]];
    [explanation_
        setStringValue:@"The previous game session did not close cleanly. Your files are safe. "
                       @"Use Safe Mode once, play normally, or export diagnostics for help."];
  } else {
    [explanation_ setTextColor:[NSColor labelColor]];
    [explanation_
        setStringValue:@"Your game data is ready. Choose Play when you want to start, or export "
                       @"one easy-to-send diagnostic ZIP if you need help."];
  }
  [choose_backup_ setKeyEquivalent:@""];
  [safe_mode_ setKeyEquivalent:(startup_warning_.empty() && recovery_needed_) ? @"\r" : @""];
  [play_ setKeyEquivalent:(!recovery_needed_ || !startup_warning_.empty()) ? @"\r" : @""];
}

- (void)createSaveManagerWindowIfNeeded {
  if (save_window_) {
    return;
  }

  NSRect frame = NSMakeRect(0, 0, 600, 430);
  NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
  save_window_ = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:style
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
  [save_window_ setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
  [save_window_ setTitle:@"GoldenEye Save Management"];
  [save_window_ setReleasedWhenClosed:NO];
  [save_window_ setDelegate:self];
  [save_window_ center];

  NSView* content = [save_window_ contentView];
  [content setWantsLayer:YES];
  [content layer].backgroundColor = [[NSColor colorWithCalibratedRed:0.055
                                                               green:0.06
                                                                blue:0.07
                                                               alpha:1.0] CGColor];

  NSTextField* title = [self labelWithFrame:NSMakeRect(36, 364, 528, 40)
                                       text:@"Manage Saves"
                                       font:[NSFont systemFontOfSize:25 weight:NSFontWeightBold]
                                      color:[NSColor colorWithCalibratedRed:0.88
                                                                      green:0.70
                                                                       blue:0.22
                                                                      alpha:1.0]];
  [content addSubview:title];

  save_summary_ = [self labelWithFrame:NSMakeRect(36, 294, 528, 58)
                                  text:@"Checking for GoldenEye save data…"
                                  font:[NSFont systemFontOfSize:15 weight:NSFontWeightMedium]
                                 color:[NSColor labelColor]];
  [save_summary_ setLineBreakMode:NSLineBreakByWordWrapping];
  [save_summary_ setMaximumNumberOfLines:3];
  [content addSubview:save_summary_];

  save_backup_ = [[[NSButton alloc] initWithFrame:NSMakeRect(36, 228, 252, 44)] autorelease];
  [save_backup_ setTitle:@"Back Up Save…"];
  [save_backup_ setBezelStyle:NSBezelStyleRounded];
  [save_backup_ setTarget:self];
  [save_backup_ setAction:@selector(backupSave:)];
  [content addSubview:save_backup_];

  save_restore_ = [[[NSButton alloc] initWithFrame:NSMakeRect(312, 228, 252, 44)] autorelease];
  [save_restore_ setTitle:@"Restore Backup…"];
  [save_restore_ setBezelStyle:NSBezelStyleRounded];
  [save_restore_ setTarget:self];
  [save_restore_ setAction:@selector(restoreSave:)];
  [content addSubview:save_restore_];

  save_reset_ = [[[NSButton alloc] initWithFrame:NSMakeRect(36, 172, 252, 44)] autorelease];
  [save_reset_ setTitle:@"Reset to Fresh…"];
  [save_reset_ setBezelStyle:NSBezelStyleRounded];
  [save_reset_ setTarget:self];
  [save_reset_ setAction:@selector(resetSave:)];
  [content addSubview:save_reset_];

  save_undo_ = [[[NSButton alloc] initWithFrame:NSMakeRect(312, 172, 252, 44)] autorelease];
  [save_undo_ setTitle:@"Undo Last Change"];
  [save_undo_ setBezelStyle:NSBezelStyleRounded];
  [save_undo_ setTarget:self];
  [save_undo_ setAction:@selector(undoSaveChange:)];
  [save_undo_ setHidden:YES];
  [content addSubview:save_undo_];

  save_progress_ =
      [[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(36, 142, 528, 14)] autorelease];
  [save_progress_ setStyle:NSProgressIndicatorStyleBar];
  [save_progress_ setIndeterminate:YES];
  [save_progress_ setDisplayedWhenStopped:NO];
  [content addSubview:save_progress_];

  save_status_ = [self labelWithFrame:NSMakeRect(36, 108, 528, 24)
                                 text:@"Ready"
                                 font:[NSFont systemFontOfSize:13]
                                color:[NSColor secondaryLabelColor]];
  [content addSubview:save_status_];

  NSTextField* notice = [self
      labelWithFrame:NSMakeRect(36, 46, 438, 48)
                text:@"Restore and Reset preserve replaced data in a private quarantine. "
                     @"Game files and launcher settings are never included in save backups."
                font:[NSFont systemFontOfSize:12]
               color:[NSColor tertiaryLabelColor]];
  [notice setLineBreakMode:NSLineBreakByWordWrapping];
  [notice setMaximumNumberOfLines:3];
  [content addSubview:notice];

  save_done_ = [[[NSButton alloc] initWithFrame:NSMakeRect(490, 48, 74, 32)] autorelease];
  [save_done_ setTitle:@"Done"];
  [save_done_ setBezelStyle:NSBezelStyleRounded];
  [save_done_ setTarget:self];
  [save_done_ setAction:@selector(closeSaveManager:)];
  [content addSubview:save_done_];
}

- (BOOL)beginSaveWork:(NSString*)message {
  if (busy_.exchange(true)) {
    return NO;
  }
  save_work_active_.store(true);
  cancel_requested_.store(false);
  [self setBusy:YES message:message];
  [save_status_ setTextColor:[NSColor secondaryLabelColor]];
  [save_status_ setStringValue:message ?: @"Working…"];
  return YES;
}

- (void)finishSaveWork:(NSString*)launcherMessage saveMessage:(NSString*)saveMessage {
  save_work_active_.store(false);
  [self setBusy:NO message:launcherMessage ?: @"Save Management is open."];
  [save_status_ setTextColor:[NSColor secondaryLabelColor]];
  [save_status_ setStringValue:saveMessage ?: @"Ready"];
}

- (void)applySaveSnapshot:(const ge::save::Snapshot&)snapshot {
  save_snapshot_ = snapshot;
  save_snapshot_valid_ = YES;
  [save_summary_ setTextColor:[NSColor labelColor]];
  [save_summary_ setStringValue:FormatSaveSnapshot(snapshot)];
  [self updateSaveManagerControls];
}

- (void)showSaveError:(const std::string&)message title:(NSString*)title {
  [save_status_ setTextColor:[NSColor systemRedColor]];
  [save_status_ setStringValue:ToNSString(std::string_view(message))];
  NSAlert* alert = [[[NSAlert alloc] init] autorelease];
  [alert setAlertStyle:NSAlertStyleCritical];
  [alert setMessageText:title ?: @"Save operation failed"];
  [alert setInformativeText:ToNSString(std::string_view(message))];
  [alert addButtonWithTitle:@"OK"];
  NSWindow* parent = save_window_ && [save_window_ isVisible] ? save_window_ : window_;
  [alert beginSheetModalForWindow:parent completionHandler:nil];
}

- (void)refreshSaveSnapshot {
  if (![self beginSaveWork:@"Checking GoldenEye save data…"]) {
    return;
  }
  GoldenEyeLauncherController* controller = [self retain];
  auto root = paths_.user_data_root;
  std::thread([controller, root = std::move(root)]() mutable {
    @autoreleasepool {
      ge::save::Snapshot snapshot;
      ge::save::Status status = ge::save::Discover(root, &snapshot);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (status) {
          [controller applySaveSnapshot:snapshot];
          [controller finishSaveWork:@"Save Management is open." saveMessage:@"Ready"];
        } else {
          controller->save_snapshot_valid_ = NO;
          [controller->save_summary_ setTextColor:[NSColor systemRedColor]];
          [controller->save_summary_ setStringValue:@"Save data could not be inspected safely."];
          [controller finishSaveWork:@"Save inspection failed."
                         saveMessage:@"Save data could not be inspected."];
          [controller showSaveError:status.message title:@"Save data could not be inspected"];
        }
        [controller release];
      });
    }
  }).detach();
}

- (void)manageSaves:(id)sender {
  (void)sender;
  if (busy_.load() || !save_reconciliation_ok_) {
    if (!save_reconciliation_ok_) {
      [status_ setStringValue:@"Save recovery must finish before saves can be managed."];
    }
    return;
  }
  [self createSaveManagerWindowIfNeeded];
  [save_window_ makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  [self setBusy:NO message:@"Save Management is open."];
  [self refreshSaveSnapshot];
}

- (void)closeSaveManager:(id)sender {
  (void)sender;
  if (busy_.load() || save_work_active_.load()) {
    [save_status_ setStringValue:@"Please wait for the save operation to finish…"];
    return;
  }
  [save_window_ orderOut:nil];
  [window_ makeKeyAndOrderFront:nil];
  [self setBusy:NO
          message:save_reconciliation_ok_
                      ? @"Ready."
                      : @"Save recovery failed safely — export diagnostics, then restart."];
}

- (void)beginSaveBackup:(std::filesystem::path)destination {
  if (![self beginSaveWork:@"Creating verified save backup…"]) {
    return;
  }
  GoldenEyeLauncherController* controller = [self retain];
  auto root = paths_.user_data_root;
  std::thread([controller, root = std::move(root),
               destination = std::move(destination)]() mutable {
    @autoreleasepool {
      ge::save::BackupInfo info;
      ge::save::Status status = ge::save::CreateBackup(root, destination, &info);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (status) {
          [controller finishSaveWork:@"Save backup created."
                         saveMessage:@"Backup saved and verified."];
          NSURL* saved_url = [NSURL fileURLWithPath:ToNSString(destination)];
          if (saved_url) {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ saved_url ]];
          }
        } else {
          [controller finishSaveWork:@"Save backup failed."
                         saveMessage:@"No backup was created."];
          [controller showSaveError:status.message title:@"Save backup could not be created"];
        }
        [controller release];
      });
    }
  }).detach();
}

- (void)backupSave:(id)sender {
  (void)sender;
  if (busy_.load() || !save_snapshot_valid_ || !static_cast<bool>(save_snapshot_)) {
    return;
  }
  NSDateFormatter* formatter = [[[NSDateFormatter alloc] init] autorelease];
  [formatter setLocale:[[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"] autorelease]];
  [formatter setDateFormat:@"yyyyMMdd-HHmmss"];
  NSString* filename = [NSString stringWithFormat:@"GoldenEye-Save-%@.gesave",
                                                  [formatter stringFromDate:[NSDate date]]];
  NSSavePanel* panel = [NSSavePanel savePanel];
  [panel setTitle:@"Back up GoldenEye save data"];
  [panel setMessage:@"Creates one verified .gesave file containing only GoldenEye save and "
                    @"title-profile data."];
  [panel setPrompt:@"Back Up"];
  [panel setNameFieldStringValue:filename];
  [panel setAllowedContentTypes:@[ GoldenEyeSaveBackupType() ]];
  [panel setCanCreateDirectories:YES];
  [panel setExtensionHidden:NO];
  [panel beginSheetModalForWindow:save_window_
                completionHandler:^(NSModalResponse response) {
                  if (response != NSModalResponseOK || ![panel URL]) {
                    [save_status_ setStringValue:@"Backup cancelled."];
                    return;
                  }
                  std::filesystem::path destination([[[panel URL] path] fileSystemRepresentation]);
                  if (LowerASCII(destination.extension().string()) != ge::save::kBackupExtension) {
                    destination.replace_extension(ge::save::kBackupExtension);
                  }
                  [self beginSaveBackup:std::move(destination)];
                }];
}

- (void)beginRestoreBackup:(std::filesystem::path)archive {
  if (![self beginSaveWork:@"Restoring verified save backup…"]) {
    return;
  }
  GoldenEyeLauncherController* controller = [self retain];
  auto root = paths_.user_data_root;
  std::thread([controller, root = std::move(root), archive = std::move(archive)]() mutable {
    @autoreleasepool {
      ge::save::MutationResult result = ge::save::RestoreBackup(root, archive);
      ge::save::RecoveryResult recovery;
      if (!result) {
        recovery = ge::save::RecoverInterruptedTransaction(root);
      }
      ge::save::Snapshot snapshot;
      ge::save::Status discovery;
      if (result || recovery) {
        discovery = ge::save::Discover(root, &snapshot);
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        if (result) {
          controller->save_undo_quarantine_ = result.quarantine_path;
          controller->save_undo_is_redo_ = NO;
          if (discovery) {
            [controller applySaveSnapshot:snapshot];
          } else {
            controller->save_snapshot_valid_ = NO;
            [controller->save_summary_ setStringValue:@"Save restored; refresh failed."];
          }
          NSString* message = result.quarantine_path.empty()
                                  ? @"Backup restored. There was no prior save to quarantine."
                                  : @"Backup restored. Undo Last Change is available now.";
          [controller finishSaveWork:@"Save backup restored." saveMessage:message];
          if (!discovery) {
            [controller showSaveError:discovery.message
                                title:@"Save restored, but status could not be refreshed"];
          }
        } else {
          if (!recovery) {
            std::string failure = result.status.message;
            if (!failure.empty()) {
              failure += " ";
            }
            failure += recovery.status.message;
            [controller finishSaveWork:@"Save recovery is required."
                           saveMessage:@"The launcher stopped safely before gameplay."];
            [controller failClosedForSaveRecovery:recovery.status.message];
            [controller showSaveError:failure title:@"Save recovery could not finish"];
          } else {
            if (discovery) {
              [controller applySaveSnapshot:snapshot];
            }
            [controller finishSaveWork:@"Save restore failed."
                           saveMessage:@"Your existing save was recovered safely."];
            [controller showSaveError:result.status.message
                                  title:@"Save backup could not be restored"];
          }
        }
        [controller release];
      });
    }
  }).detach();
}

- (void)inspectAndConfirmRestore:(std::filesystem::path)archive {
  if (![self beginSaveWork:@"Inspecting save backup…"]) {
    return;
  }
  GoldenEyeLauncherController* controller = [self retain];
  std::thread([controller, archive = std::move(archive)]() mutable {
    @autoreleasepool {
      ge::save::BackupInfo info;
      ge::save::Status status = ge::save::InspectBackup(archive, &info);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!status) {
          [controller finishSaveWork:@"Save backup inspection failed."
                         saveMessage:@"The selected file was not changed."];
          [controller showSaveError:status.message title:@"This is not a valid GoldenEye backup"];
          [controller release];
          return;
        }

        [controller finishSaveWork:@"Verified backup ready for confirmation."
                       saveMessage:@"Backup verified. Confirm restore to continue."];
        NSAlert* alert = [[[NSAlert alloc] init] autorelease];
        [alert setAlertStyle:NSAlertStyleWarning];
        [alert setMessageText:@"Restore this GoldenEye save backup?"];
        NSString* detail = [NSString
            stringWithFormat:@"%@\n\nCurrent GoldenEye save/profile data will be moved into a "
                             @"private quarantine first. You can immediately undo the restore.",
                             FormatBackupInfo(info)];
        [alert setInformativeText:detail];
        [alert addButtonWithTitle:@"Restore Backup"];
        [alert addButtonWithTitle:@"Cancel"];
        [alert beginSheetModalForWindow:controller->save_window_
                      completionHandler:^(NSModalResponse response) {
                        if (response == NSAlertFirstButtonReturn) {
                          [controller beginRestoreBackup:archive];
                        } else {
                          [controller->save_status_ setStringValue:@"Restore cancelled."];
                        }
                      }];
        [controller release];
      });
    }
  }).detach();
}

- (void)restoreSave:(id)sender {
  (void)sender;
  if (busy_.load()) {
    return;
  }
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  [panel setTitle:@"Choose a GoldenEye save backup"];
  [panel setMessage:@"Select a .gesave backup. It will be fully inspected before restore is "
                    @"offered."];
  [panel setPrompt:@"Inspect Backup"];
  [panel setCanChooseFiles:YES];
  [panel setCanChooseDirectories:NO];
  [panel setAllowsMultipleSelection:NO];
  [panel setAllowedContentTypes:@[ GoldenEyeSaveBackupType() ]];
  [panel beginSheetModalForWindow:save_window_
                completionHandler:^(NSModalResponse response) {
                  if (response != NSModalResponseOK || ![[panel URLs] firstObject]) {
                    [save_status_ setStringValue:@"Restore cancelled."];
                    return;
                  }
                  NSURL* url = [[panel URLs] firstObject];
                  std::filesystem::path archive([[url path] fileSystemRepresentation]);
                  [self inspectAndConfirmRestore:std::move(archive)];
                }];
}

- (void)beginResetSave {
  if (![self beginSaveWork:@"Moving save data into private quarantine…"]) {
    return;
  }
  GoldenEyeLauncherController* controller = [self retain];
  auto root = paths_.user_data_root;
  std::thread([controller, root = std::move(root)]() mutable {
    @autoreleasepool {
      ge::save::MutationResult result = ge::save::ResetToFresh(root);
      ge::save::RecoveryResult recovery;
      if (!result) {
        recovery = ge::save::RecoverInterruptedTransaction(root);
      }
      ge::save::Snapshot snapshot;
      ge::save::Status discovery;
      if (result || recovery) {
        discovery = ge::save::Discover(root, &snapshot);
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        if (result) {
          controller->save_undo_quarantine_ = result.quarantine_path;
          controller->save_undo_is_redo_ = NO;
          if (discovery) {
            [controller applySaveSnapshot:snapshot];
          } else {
            controller->save_snapshot_valid_ = NO;
            [controller->save_summary_ setStringValue:@"Save reset; refresh failed."];
          }
          [controller finishSaveWork:@"GoldenEye save reset."
                         saveMessage:@"Save reset to fresh. Undo Last Change is available now."];
          if (!discovery) {
            [controller showSaveError:discovery.message
                                title:@"Save reset, but status could not be refreshed"];
          }
        } else {
          if (!recovery) {
            std::string failure = result.status.message;
            if (!failure.empty()) {
              failure += " ";
            }
            failure += recovery.status.message;
            [controller finishSaveWork:@"Save recovery is required."
                           saveMessage:@"The launcher stopped safely before gameplay."];
            [controller failClosedForSaveRecovery:recovery.status.message];
            [controller showSaveError:failure title:@"Save recovery could not finish"];
          } else {
            if (discovery) {
              [controller applySaveSnapshot:snapshot];
            }
            [controller finishSaveWork:@"Save reset failed."
                           saveMessage:@"No save data was removed; the prior state is safe."];
            [controller showSaveError:result.status.message
                                  title:@"GoldenEye save could not be reset"];
          }
        }
        [controller release];
      });
    }
  }).detach();
}

- (void)resetSave:(id)sender {
  (void)sender;
  if (busy_.load() || !save_snapshot_valid_ || !static_cast<bool>(save_snapshot_)) {
    return;
  }
  NSAlert* alert = [[[NSAlert alloc] init] autorelease];
  [alert setAlertStyle:NSAlertStyleCritical];
  [alert setMessageText:@"Reset GoldenEye save and profile data?"];
  [alert setInformativeText:@"This starts GoldenEye fresh. Existing managed save/profile data is "
                            @"moved into a private quarantine rather than deleted, and Undo Last "
                            @"Change will be offered immediately."];
  [alert addButtonWithTitle:@"Reset Save Data"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert beginSheetModalForWindow:save_window_
                completionHandler:^(NSModalResponse response) {
                  if (response == NSAlertFirstButtonReturn) {
                    [self beginResetSave];
                  } else {
                    [save_status_ setStringValue:@"Reset cancelled."];
                  }
                }];
}

- (void)undoSaveChange:(id)sender {
  (void)sender;
  if (busy_.load() || save_undo_quarantine_.empty()) {
    return;
  }
  const BOOL was_redo = save_undo_is_redo_;
  if (![self beginSaveWork:was_redo ? @"Redoing last save change…"
                                    : @"Undoing last save change…"]) {
    return;
  }
  GoldenEyeLauncherController* controller = [self retain];
  auto root = paths_.user_data_root;
  auto quarantine = save_undo_quarantine_;
  std::thread([controller, root = std::move(root), quarantine = std::move(quarantine),
               was_redo]() mutable {
    @autoreleasepool {
      ge::save::MutationResult result = ge::save::UndoQuarantine(root, quarantine);
      ge::save::RecoveryResult recovery;
      if (!result) {
        recovery = ge::save::RecoverInterruptedTransaction(root);
      }
      ge::save::Snapshot snapshot;
      ge::save::Status discovery;
      if (result || recovery) {
        discovery = ge::save::Discover(root, &snapshot);
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        if (result) {
          controller->save_undo_quarantine_ = result.quarantine_path;
          controller->save_undo_is_redo_ =
              result.quarantine_path.empty() ? NO : (was_redo ? NO : YES);
          if (discovery) {
            [controller applySaveSnapshot:snapshot];
          } else {
            controller->save_snapshot_valid_ = NO;
            [controller->save_summary_ setStringValue:@"Undo completed; refresh failed."];
          }
          NSString* launcher_message = was_redo ? @"Save change reapplied."
                                                 : @"Last save change undone.";
          NSString* save_message =
              result.quarantine_path.empty()
                  ? (was_redo ? @"Save change reapplied." : @"Previous save data restored.")
                  : (was_redo ? @"Save change reapplied. Undo Last Change is available."
                              : @"Previous save restored. Redo Last Change is available.");
          [controller finishSaveWork:launcher_message saveMessage:save_message];
          if (!discovery) {
            [controller showSaveError:discovery.message
                                title:@"Undo completed, but status could not be refreshed"];
          }
        } else {
          if (!result.quarantine_path.empty()) {
            controller->save_undo_quarantine_ = result.quarantine_path;
          }
          if (!recovery) {
            std::string failure = result.status.message;
            if (!failure.empty()) {
              failure += " ";
            }
            failure += recovery.status.message;
            [controller finishSaveWork:@"Save recovery is required."
                           saveMessage:@"The launcher stopped safely before gameplay."];
            [controller failClosedForSaveRecovery:recovery.status.message];
            [controller showSaveError:failure title:@"Save recovery could not finish"];
          } else {
            if (discovery) {
              [controller applySaveSnapshot:snapshot];
            }
            [controller finishSaveWork:@"Undo failed."
                           saveMessage:@"The prior save state was recovered safely."];
            [controller showSaveError:result.status.message
                                  title:@"Last save change could not be undone"];
          }
        }
        [controller release];
      });
    }
  }).detach();
}

- (void)launchGameInSafeMode:(BOOL)safeMode {
  if (busy_.load() || paths_.game_data_root.empty() || !resume_) {
    return;
  }
  if (!save_reconciliation_ok_) {
    [status_ setStringValue:@"Play is disabled until the interrupted save change is recovered."];
    return;
  }
  if (safeMode && !startup_warning_.empty()) {
    [status_ setStringValue:@"Safe Mode is unavailable; choose Play Normally or export diagnostics."];
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
    [safe_mode_ setEnabled:NO];
    [play_ setKeyEquivalent:@""];
    [choose_backup_ setKeyEquivalent:@"\r"];
    [explanation_
        setStringValue:@"The previously selected game data is no longer available. Choose your "
                       @"backup or extracted game folder again."];
    [self showError:validation.error];
    return;
  }

  std::string recovery_error;
  if (safeMode && !ge::launch_recovery::BeginSafeMode(
                      paths_.user_data_root, paths_.config_path, &recovery_error)) {
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setAlertStyle:NSAlertStyleCritical];
    [alert setMessageText:@"Safe Mode could not be prepared"];
    [alert setInformativeText:ToNSString(std::string_view(recovery_error))];
    [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:window_ completionHandler:nil];
    [status_ setStringValue:@"No settings were changed. You can play normally or try again."];
    return;
  }
  if (!ge::launch_recovery::BeginRun(paths_.user_data_root, &recovery_error)) {
    if (safeMode) {
      (void)ge::launch_recovery::CancelSafeMode(paths_.user_data_root, paths_.config_path);
    }
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setAlertStyle:NSAlertStyleCritical];
    [alert setMessageText:@"Crash recovery could not be prepared"];
    [alert setInformativeText:ToNSString(std::string_view(recovery_error))];
    [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:window_ completionHandler:nil];
    [status_ setStringValue:@"The game was not started. Your files were not changed."];
    return;
  }
  if (safeMode) {
    ApplySafeModeOverrides();
  }

  [status_ setStringValue:safeMode ? @"Starting GoldenEye Metal in Safe Mode…"
                                      : @"Starting GoldenEye Metal…"];
  [window_ displayIfNeeded];

  auto resume = std::move(resume_);
  auto paths = paths_;
  resume(std::move(paths));

  NSWindow* game_window = FindGameplayWindow(window_);
  if (game_window) {
    ArmCleanRunMarkerRemoval(game_window, paths_.user_data_root, paths_.config_path,
                             safeMode == YES);
  } else {
    // A failed initialization intentionally leaves the marker behind so the
    // next interactive launch offers recovery and diagnostics.
    REXLOG_ERROR(
        "GoldenEye gameplay window was not created; leaving recovery marker for next launch");
  }

  [window_ orderOut:nil];
  [window_ setDelegate:nil];
  [window_ close];
  g_launcher = nil;
  [self release];
}

- (void)play:(id)sender {
  (void)sender;
  [self launchGameInSafeMode:NO];
}

- (void)safeMode:(id)sender {
  (void)sender;
  [self launchGameInSafeMode:YES];
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
  if (save_window_ && [save_window_ isVisible]) {
    [self closeSaveManager:nil];
    return;
  }
  if (busy_.load()) {
    if (save_work_active_.load()) {
      [status_ setStringValue:@"Please wait for the save operation to finish…"];
      return;
    }
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
  if (sender == save_window_) {
    if (save_work_active_.load() || busy_.load()) {
      [save_status_ setStringValue:@"Please wait for the save operation to finish…"];
      return NO;
    }
    [self closeSaveManager:nil];
    return NO;
  }
  if (save_window_ && [save_window_ isVisible]) {
    [save_status_ setStringValue:@"Close Save Management before quitting GoldenEye Metal."];
    [save_window_ makeKeyAndOrderFront:nil];
    return NO;
  }
  if (busy_.load()) {
    if (save_work_active_.load()) {
      [status_ setStringValue:@"Please wait for the save operation to finish…"];
      return NO;
    }
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
  g_save_reconciliation_ok = true;
  g_save_reconciliation_repaired = false;
  g_save_reconciliation_warning.clear();
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
    // ReXApp loads config immediately after this hook returns. Restore the
    // player's pre-Safe-Mode snapshot here so temporary one-run overrides can
    // never become the input configuration for a later process.
    g_startup_recovery =
        launch_recovery::PrepareStartup(paths.user_data_root, paths.config_path);
    const auto save_recovery = save::RecoverInterruptedTransaction(paths.user_data_root);
    g_save_reconciliation_ok = static_cast<bool>(save_recovery);
    g_save_reconciliation_repaired = save_recovery.recovered;
    if (!save_recovery) {
      g_save_reconciliation_warning = save_recovery.status.message;
    }
    CleanupStaleImportArtifacts(paths.user_data_root);
  } else {
    g_startup_recovery.warning =
        "Crash recovery could not access the private Application Support folder.";
    g_save_reconciliation_ok = false;
    g_save_reconciliation_warning =
        "Save recovery could not access the private Application Support folder.";
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
    if (valid && explicit_noninteractive && g_save_reconciliation_ok) {
      return paths;
    }
    if (!valid || force_setup) {
      paths.game_data_root.clear();
    }
  }
  if (g_save_reconciliation_ok && explicit_noninteractive && paths.game_data_root.empty() &&
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
                                                       appContext:&app_context
                                                          recovery:g_startup_recovery];
  [g_launcher show];
  return std::nullopt;
}

}  // namespace ge
