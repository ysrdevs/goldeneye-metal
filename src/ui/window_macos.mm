#include <rex/ui/window.h>

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <rex/ui/surface_macos.h>

#include "macos_input_util.h"
#include "macos_key_translation.h"

namespace rex::ui {
class MacOSWindow;
}  // namespace rex::ui

@interface RexMacWindowDelegate : NSObject <NSWindowDelegate> {
 @private
  rex::ui::MacOSWindow* owner_;
}
- (instancetype)initWithOwner:(rex::ui::MacOSWindow*)owner;
@end

@interface RexMacContentView : NSView {
 @private
  rex::ui::MacOSWindow* owner_;
  BOOL shift_down_;
  BOOL control_down_;
  BOOL option_down_;
  BOOL caps_lock_enabled_;
  BOOL command_key_down_[128];
}
- (instancetype)initWithFrame:(NSRect)frame owner:(rex::ui::MacOSWindow*)owner;
- (void)detachOwner;
- (void)resetModifierState;
@end

namespace rex::ui {
namespace {

NSString* ToNSString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data()
                                  length:value.size()
                                encoding:NSUTF8StringEncoding];
}

struct MousePosition {
  int32_t x;
  int32_t y;
};

MousePosition GetMousePosition(NSView* view, NSEvent* event) {
  const NSPoint view_position = [view convertPoint:[event locationInWindow] fromView:nil];
  const NSPoint backing_position = [view convertPointToBacking:view_position];
  const NSRect backing_bounds = [view convertRectToBacking:[view bounds]];
  return {
      static_cast<int32_t>(std::lround(backing_position.x - NSMinX(backing_bounds))),
      static_cast<int32_t>(std::lround(NSMaxY(backing_bounds) - backing_position.y)),
  };
}

MouseEvent::Button GetMouseButton(NSEvent* event) {
  switch ([event buttonNumber]) {
    case 0:
      return MouseEvent::Button::kLeft;
    case 1:
      return MouseEvent::Button::kRight;
    case 2:
      return MouseEvent::Button::kMiddle;
    case 3:
      return MouseEvent::Button::kX1;
    case 4:
      return MouseEvent::Button::kX2;
    default:
      return MouseEvent::Button::kNone;
  }
}

}  // namespace

class MacOSWindow final : public Window {
 public:
  enum class MouseAction {
    kDown,
    kMove,
    kUp,
    kWheel,
  };

  MacOSWindow(WindowedAppContext& app_context, std::string_view title,
              uint32_t desired_logical_width, uint32_t desired_logical_height)
      : Window(app_context, title, desired_logical_width, desired_logical_height) {}

