// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "mbvip/core/mb.h"

extern "C" void mbMacSetComposition(mbWebView web_view,
                                    const char16_t* text,
                                    size_t length,
                                    BOOL committed);
extern "C" void mbMacSetDeviceScaleFactor(mbWebView web_view, float scale);


static std::u16string UTF16String(NSString* value) {
  std::u16string result([value length], u'\0');
  if (!result.empty()) {
    [value getCharacters:reinterpret_cast<unichar*>(result.data())
                   range:NSMakeRange(0, [value length])];
  }
  return result;
}

@class MiniBlinkView;

static void MB_CALL_TYPE PaintUpdated(mbWebView web_view,
                                      void* parameter,
                                      const void* buffer,
                                      const mbRect* dirty_rect,
                                      int width,
                                      int height);
static void MB_CALL_TYPE LoadingFinished(mbWebView web_view,
                                         void* parameter,
                                         mbWebFrameHandle frame,
                                         const utf8* url,
                                         mbLoadingResult result,
                                         const utf8* failed_reason);

@interface MiniBlinkView : NSView <NSTextInputClient>
- (instancetype)initWithFrame:(NSRect)frame webView:(mbWebView)webView;
- (void)acceptPixels:(NSData*)pixels width:(int)width height:(int)height;
- (void)pageDidFinishLoading:(NSString*)url;
- (BOOL)hasFrame;
- (void)exerciseInputPath;
- (NSData*)snapshotPNG;
@end

@interface DemoAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithFrontendURL:(NSURL*)frontendURL
                     screenshotPath:(NSString*)screenshotPath;
- (void)blinkViewDidLoad:(MiniBlinkView*)view url:(NSString*)url;
@end

@implementation MiniBlinkView {
  mbWebView _webView;
  NSData* _pixels;
  int _pixelWidth;
  int _pixelHeight;
  NSMutableAttributedString* _markedText;
}

- (instancetype)initWithFrame:(NSRect)frame webView:(mbWebView)webView {
  self = [super initWithFrame:frame];
  if (self) {
    _webView = webView;
    _markedText = [[NSMutableAttributedString alloc] init];
    [self setWantsLayer:YES];
    mbOnPaintBitUpdated(_webView, PaintUpdated, (__bridge void*)self);
    mbOnLoadingFinish(_webView, LoadingFinished, (__bridge void*)self);
    [self syncRendererMetrics];
  }
  return self;
}

- (void)dealloc {
  if (_webView != NULL_WEBVIEW)
    mbDestroyWebView(_webView);
#if !__has_feature(objc_arc)
  [_pixels release];
  [_markedText release];
  [super dealloc];
#endif
}

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)isOpaque {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)becomeFirstResponder {
  mbSetFocus(_webView);
  return YES;
}

- (BOOL)resignFirstResponder {
  mbKillFocus(_webView);
  return YES;
}

- (float)currentDeviceScaleFactor {
  NSWindow* window = [self window];
  if (window)
    return static_cast<float>([window backingScaleFactor]);
  NSScreen* screen = [NSScreen mainScreen];
  return screen ? static_cast<float>([screen backingScaleFactor]) : 1.f;
}

- (void)syncRendererMetrics {
  if (_webView == NULL_WEBVIEW)
    return;
  const NSSize size = self.bounds.size;
  const float scale = [self currentDeviceScaleFactor];
  std::fprintf(stderr, "[mb] NSView dip=%.0fx%.0f scale=%.2f\n",
               size.width, size.height, scale);
  mbMacSetDeviceScaleFactor(_webView, scale);
  mbResize(_webView, std::max(1, static_cast<int>(size.width)),
           std::max(1, static_cast<int>(size.height)));
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  [self syncRendererMetrics];
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  [self syncRendererMetrics];
}

- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  [self syncRendererMetrics];
}

- (void)acceptPixels:(NSData*)pixels width:(int)width height:(int)height {
#if !__has_feature(objc_arc)
  [_pixels release];
#endif
  _pixels = [pixels copy];
  _pixelWidth = width;
  _pixelHeight = height;
  [self setNeedsDisplay:YES];
}

