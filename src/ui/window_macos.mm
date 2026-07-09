#include <rex/ui/window.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <memory>
#include <string>
#include <string_view>

#include <rex/ui/surface_macos.h>

namespace rex::ui {
class MacOSWindow;
}  // namespace rex::ui

@interface RexMacWindowDelegate : NSObject <NSWindowDelegate> {
 @private
  rex::ui::MacOSWindow* owner_;
}
- (instancetype)initWithOwner:(rex::ui::MacOSWindow*)owner;
@end

namespace rex::ui {
namespace {

NSString* ToNSString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data()
                                  length:value.size()
                                encoding:NSUTF8StringEncoding];
}

}  // namespace

class MacOSWindow final : public Window {
 public:
  MacOSWindow(WindowedAppContext& app_context, std::string_view title,
              uint32_t desired_logical_width, uint32_t desired_logical_height)
      : Window(app_context, title, desired_logical_width, desired_logical_height) {}

  ~MacOSWindow() override {
    EnterDestructor();
    if (window_) {
      [window_ setDelegate:nil];
      [window_ close];
      [window_ release];
      window_ = nil;
    }
    if (delegate_) {
      [delegate_ release];
      delegate_ = nil;
    }
  }

  void HandleResize() {
    if (!window_) {
      return;
    }
    UpdateDrawableSize();
    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(SizeToPhysical(GetDesiredLogicalWidth()),
                       SizeToPhysical(GetDesiredLogicalHeight()), destruction_receiver);
  }

  void HandleFocus(bool focused) {
    WindowDestructionReceiver destruction_receiver(this);
    OnFocusUpdate(focused, destruction_receiver);
  }

  void HandleCloseRequest() { RequestClose(); }

 private:
  bool OpenImpl() override {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      return false;
    }

    const CGFloat width = static_cast<CGFloat>(GetDesiredLogicalWidth());
    const CGFloat height = static_cast<CGFloat>(GetDesiredLogicalHeight());
    NSRect frame = NSMakeRect(100.0, 100.0, width, height);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    window_ = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    if (!window_) {
      [device release];
      return false;
    }

    NSString* title = ToNSString(GetTitle());
    [window_ setTitle:title];
    [title release];

    NSView* content_view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];
    [content_view setWantsLayer:YES];

    metal_layer_ = [CAMetalLayer layer];
    [metal_layer_ retain];
    [metal_layer_ setDevice:device];
    [metal_layer_ setPixelFormat:MTLPixelFormatBGRA8Unorm];
    [metal_layer_ setFramebufferOnly:YES];
    [content_view setLayer:metal_layer_];
    [window_ setContentView:content_view];
    [content_view release];
    [device release];

    delegate_ = [[RexMacWindowDelegate alloc] initWithOwner:this];
    [window_ setDelegate:delegate_];
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    UpdateDrawableSize();

    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(SizeToPhysical(GetDesiredLogicalWidth()),
                       SizeToPhysical(GetDesiredLogicalHeight()), destruction_receiver);
    if (!destruction_receiver.IsWindowDestroyedOrClosed()) {
      OnFocusUpdate([window_ isKeyWindow], destruction_receiver);
    }
    return !destruction_receiver.IsWindowDestroyedOrClosed();
  }

  void RequestCloseImpl() override {
    WindowDestructionReceiver destruction_receiver(this);
    OnBeforeClose(destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return;
    }
    if (window_) {
      [window_ setDelegate:nil];
      [window_ close];
      [window_ release];
      window_ = nil;
    }
    OnAfterClose();
  }

  void ApplyNewTitle() override {
    if (!window_) {
      return;
    }
    NSString* title = ToNSString(GetTitle());
    [window_ setTitle:title];
    [title release];
  }

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override {
    if (!(allowed_types & Surface::kTypeFlag_CAMetalLayer) || !metal_layer_) {
      return nullptr;
    }
    return std::make_unique<MacOSMetalLayerSurface>((void*)metal_layer_);
  }

  void RequestPaintImpl() override { OnPaint(true); }

  uint32_t GetLatestDpiImpl() const override {
    NSScreen* screen = window_ ? [window_ screen] : [NSScreen mainScreen];
    CGFloat scale = screen ? [screen backingScaleFactor] : 1.0;
    return static_cast<uint32_t>(96.0 * scale);
  }

  void UpdateDrawableSize() {
    if (!window_ || !metal_layer_) {
      return;
    }
    NSView* content_view = [window_ contentView];
    CGSize size = [content_view bounds].size;
    CGFloat scale = [[window_ screen] backingScaleFactor];
    [metal_layer_ setContentsScale:scale];
    [metal_layer_ setDrawableSize:CGSizeMake(size.width * scale, size.height * scale)];
  }

  NSWindow* window_ = nil;
  CAMetalLayer* metal_layer_ = nil;
  RexMacWindowDelegate* delegate_ = nil;
};

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context, std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<MacOSWindow>(app_context, title, desired_logical_width,
                                       desired_logical_height);
}

}  // namespace rex::ui

@implementation RexMacWindowDelegate

- (instancetype)initWithOwner:(rex::ui::MacOSWindow*)owner {
  self = [super init];
  if (self) {
    owner_ = owner;
  }
  return self;
}

- (void)windowDidResize:(NSNotification*)notification {
  (void)notification;
  if (owner_) {
    owner_->HandleResize();
  }
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
  (void)notification;
  if (owner_) {
    owner_->HandleFocus(true);
  }
}

- (void)windowDidResignKey:(NSNotification*)notification {
  (void)notification;
  if (owner_) {
    owner_->HandleFocus(false);
  }
}

- (BOOL)windowShouldClose:(id)sender {
  (void)sender;
  if (owner_) {
    owner_->HandleCloseRequest();
  }
  return NO;
}

@end
