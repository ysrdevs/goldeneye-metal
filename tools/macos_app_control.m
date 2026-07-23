#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

static int Fail(NSString* message, int code) {
  fprintf(stderr, "macos-app-control: %s\n", message.UTF8String);
  return code;
}

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (argc != 3) {
      return Fail(@"usage: macos_app_control <window-id|terminate> <pid>", 2);
    }
    NSString* command = [NSString stringWithUTF8String:argv[1]];
    char* end = NULL;
    long parsed_pid = strtol(argv[2], &end, 10);
    if (!end || *end || parsed_pid <= 0 || parsed_pid > INT32_MAX) {
      return Fail(@"PID must be a positive integer", 2);
    }
    pid_t pid = (pid_t)parsed_pid;

    if ([command isEqualToString:@"terminate"]) {
      NSRunningApplication* application =
          [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
      if (!application) {
        return Fail([NSString stringWithFormat:@"no running application found for PID %d", pid],
                    3);
      }
      if (![application terminate]) {
        return Fail(
            [NSString stringWithFormat:@"AppKit rejected clean termination for PID %d", pid], 4);
      }
      return 0;
    }

    if ([command isEqualToString:@"window-id"]) {
      CFArrayRef raw_windows = CGWindowListCopyWindowInfo(
          kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
          kCGNullWindowID);
      if (!raw_windows) {
        return Fail(@"CoreGraphics did not return a window list", 3);
      }
      NSArray* windows = CFBridgingRelease(raw_windows);
      CGWindowID best_id = kCGNullWindowID;
      CGFloat best_area = 0;
      for (NSDictionary* window in windows) {
        NSNumber* owner_pid = window[(id)kCGWindowOwnerPID];
        NSNumber* layer = window[(id)kCGWindowLayer];
        NSNumber* number = window[(id)kCGWindowNumber];
        NSDictionary* bounds_dictionary = window[(id)kCGWindowBounds];
        CGRect bounds = CGRectZero;
        if (owner_pid.intValue != pid || layer.intValue != 0 || !number ||
            !bounds_dictionary ||
            !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)bounds_dictionary,
                                                     &bounds) ||
            bounds.size.width < 64 || bounds.size.height < 64) {
          continue;
        }
        CGFloat area = bounds.size.width * bounds.size.height;
        if (area > best_area) {
          best_area = area;
          best_id = number.unsignedIntValue;
        }
      }
      if (best_id == kCGNullWindowID) {
        return Fail(
            [NSString stringWithFormat:@"no visible top-level window found for PID %d", pid], 3);
      }
      printf("%u\n", best_id);
      return 0;
    }

    return Fail([NSString stringWithFormat:@"unknown command %@", command], 2);
  }
}