- (BOOL)hasFrame {
  return _pixels != nil && _pixelWidth > 0 && _pixelHeight > 0;
}
- (NSData*)snapshotPNG {
  if (!_pixels)
    return nil;
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(
      (__bridge CFDataRef)_pixels);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  CGImageRef image = CGImageCreate(
      _pixelWidth, _pixelHeight, 8, 32, _pixelWidth * 4, colorSpace,
      kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast,
      provider, nullptr, false, kCGRenderingIntentDefault);
  NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
  NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                     properties:@{}];
  CGImageRelease(image);
  CGColorSpaceRelease(colorSpace);
  CGDataProviderRelease(provider);
  return png;
}

- (void)exerciseInputPath {
  std::fputs("[mb] AppKit input -> resize/mouse/keyboard/focus/IME\n", stderr);
  mbSetFocus(_webView);
  mbFireMouseEvent(_webView, MB_MSG_MOUSEMOVE, 30, 30, 0);
  mbFireMouseEvent(_webView, MB_MSG_LBUTTONDOWN, 30, 30, MB_LBUTTON);
  mbFireMouseEvent(_webView, MB_MSG_LBUTTONUP, 30, 30, 0);
  NSEvent* keyDown =
      [NSEvent keyEventWithType:NSEventTypeKeyDown
                       location:NSZeroPoint
                  modifierFlags:NSEventModifierFlagCommand
                      timestamp:0
                   windowNumber:[[self window] windowNumber]
                        context:nil
                     characters:@"v"
    charactersIgnoringModifiers:@"v"
                      isARepeat:NO
                        keyCode:9];
  NSEvent* keyUp =
      [NSEvent keyEventWithType:NSEventTypeKeyUp
                       location:NSZeroPoint
                  modifierFlags:NSEventModifierFlagCommand
                      timestamp:0
                   windowNumber:[[self window] windowNumber]
                        context:nil
                     characters:@"v"
    charactersIgnoringModifiers:@"v"
                      isARepeat:NO
                        keyCode:9];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC),
                 dispatch_get_main_queue(), ^{
    const NSSize size = self.bounds.size;
    const int x = static_cast<int>(size.width * 0.5);
    const int y = static_cast<int>(size.height * 0.64);
    mbFireMouseEvent(self->_webView, MB_MSG_LBUTTONDOWN, x, y, MB_LBUTTON);
    mbFireMouseEvent(self->_webView, MB_MSG_LBUTTONUP, x, y, 0);
  });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 11 * NSEC_PER_SEC),
                 dispatch_get_main_queue(), ^{
    const NSSize size = self.bounds.size;
    const int x = static_cast<int>(size.width * 0.5);
    const int y = static_cast<int>(size.height * 0.55);
    mbFireMouseEvent(self->_webView, MB_MSG_LBUTTONDOWN, x, y, MB_LBUTTON);
    mbFireMouseEvent(self->_webView, MB_MSG_LBUTTONUP, x, y, 0);
  });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC),
                 dispatch_get_main_queue(), ^{
    [NSApp postEvent:keyDown atStart:NO];
    [NSApp postEvent:keyUp atStart:NO];
  });
}

- (void)drawRect:(NSRect)dirtyRect {
  [[NSColor colorWithRed:0.047 green:0.067 blue:0.106 alpha:1.0] setFill];
  NSRectFill(dirtyRect);
  if (!_pixels)
    return;

  CGDataProviderRef provider = CGDataProviderCreateWithCFData(
      (__bridge CFDataRef)_pixels);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  CGImageRef image = CGImageCreate(
      _pixelWidth, _pixelHeight, 8, 32, _pixelWidth * 4, colorSpace,
      kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast,
      provider, nullptr, false, kCGRenderingIntentDefault);
  CGContextRef context = [[NSGraphicsContext currentContext] CGContext];
  CGContextSaveGState(context);
  CGContextTranslateCTM(context, 0, self.bounds.size.height);
  CGContextScaleCTM(context, 1, -1);
  CGContextSetInterpolationQuality(context, kCGInterpolationNone);
  CGContextDrawImage(context,
                     CGRectMake(0, 0, self.bounds.size.width,
                                self.bounds.size.height),
                     image);
  CGContextRestoreGState(context);
  CGImageRelease(image);
  CGColorSpaceRelease(colorSpace);
  CGDataProviderRelease(provider);
}