  ~MacOSWindow() override {
    EnterDestructor();
    ResetPlatformMouseStateFromUIThread();
    if (window_) {
      [(RexMacContentView*)[window_ contentView] detachOwner];
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
    if (!focused && window_) {
      RexMacContentView* content_view = (RexMacContentView*)[window_ contentView];
      [content_view resetModifierState];
      ResetPlatformMouseStateFromUIThread();
    }
    WindowDestructionReceiver destruction_receiver(this);
    OnFocusUpdate(focused, destruction_receiver);
    if (!destruction_receiver.IsWindowDestroyedOrClosed() && focused) {
      UpdatePlatformMouseStateFromUIThread();
    }
  }

  void HandleCloseRequest() { RequestClose(); }

  void HandleKey(NSEvent* event, bool down, bool emit_characters) {
    const NSEventModifierFlags modifiers = [event modifierFlags];
    const bool shift_pressed = (modifiers & NSEventModifierFlagShift) != 0;
    const bool ctrl_pressed = (modifiers & NSEventModifierFlagControl) != 0;
    const bool alt_pressed = (modifiers & NSEventModifierFlagOption) != 0;
    const bool super_pressed = (modifiers & NSEventModifierFlagCommand) != 0;

    const bool repeat_capable = down && emit_characters;
    const bool is_repeat = repeat_capable ? [event isARepeat] : false;
    KeyEvent key_event(this, macos::TranslateKeyCode([event keyCode]), 1,
                       macos::GetPreviousKeyState(down, repeat_capable, is_repeat), shift_pressed,
                       ctrl_pressed, alt_pressed, super_pressed);
    WindowDestructionReceiver destruction_receiver(this);
    if (down) {
      OnKeyDown(key_event, destruction_receiver);
    } else {
      OnKeyUp(key_event, destruction_receiver);
    }
    if (!down || !emit_characters || destruction_receiver.IsWindowDestroyedOrClosed()) {
      return;
    }

    NSString* characters = [event characters];
    const NSUInteger character_count = [characters length];
    for (NSUInteger i = 0; i < character_count; ++i) {
      const unichar character = [characters characterAtIndex:i];
      // Cocoa represents non-text keys in the private function-key Unicode
      // range. They have already been delivered as virtual-key events.
      if (character >= 0xF700 && character <= 0xF8FF) {
        continue;
      }
      KeyEvent char_event(this, VirtualKey(character), 1, false, shift_pressed, ctrl_pressed,
                          alt_pressed, super_pressed);
      OnKeyChar(char_event, destruction_receiver);
      if (destruction_receiver.IsWindowDestroyedOrClosed()) {
        return;
      }
    }
  }

  void HandleMouse(NSEvent* event, MouseAction action) {
    if (!window_ || [event window] != window_) {
      return;
    }

    RexMacContentView* content_view = (RexMacContentView*)[window_ contentView];
    const MousePosition position = GetMousePosition(content_view, event);
    WindowDestructionReceiver destruction_receiver(this);
    switch (action) {
      case MouseAction::kDown: {
        MouseEvent mouse_event(this, GetMouseButton(event), position.x, position.y);
        OnMouseDown(mouse_event, destruction_receiver);
      } break;
      case MouseAction::kMove: {
        // When Core Graphics disassociates the cursor from the mouse, AppKit
        // intentionally reports a constant absolute position while preserving
        // relative deltas. Convert the Y delta to the common top-down window
        // coordinate system used by MouseEvent.
        MouseEvent mouse_event(this, MouseEvent::Button::kNone, position.x, position.y, 0, 0,
                               {static_cast<int32_t>(std::lround([event deltaX])),
                                static_cast<int32_t>(std::lround(-[event deltaY]))});
        OnMouseMove(mouse_event, destruction_receiver);
      } break;
      case MouseAction::kUp: {
        MouseEvent mouse_event(this, GetMouseButton(event), position.x, position.y);
        OnMouseUp(mouse_event, destruction_receiver);
      } break;
      case MouseAction::kWheel: {
        if ([event phase] == NSEventPhaseCancelled) {
          return;
        }
        double scroll_x = [event scrollingDeltaX];
        double scroll_y = [event scrollingDeltaY];
        if ([event hasPreciseScrollingDeltas]) {
          // Match the normalization used by Dear ImGui's Cocoa backend: one
          // hundred precise points are one wheel detent.
          scroll_x *= 0.01;
          scroll_y *= 0.01;
        }
        MouseEvent mouse_event(
            this, MouseEvent::Button::kNone, position.x, position.y,
            static_cast<int32_t>(std::lround(scroll_x * MouseEvent::kScrollPerDetent)),
            static_cast<int32_t>(std::lround(scroll_y * MouseEvent::kScrollPerDetent)));
        OnMouseWheel(mouse_event, destruction_receiver);
      } break;
    }
  }

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

    RexMacContentView* content_view =
        [[RexMacContentView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height) owner:this];
    [content_view setWantsLayer:YES];

    metal_layer_ = [CAMetalLayer layer];
    [metal_layer_ retain];
    [metal_layer_ setDevice:device];
    [metal_layer_ setPixelFormat:MTLPixelFormatBGRA8Unorm];
    [metal_layer_ setFramebufferOnly:YES];
    [content_view setLayer:metal_layer_];
    [window_ setContentView:content_view];
    [window_ setAcceptsMouseMovedEvents:YES];
    [content_view release];
    [device release];

    delegate_ = [[RexMacWindowDelegate alloc] initWithOwner:this];
    [window_ setDelegate:delegate_];
    [window_ makeFirstResponder:content_view];
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    UpdateDrawableSize();

    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(SizeToPhysical(GetDesiredLogicalWidth()),
                       SizeToPhysical(GetDesiredLogicalHeight()), destruction_receiver);
    if (!destruction_receiver.IsWindowDestroyedOrClosed()) {
      OnFocusUpdate([window_ isKeyWindow], destruction_receiver);
    }
    if (!destruction_receiver.IsWindowDestroyedOrClosed()) {
      UpdatePlatformMouseStateFromUIThread();
    }
    return !destruction_receiver.IsWindowDestroyedOrClosed();
  }

