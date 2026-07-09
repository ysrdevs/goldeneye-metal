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

int main(int argc, char** argv) {
  [NSApplication sharedApplication];

  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result = EXIT_FAILURE;

  {
    MacOSWindowedAppContext app_context;

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

    app->InvokeOnDestroy();
  }

  rex::ShutdownLogging();
  return result;
}