- (unsigned int)mouseFlagsForEvent:(NSEvent*)event {
  unsigned int flags = 0;
  NSEventModifierFlags modifiers = [event modifierFlags];
  if (modifiers & NSEventModifierFlagShift)
    flags |= MB_SHIFT;
  if (modifiers & (NSEventModifierFlagControl | NSEventModifierFlagCommand))
    flags |= MB_CONTROL;
  NSUInteger buttons = [NSEvent pressedMouseButtons];
  if (buttons & 1)
    flags |= MB_LBUTTON;
  if (buttons & 2)
    flags |= MB_RBUTTON;
  if (buttons & 4)
    flags |= MB_MBUTTON;
  return flags;
}

- (void)sendMouse:(NSEvent*)event message:(unsigned int)message {
  NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
  mbFireMouseEvent(_webView, message, static_cast<int>(point.x),
                   static_cast<int>(point.y), [self mouseFlagsForEvent:event]);
}

- (void)mouseMoved:(NSEvent*)event {
  [self sendMouse:event message:MB_MSG_MOUSEMOVE];
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
- (void)mouseDown:(NSEvent*)event {
  [[self window] makeFirstResponder:self];
  [self sendMouse:event message:MB_MSG_LBUTTONDOWN];
}
- (void)mouseUp:(NSEvent*)event {
  [self sendMouse:event message:MB_MSG_LBUTTONUP];
}
- (void)rightMouseDown:(NSEvent*)event {
  [self sendMouse:event message:MB_MSG_RBUTTONDOWN];
}
- (void)rightMouseUp:(NSEvent*)event {
  [self sendMouse:event message:MB_MSG_RBUTTONUP];
}
- (void)otherMouseDown:(NSEvent*)event {
  [self sendMouse:event message:MB_MSG_MBUTTONDOWN];
}
- (void)otherMouseUp:(NSEvent*)event {
  [self sendMouse:event message:MB_MSG_MBUTTONUP];
}
- (void)scrollWheel:(NSEvent*)event {
  NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
  int delta = static_cast<int>([event scrollingDeltaY] * 120.0);
  mbFireMouseWheelEvent(_webView, static_cast<int>(point.x),
                        static_cast<int>(point.y), delta,
                        [self mouseFlagsForEvent:event]);
}

- (unsigned int)keyForEvent:(NSEvent*)event {
  NSString* characters = [event charactersIgnoringModifiers];
  if ([characters length] == 0)
    return 0;
  unichar character = [characters characterAtIndex:0];
  if (character >= 'a' && character <= 'z')
    character -= 'a' - 'A';
  return character;
}

- (void)keyDown:(NSEvent*)event {
  if (([event modifierFlags] & NSEventModifierFlagCommand) != 0)
    return;
  unsigned int key = [self keyForEvent:event];
  mbFireKeyDownEvent(_webView, key, 0, FALSE);
  [self interpretKeyEvents:@[ event ]];
}

- (void)keyUp:(NSEvent*)event {
  if (([event modifierFlags] & NSEventModifierFlagCommand) != 0)
    return;
  mbFireKeyUpEvent(_webView, [self keyForEvent:event], 0, FALSE);
}

- (void)insertCommittedText:(NSString*)plain {
  if ([plain length] == 0)
    return;
  mbSetFocus(_webView);
  std::u16string characters = UTF16String(plain);
  mbMacSetComposition(_webView, characters.data(), characters.size(), TRUE);
  [_markedText deleteCharactersInRange:NSMakeRange(0, [_markedText length])];
}

- (void)paste:(id)sender {
  NSString* value =
      [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
  if (![value length]) {
    NSArray* objects = [[NSPasteboard generalPasteboard]
        readObjectsForClasses:@[ [NSString class] ]
                      options:@{}];
    if ([objects count])
      value = objects[0];
  }
  if (![value length])
    return;
  [self insertCommittedText:value];
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
  NSString* plain = [string isKindOfClass:[NSAttributedString class]]
                        ? [string string]
                        : string;
  [self insertCommittedText:plain];
}

- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
      replacementRange:(NSRange)replacementRange {
  NSAttributedString* attributed =
      [string isKindOfClass:[NSAttributedString class]]
          ? string
          : [[NSAttributedString alloc] initWithString:string];
  [_markedText setAttributedString:attributed];
  NSString* plain = [attributed string];
  std::u16string characters = UTF16String(plain);
  mbMacSetComposition(_webView, characters.data(), characters.size(), FALSE);
#if !__has_feature(objc_arc)
  if (![string isKindOfClass:[NSAttributedString class]])
    [attributed release];
#endif
}

- (void)unmarkText {
  [_markedText deleteCharactersInRange:NSMakeRange(0, [_markedText length])];
  mbMacSetComposition(_webView, nullptr, 0, FALSE);
}

- (BOOL)hasMarkedText {
  return [_markedText length] != 0;
}
- (NSRange)markedRange {
  return [self hasMarkedText] ? NSMakeRange(0, [_markedText length])
                              : NSMakeRange(NSNotFound, 0);
}
- (NSRange)selectedRange {
  return NSMakeRange(NSNotFound, 0);
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
  return @[];
}
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range
                                               actualRange:(NSRangePointer)actualRange {
  return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
  return NSNotFound;
}
- (NSRect)firstRectForCharacterRange:(NSRange)range
                         actualRange:(NSRangePointer)actualRange {
  if (actualRange)
    *actualRange = range;
  mbRect caret{};
  mbGetCaretRect(_webView, &caret);
  NSRect local = NSMakeRect(caret.x, caret.y, std::max(1, caret.w),
                            std::max(1, caret.h));
  return [[self window] convertRectToScreen:[self convertRect:local toView:nil]];
}
- (void)doCommandBySelector:(SEL)selector {
  if (selector == @selector(paste:) ||
      selector == @selector(pasteAsPlainText:) ||
      selector == @selector(pasteAsRichText:)) {
    [self paste:nil];
  }
}

- (void)pageDidFinishLoading:(NSString*)url {
  DemoAppDelegate* delegate = (DemoAppDelegate*)[NSApp delegate];
  [delegate blinkViewDidLoad:self url:url];
}

@end

static void MB_CALL_TYPE PaintUpdated(mbWebView web_view,
                                      void* parameter,
                                      const void* buffer,
                                      const mbRect* dirty_rect,
                                      int width,
                                      int height) {
  if (!buffer || width <= 0 || height <= 0)
    return;
  const size_t byteCount = static_cast<size_t>(width) * height * 4;
  NSData* pixels = [[NSData alloc] initWithBytes:buffer length:byteCount];
  MiniBlinkView* view = (__bridge MiniBlinkView*)parameter;
  dispatch_async(dispatch_get_main_queue(), ^{
    [view acceptPixels:pixels width:width height:height];
  });
#if !__has_feature(objc_arc)
  [pixels release];
#endif
}

static void MB_CALL_TYPE LoadingFinished(mbWebView web_view,
                                         void* parameter,
                                         mbWebFrameHandle frame,
                                         const utf8* url,
                                         mbLoadingResult result,
                                         const utf8* failed_reason) {
  if (result != MB_LOADING_SUCCEEDED) {
    std::fprintf(stderr, "GUI_ERROR navigation failed: %s\n",
                 failed_reason ? failed_reason : "unknown");
    std::fflush(stderr);
    return;
  }
  MiniBlinkView* view = (__bridge MiniBlinkView*)parameter;
  NSString* loadedURL = url ? [NSString stringWithUTF8String:url] : @"";
  dispatch_async(dispatch_get_main_queue(), ^{
    [view pageDidFinishLoading:loadedURL];
  });
}

@implementation DemoAppDelegate {
  NSURL* _frontendURL;
  NSString* _screenshotPath;
  NSWindow* _window;
  MiniBlinkView* _blinkView;
  BOOL _captureScheduled;
}

- (instancetype)initWithFrontendURL:(NSURL*)frontendURL
                     screenshotPath:(NSString*)screenshotPath {
  self = [super init];
  if (self) {
    _frontendURL = [frontendURL copy];
    _screenshotPath = [screenshotPath copy];
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  NSMenu* menuBar = [[NSMenu alloc] init];
  NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
  [menuBar addItem:appMenuItem];
  NSMenu* appMenu = [[NSMenu alloc] init];
  [appMenu addItemWithTitle:@"退出 miniblink132 GUI Demo"
                     action:@selector(terminate:)
              keyEquivalent:@"q"];
  [appMenuItem setSubmenu:appMenu];
  NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
  [menuBar addItem:editMenuItem];
  NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
  [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [editMenuItem setSubmenu:editMenu];
  [NSApp setMainMenu:menuBar];

  NSRect frame = NSMakeRect(0, 0, 960, 640);
  NSWindowStyleMask style = NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable;
  _window = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  [_window setTitle:@"miniblink132 macOS Blink Host"];
  [_window setContentMinSize:NSMakeSize(720, 480)];
  [_window setDelegate:self];
  [_window setAcceptsMouseMovedEvents:YES];

  mbWebView webView = mbCreateWebView();
  _blinkView = [[MiniBlinkView alloc] initWithFrame:[[_window contentView] bounds]
                                            webView:webView];
  [_blinkView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [[_window contentView] addSubview:_blinkView];

  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [_window makeFirstResponder:_blinkView];
  [NSApp activateIgnoringOtherApps:YES];
  mbLoadURL(webView, [[_frontendURL absoluteString] UTF8String]);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}

- (void)windowWillClose:(NSNotification*)notification {
  [NSApp terminate:nil];
}

- (void)blinkViewDidLoad:(MiniBlinkView*)view url:(NSString*)url {
  if (_captureScheduled)
    return;
  _captureScheduled = YES;
  [view exerciseInputPath];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                 dispatch_get_main_queue(), ^{
    if (![view hasFrame]) {
      self->_captureScheduled = NO;
      [self blinkViewDidLoad:view url:url];
      return;
    }
    NSData* png = [view snapshotPNG];
    if (![png writeToFile:self->_screenshotPath atomically:YES]) {
      std::fputs("GUI_ERROR cannot write Blink snapshot\n", stderr);
      return;
    }
    std::printf("GUI_READY engine=MiniBlink/Blink/V8 window=%ld url=%s screenshot=%s\n",
                (long)[self->_window windowNumber], [url UTF8String],
                [self->_screenshotPath UTF8String]);
    std::fflush(stdout);
  });
}

@end

int main(int argc, char** argv) {
  @autoreleasepool {
    const char* frontendSource =
        argc >= 2 ? argv[1] : "platform/macos/resources/demo/index.html";
    const char* screenshotPath =
        argc >= 3 ? argv[2] : "/tmp/miniblink132-gui-demo.png";
    NSString* source = [NSString stringWithUTF8String:frontendSource];
    NSURL* frontendURL = nil;
    if ([source hasPrefix:@"https://"] || [source hasPrefix:@"http://"]) {
      frontendURL = [NSURL URLWithString:source];
    } else {
      NSString* absolute = [[NSURL fileURLWithPath:source]
          URLByStandardizingPath].path;
      if (![[NSFileManager defaultManager] fileExistsAtPath:absolute]) {
        std::fprintf(stderr, "GUI_ERROR frontend not found: %s\n", frontendSource);
        return 2;
      }
      frontendURL = [NSURL fileURLWithPath:absolute];
    }
    if (!frontendURL) {
      std::fprintf(stderr, "GUI_ERROR invalid URL: %s\n", frontendSource);
      return 2;
    }

    NSApplication* application = [NSApplication sharedApplication];
    mbInit(nullptr);
    __attribute__((objc_precise_lifetime)) DemoAppDelegate* delegate =
        [[DemoAppDelegate alloc]
            initWithFrontendURL:frontendURL
                 screenshotPath:[NSString stringWithUTF8String:screenshotPath]];
    [application setDelegate:delegate];
    [application run];
    mbUninit();
  }
  return 0;
}