  void RequestCloseImpl() override {
    WindowDestructionReceiver destruction_receiver(this);
    OnBeforeClose(destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return;
    }
    ResetPlatformMouseStateFromUIThread();
    if (window_) {
      [(RexMacContentView*)[window_ contentView] detachOwner];
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

  void ApplyNewMouseCapture() override { ApplyPlatformMouseState(); }

  void ApplyNewMouseRelease() override { ApplyPlatformMouseState(); }

  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override {
    (void)old_cursor_visibility;
    ApplyPlatformMouseState();
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

  void ApplyPlatformMouseState() {
    // MnK capture is driven by guest input polling rather than the AppKit
    // event loop. NSCursor is main-thread UI state, so perform the native
    // transition synchronously before returning from the common state setter.
    if (app_context().IsInUIThread()) {
      UpdatePlatformMouseStateFromUIThread();
      return;
    }
    app_context().CallInUIThreadSynchronous([this] { UpdatePlatformMouseStateFromUIThread(); });
  }

  bool WarpCursorToContentCenterFromUIThread() {
    assert_true(app_context().IsInUIThread());
    if (!window_) {
      return false;
    }
    NSView* content_view = [window_ contentView];
    NSScreen* screen = [window_ screen];
    NSNumber* screen_number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    if (!content_view || !screen || !screen_number) {
      return false;
    }

    const NSRect content_bounds = [content_view bounds];
    const NSPoint content_center = NSMakePoint(NSMidX(content_bounds), NSMidY(content_bounds));
    const NSPoint window_center = [content_view convertPoint:content_center toView:nil];
    const NSPoint screen_center = [window_ convertPointToScreen:window_center];
    const NSRect screen_frame = [screen frame];
    const CGDirectDisplayID display_id = CGDirectDisplayID([screen_number unsignedIntValue]);
    const CGRect display_bounds = CGDisplayBounds(display_id);
    const macos::CursorWarpPoint warp_point = macos::CalculateCursorWarpPoint(
        screen_center.x, screen_center.y, NSMinX(screen_frame), NSMinY(screen_frame),
        NSWidth(screen_frame), NSHeight(screen_frame), CGRectGetMinX(display_bounds),
        CGRectGetMinY(display_bounds), CGRectGetWidth(display_bounds),
        CGRectGetHeight(display_bounds));
    if (!warp_point.valid) {
      return false;
    }
    // Unlike posting a synthetic move, warping doesn't add an absolute-motion
    // event that could become a large first-frame camera delta.
    return CGWarpMouseCursorPosition(CGPointMake(warp_point.x, warp_point.y)) == kCGErrorSuccess;
  }

  void UpdatePlatformMouseStateFromUIThread() {
    assert_true(app_context().IsInUIThread());
    const bool focused = window_ && HasFocus();
    const bool mouse_should_be_disassociated =
        focused && IsMouseCaptureRequested() && GetCursorVisibility() == CursorVisibility::kHidden;

    if (mouse_should_be_disassociated != mouse_disassociated_) {
      bool transition_ready = !mouse_should_be_disassociated;
      if (mouse_should_be_disassociated) {
        // AppKit routes mouseMoved: to the window under the cursor. Put the
        // pointer inside this content view before freezing its absolute
        // position so relative motion keeps arriving even if capture was
        // requested while the pointer was over another window or display.
        transition_ready = WarpCursorToContentCenterFromUIThread();
      }
      if (transition_ready && CGAssociateMouseAndMouseCursorPosition(
                                  !mouse_should_be_disassociated) == kCGErrorSuccess) {
        mouse_disassociated_ = mouse_should_be_disassociated;
      }
    }
    // If hidden capture couldn't be established, leave the system cursor
    // visible and associated instead of consuming an unmatched global hide.
    const bool cursor_should_be_hidden = focused &&
                                         GetCursorVisibility() != CursorVisibility::kVisible &&
                                         (!mouse_should_be_disassociated || mouse_disassociated_);
    if (cursor_should_be_hidden != owns_cursor_hide_) {
      if (cursor_should_be_hidden) {
        [NSCursor hide];
      } else {
        [NSCursor unhide];
      }
      // NSCursor hide/unhide is globally counted. This flag represents exactly
      // one hide owned by this window so every path releases only its own call.
      owns_cursor_hide_ = cursor_should_be_hidden;
    }
  }

  void ResetPlatformMouseStateFromUIThread() {
    assert_true(app_context().IsInUIThread());
    if (mouse_disassociated_) {
      if (CGAssociateMouseAndMouseCursorPosition(true) == kCGErrorSuccess) {
        mouse_disassociated_ = false;
      }
    }
    if (owns_cursor_hide_) {
      [NSCursor unhide];
      owns_cursor_hide_ = false;
    }
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
  bool mouse_disassociated_ = false;
  bool owns_cursor_hide_ = false;
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

@implementation RexMacContentView

- (instancetype)initWithFrame:(NSRect)frame owner:(rex::ui::MacOSWindow*)owner {
  self = [super initWithFrame:frame];
  if (self) {
    owner_ = owner;
    [self resetModifierState];
  }
  return self;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)detachOwner {
  owner_ = nullptr;
}

- (void)resetModifierState {
  shift_down_ = NO;
  control_down_ = NO;
  option_down_ = NO;
  caps_lock_enabled_ = ([NSEvent modifierFlags] & NSEventModifierFlagCapsLock) != 0;
  memset(command_key_down_, 0, sizeof(command_key_down_));
}

- (void)keyDown:(NSEvent*)event {
  if (owner_) {
    owner_->HandleKey(event, true, true);
  } else {
    [super keyDown:event];
  }
}

- (void)keyUp:(NSEvent*)event {
  if (owner_) {
    owner_->HandleKey(event, false, false);
  } else {
    [super keyUp:event];
  }
}

- (void)flagsChanged:(NSEvent*)event {
  if (!owner_) {
    [super flagsChanged:event];
    return;
  }

  const rex::ui::VirtualKey virtual_key = rex::ui::macos::TranslateKeyCode([event keyCode]);
  const NSEventModifierFlags flags = [event modifierFlags];
  BOOL* state = nullptr;
  BOOL down = NO;
  switch (virtual_key) {
    case rex::ui::VirtualKey::kShift:
      state = &shift_down_;
      down = (flags & NSEventModifierFlagShift) != 0;
      break;
    case rex::ui::VirtualKey::kControl:
      state = &control_down_;
      down = (flags & NSEventModifierFlagControl) != 0;
      break;
    case rex::ui::VirtualKey::kMenu:
      state = &option_down_;
      down = (flags & NSEventModifierFlagOption) != 0;
      break;
    case rex::ui::VirtualKey::kCapital: {
      const rex::ui::macos::CapsLockTransition transition =
          rex::ui::macos::ResolveCapsLockTransition(caps_lock_enabled_ != NO,
                                                    (flags & NSEventModifierFlagCapsLock) != 0);
      caps_lock_enabled_ = transition.enabled ? YES : NO;
      if (transition.emit_key_down) {
        owner_->HandleKey(event, true, false);
      }
      // HandleKey listeners are allowed to close and destroy the window. The
      // teardown path detaches this view before releasing it.
      if (owner_ && transition.emit_key_up) {
        owner_->HandleKey(event, false, false);
      }
      return;
    }
    case rex::ui::VirtualKey::kLWin:
    case rex::ui::VirtualKey::kRWin: {
      const unsigned short key_code = [event keyCode];
      if (key_code >= sizeof(command_key_down_) / sizeof(command_key_down_[0])) {
        return;
      }
      state = &command_key_down_[key_code];
      down = *state ? NO : ((flags & NSEventModifierFlagCommand) != 0);
    } break;
    default:
      return;
  }

  if (*state == down) {
    return;
  }
  *state = down;
  owner_->HandleKey(event, down != NO, false);
}

- (void)mouseDown:(NSEvent*)event {
  if (owner_) {
    owner_->HandleMouse(event, rex::ui::MacOSWindow::MouseAction::kDown);
  } else {
    [super mouseDown:event];
  }
}

- (void)mouseUp:(NSEvent*)event {
  if (owner_) {
    owner_->HandleMouse(event, rex::ui::MacOSWindow::MouseAction::kUp);
  } else {
    [super mouseUp:event];
  }
}

- (void)rightMouseDown:(NSEvent*)event {
  if (owner_) {
    owner_->HandleMouse(event, rex::ui::MacOSWindow::MouseAction::kDown);
  } else {
    [super rightMouseDown:event];
  }
}

- (void)rightMouseUp:(NSEvent*)event {
  if (owner_) {
    owner_->HandleMouse(event, rex::ui::MacOSWindow::MouseAction::kUp);
  } else {
    [super rightMouseUp:event];
  }
}

- (void)otherMouseDown:(NSEvent*)event {
  if (owner_) {
    owner_->HandleMouse(event, rex::ui::MacOSWindow::MouseAction::kDown);
  } else {
    [super otherMouseDown:event];
  }
}

- (void)otherMouseUp:(NSEvent*)event {
  if (owner_) {
    owner_->HandleMouse(event, rex::ui::MacOSWindow::MouseAction::kUp);
  } else {
    [super otherMouseUp:event];
  }
}

- (void)mouseMoved:(NSEvent*)event {
  if (owner_) {
    owner_->HandleMouse(event, rex::ui::MacOSWindow::MouseAction::kMove);
  } else {
    [super mouseMoved:event];
  }
}

- (void)mouseDragged:(NSEvent*)event {
  [self mouseMoved:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
  [self mouseMoved:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
  [self mouseMoved:event];
}

- (void)scrollWheel:(NSEvent*)event {
  if (owner_) {
    owner_->HandleMouse(event, rex::ui::MacOSWindow::MouseAction::kWheel);
  } else {
    [super scrollWheel:event];
  }
}

@end
