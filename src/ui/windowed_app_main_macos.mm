/**
 ******************************************************************************
 * ReXGlue macOS windowed app entry point                                      *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>

namespace {

class MacOSWindowedAppContext final : public rex::ui::WindowedAppContext {
 public:
  MacOSWindowedAppContext() : poll_date_([[NSDate distantPast] retain]) {}

  ~MacOSWindowedAppContext() override {
    [poll_date_ release];
    poll_date_ = nil;
  }

  void RunMainLoop() {
    while (!HasQuitFromUIThread()) {
      @autoreleasepool {
        NSEvent* event = nil;
        do {
          event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                     untilDate:poll_date_
                                        inMode:NSDefaultRunLoopMode
                                       dequeue:YES];
          if (event) {
            [NSApp sendEvent:event];
          }
        } while (event);
        ExecutePendingFunctionsFromUIThread();
        [NSApp updateWindows];
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

 private:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}

  NSDate* poll_date_ = nil;
};

}  // namespace

@interface RexMacApplicationDelegate : NSObject <NSApplicationDelegate> {
 @private
  MacOSWindowedAppContext* app_context_;
}
- (instancetype)initWithAppContext:(MacOSWindowedAppContext*)appContext;
- (void)detachAppContext;
@end

namespace {

NSString* GetApplicationName() {
  NSBundle* bundle = [NSBundle mainBundle];
  NSString* name = [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
  if (![name length]) {
    name = [bundle objectForInfoDictionaryKey:@"CFBundleName"];
  }
  if (![name length]) {
    name = [[NSProcessInfo processInfo] processName];
  }
  return [name length] ? name : @"Application";
}

NSMenu* CreateMainMenu() {
  NSString* application_name = GetApplicationName();
  NSMenu* main_menu = [[NSMenu alloc] initWithTitle:@""];

  NSMenuItem* application_menu_item =
      [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
  NSMenu* application_menu = [[NSMenu alloc] initWithTitle:application_name];

  NSMenuItem* about_item =
      [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"About %@", application_name]
                                 action:@selector(orderFrontStandardAboutPanel:)
                          keyEquivalent:@""];
  [about_item setTarget:NSApp];
  [application_menu addItem:about_item];
  [about_item release];
  [application_menu addItem:[NSMenuItem separatorItem]];

  NSMenuItem* hide_item =
      [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Hide %@", application_name]
                                 action:@selector(hide:)
                          keyEquivalent:@"h"];
  [hide_item setTarget:NSApp];
  [application_menu addItem:hide_item];
  [hide_item release];

  NSMenuItem* hide_others_item =
      [[NSMenuItem alloc] initWithTitle:@"Hide Others"
                                 action:@selector(hideOtherApplications:)
                          keyEquivalent:@"h"];
  [hide_others_item
      setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
  [hide_others_item setTarget:NSApp];
  [application_menu addItem:hide_others_item];
  [hide_others_item release];

  NSMenuItem* show_all_item = [[NSMenuItem alloc] initWithTitle:@"Show All"
                                                        action:@selector(unhideAllApplications:)
                                                 keyEquivalent:@""];
  [show_all_item setTarget:NSApp];
  [application_menu addItem:show_all_item];
  [show_all_item release];
  [application_menu addItem:[NSMenuItem separatorItem]];

  NSMenuItem* quit_item =
      [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Quit %@", application_name]
                                 action:@selector(terminate:)
                          keyEquivalent:@"q"];
  [quit_item setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
  [quit_item setTarget:NSApp];
  [application_menu addItem:quit_item];
  [quit_item release];

  [application_menu_item setSubmenu:application_menu];
  [main_menu addItem:application_menu_item];
  [application_menu release];
  [application_menu_item release];

  NSMenuItem* window_menu_item =
      [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
  NSMenu* window_menu = [[NSMenu alloc] initWithTitle:@"Window"];

  NSMenuItem* close_item = [[NSMenuItem alloc] initWithTitle:@"Close"
                                                     action:@selector(performClose:)
                                              keyEquivalent:@"w"];
  [close_item setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
  [window_menu addItem:close_item];
  [close_item release];

  NSMenuItem* minimize_item = [[NSMenuItem alloc] initWithTitle:@"Minimize"
                                                        action:@selector(performMiniaturize:)
                                                 keyEquivalent:@"m"];
  [minimize_item setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
  [window_menu addItem:minimize_item];
  [minimize_item release];

  [window_menu_item setSubmenu:window_menu];
  [main_menu addItem:window_menu_item];
  [NSApp setWindowsMenu:window_menu];
  [window_menu release];
  [window_menu_item release];

  return main_menu;
}

NSWindow* GetTopLevelClosableWindow(NSWindow* window) {
  while ([window parentWindow]) {
    window = [window parentWindow];
  }
  return window && ([window styleMask] & NSWindowStyleMaskClosable) != 0 ? window : nil;
}

}  // namespace

@implementation RexMacApplicationDelegate

- (instancetype)initWithAppContext:(MacOSWindowedAppContext*)appContext {
  self = [super init];
  if (self) {
    app_context_ = appContext;
  }
  return self;
}

- (void)detachAppContext {
  app_context_ = nullptr;
}

- (void)requestCleanQuit {
  // Route every native quit request through the active window. The window
  // listener terminates the guest title before the C++ main loop begins
  // teardown; quitting the NSApplication directly would bypass that contract.
  NSWindow* window = GetTopLevelClosableWindow([NSApp mainWindow]);
  if (!window) {
    window = GetTopLevelClosableWindow([NSApp keyWindow]);
  }
  // mainWindow and keyWindow may both be nil while the app is hidden or
  // changing spaces. Keep the clean game-title termination path in that case.
  if (!window) {
    for (NSWindow* candidate in [NSApp windows]) {
      window = GetTopLevelClosableWindow(candidate);
      if (window) {
        break;
      }
    }
  }
  if (window) {
    [window performClose:self];
  } else if (app_context_) {
    app_context_->QuitFromUIThread();
  }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  (void)sender;
  [self requestCleanQuit];
  // The custom C++ event loop owns process lifetime and performs orderly
  // runtime destruction after the close callback marks the context as quit.
  return NSTerminateCancel;
}

@end

int main(int argc, char** argv) {
  @autoreleasepool {
    [NSApplication sharedApplication];

    auto remaining = rex::cvar::Init(argc, argv);
    rex::cvar::ApplyEnvironment();
    rex::InitLoggingEarly();

    int result = EXIT_FAILURE;

    {
      MacOSWindowedAppContext app_context;
      RexMacApplicationDelegate* application_delegate =
          [[RexMacApplicationDelegate alloc] initWithAppContext:&app_context];
      [NSApp setDelegate:application_delegate];
      NSMenu* main_menu = CreateMainMenu();
      [NSApp setMainMenu:main_menu];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
      // NSApplicationMain normally performs this step. This executable owns a
      // custom C++ event loop, so finish the AppKit launch explicitly to make
      // the menu bar, Dock commands, and Apple-event termination available.
      [NSApp finishLaunching];

      std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

      const auto& option_names = app->GetPositionalOptions();
      std::map<std::string, std::string> parsed;
      size_t count = std::min(remaining.size(), option_names.size());
      for (size_t i = 0; i < count; ++i) {
        parsed[option_names[i]] = remaining[i];
      }
      app->SetParsedArguments(std::move(parsed));

      if (app->OnInitialize()) {
        app_context.RunMainLoop();
        result = EXIT_SUCCESS;
      }

      if (app->RequiresImmediateProcessExit()) {
        // The accepted NSWindow close has already restored cursor and native
        // window state. An active guest on macOS uses asynchronous thread
        // cancellation, which may orphan non-robust mutexes and deadlock C++
        // teardown. End at the process boundary before invoking destructors;
        // macOS releases the remaining audio, Metal and thread resources.
        std::_Exit(result);
      }

      app->InvokeOnDestroy();

      // NSApplication doesn't own its delegate. Detach the raw C++ pointer
      // before the context leaves scope, then release all objects retained here.
      [application_delegate detachAppContext];
      [NSApp setDelegate:nil];
      [NSApp setWindowsMenu:nil];
      [NSApp setMainMenu:nil];
      [main_menu release];
      [application_delegate release];
    }

    rex::ShutdownLogging();
    return result;
  }
}
