#import <Cocoa/Cocoa.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ge_launcher.h"

#include "ge_game_data.h"

#include <rex/cvar.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

}  // namespace

@interface GoldenEyeLauncherController : NSObject <NSWindowDelegate> {
 @private
  NSWindow* window_;
  NSTextField* status_;
  NSProgressIndicator* progress_;
  NSButton* choose_backup_;
  NSButton* choose_folder_;
  NSButton* quit_;
  rex::PathConfig paths_;
  std::function<void(rex::PathConfig)> resume_;
  rex::ui::WindowedAppContext* app_context_;
  std::atomic_bool busy_;
  std::atomic_bool cancel_requested_;
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

  NSRect frame = NSMakeRect(0, 0, 680, 470);
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

  NSImageView* icon = [[[NSImageView alloc] initWithFrame:NSMakeRect(46, 350, 82, 82)] autorelease];
  [icon setImage:[NSApp applicationIconImage]];
  [icon setImageScaling:NSImageScaleProportionallyUpOrDown];
  [content addSubview:icon];

  NSTextField* title = [self labelWithFrame:NSMakeRect(148, 382, 480, 42)
                                       text:@"GoldenEye Metal"
                                       font:[NSFont systemFontOfSize:30 weight:NSFontWeightBold]
                                      color:[NSColor colorWithCalibratedRed:0.88
                                                                      green:0.70
                                                                       blue:0.22
                                                                      alpha:1.0]];
  [content addSubview:title];
  NSTextField* subtitle = [self labelWithFrame:NSMakeRect(150, 351, 475, 30)
                                          text:@"Native Apple Silicon • Metal • 60 FPS target"
                                          font:[NSFont systemFontOfSize:15
                                                                 weight:NSFontWeightMedium]
                                         color:[NSColor secondaryLabelColor]];
  [content addSubview:subtitle];

  NSTextField* explanation = [self
      labelWithFrame:NSMakeRect(48, 260, 584, 72)
                text:@"Choose your local GoldenEye game backup. The app will verify it, import the "
                     @"required game data privately, and remember it for future launches."
                font:[NSFont systemFontOfSize:16]
               color:[NSColor labelColor]];
  [explanation setLineBreakMode:NSLineBreakByWordWrapping];
  [explanation setMaximumNumberOfLines:3];
  [content addSubview:explanation];

  choose_backup_ = [[[NSButton alloc] initWithFrame:NSMakeRect(48, 194, 285, 44)] autorelease];
  [choose_backup_ setTitle:@"Choose Backup ZIP or Package…"];
  [choose_backup_ setButtonType:NSButtonTypeMomentaryPushIn];
  [choose_backup_ setBezelStyle:NSBezelStyleRounded];
  [choose_backup_ setKeyEquivalent:@"\r"];
  [choose_backup_ setTarget:self];
  [choose_backup_ setAction:@selector(chooseBackup:)];
  [content addSubview:choose_backup_];

  choose_folder_ = [[[NSButton alloc] initWithFrame:NSMakeRect(347, 194, 285, 44)] autorelease];
  [choose_folder_ setTitle:@"Use Extracted Game Folder…"];
  [choose_folder_ setButtonType:NSButtonTypeMomentaryPushIn];
  [choose_folder_ setBezelStyle:NSBezelStyleRounded];
  [choose_folder_ setTarget:self];
  [choose_folder_ setAction:@selector(chooseFolder:)];
  [content addSubview:choose_folder_];

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
      labelWithFrame:NSMakeRect(48, 43, 500, 58)
                text:@"Game files are not included or downloaded. Select only a backup you are "
                     @"authorized to use. Hold Option while opening the app to choose again."
                font:[NSFont systemFontOfSize:12]
               color:[NSColor tertiaryLabelColor]];
  [notice setLineBreakMode:NSLineBreakByWordWrapping];
  [notice setMaximumNumberOfLines:3];
  [content addSubview:notice];

  quit_ = [[[NSButton alloc] initWithFrame:NSMakeRect(558, 55, 74, 32)] autorelease];
  [quit_ setTitle:@"Quit"];
  [quit_ setBezelStyle:NSBezelStyleRounded];
  [quit_ setTarget:self];
  [quit_ setAction:@selector(quit:)];
  [content addSubview:quit_];

  [window_ makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (void)setBusy:(BOOL)busy message:(NSString*)message {
  busy_.store(busy);
  [choose_backup_ setEnabled:!busy];
  [choose_folder_ setEnabled:!busy];
  [quit_ setEnabled:YES];
  [quit_ setTitle:busy ? @"Cancel" : @"Quit"];
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

- (void)finishWithRoot:(const std::filesystem::path&)root {
  paths_.game_data_root = root;
  WriteRememberedRoot(paths_.user_data_root, root);
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
          [controller finishWithRoot:root];
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
                  [self finishWithRoot:root];
                }];
}

- (void)quit:(id)sender {
  (void)sender;
  if (busy_.load()) {
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

  if (!force_setup && !paths.game_data_root.empty()) {
    if (std::filesystem::is_directory(paths.game_data_root)) {
      auto validation = game_data::ValidateDirectory(paths.game_data_root);
      if (validation.valid) {
        return paths;
      }
    } else {
      auto validation = game_data::ValidatePackage(paths.game_data_root);
      if (validation.valid) {
        return paths;
      }
    }
  }

  if (!force_setup) {
    auto cached = paths.user_data_root / "Game Data";
    if (auto remembered = ReadRememberedRoot(paths.user_data_root)) {
      auto validation = remembered->lexically_normal() == cached.lexically_normal()
                            ? game_data::ValidateImportedDirectory(*remembered)
                            : game_data::ValidateDirectory(*remembered);
      if (validation.valid) {
        paths.game_data_root = *remembered;
        return paths;
      }
    }
    if (game_data::ValidateImportedDirectory(cached).valid) {
      paths.game_data_root = cached;
      return paths;
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
