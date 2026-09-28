// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "mbvip/core/mb.h"
#include "platform/macos/electron/electron_api.h"
#include "platform/macos/electron/omp_desktop_runtime.h"

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

static NSString* PasteboardPlainText() {
  NSPasteboard* board = [NSPasteboard generalPasteboard];
  NSString* value = [board stringForType:NSPasteboardTypeString];
  if ([value length])
    return value;
  for (NSPasteboardItem* item in [board pasteboardItems]) {
    NSString* text = [item stringForType:NSPasteboardTypeString];
    if ([text length])
      return text;
    text = [item stringForType:@"public.utf8-plain-text"];
    if ([text length])
      return text;
  }
  return nil;
}

@class ElectronBlinkView;
@class ElectronAppDelegate;

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

@interface ElectronBlinkView : NSView <NSTextInputClient>
- (instancetype)initWithFrame:(NSRect)frame
                      webView:(mbWebView)webView
                       owner:(ElectronAppDelegate*)owner;
- (void)acceptPixels:(NSData*)pixels width:(int)width height:(int)height;
- (void)didFinishLoading:(NSString*)url;
- (BOOL)hasFrame;
- (NSData*)snapshotPNG;
- (void)exerciseTypingAndHover;
@end

@interface ElectronAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithPlan:(miniblink::electron::AppPlan)plan
              screenshotPath:(NSString*)screenshotPath
                 interactive:(BOOL)interactive
               exerciseInput:(BOOL)exerciseInput
                     runtime:(OmpDesktopRuntime*)runtime;
- (void)viewDidFinishLoading:(ElectronBlinkView*)view url:(NSString*)url;
@end

@implementation ElectronBlinkView {
  mbWebView _webView;
  ElectronAppDelegate* _owner;
  NSData* _pixels;
  int _pixelWidth;
  int _pixelHeight;
  NSMutableAttributedString* _markedText;
}

- (instancetype)initWithFrame:(NSRect)frame
                      webView:(mbWebView)webView
                       owner:(ElectronAppDelegate*)owner {
  self = [super initWithFrame:frame];
  if (self) {
    _webView = webView;
    _owner = owner;
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

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
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

- (void)didFinishLoading:(NSString*)url {
  [_owner viewDidFinishLoading:self url:url];
}

- (BOOL)hasFrame {
  return _pixels != nil && _pixelWidth > 0 && _pixelHeight > 0;
}

- (NSData*)snapshotPNG {
  if (!_pixels)
    return nil;
  CGDataProviderRef provider =
      CGDataProviderCreateWithCFData((__bridge CFDataRef)_pixels);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  CGImageRef image = CGImageCreate(
      _pixelWidth, _pixelHeight, 8, 32, _pixelWidth * 4, colorSpace,
      kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast, provider,
      nullptr, false, kCGRenderingIntentDefault);
  NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
  NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                     properties:@{}];
  CGImageRelease(image);
  CGColorSpaceRelease(colorSpace);
  CGDataProviderRelease(provider);
#if !__has_feature(objc_arc)
  [bitmap autorelease];
#endif
  return png;
}

- (void)drawRect:(NSRect)dirtyRect {
  [[NSColor colorWithRed:0.047 green:0.067 blue:0.106 alpha:1.0] setFill];
  NSRectFill(dirtyRect);
  if (!_pixels)
    return;

  CGDataProviderRef provider =
      CGDataProviderCreateWithCFData((__bridge CFDataRef)_pixels);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  CGImageRef image = CGImageCreate(
      _pixelWidth, _pixelHeight, 8, 32, _pixelWidth * 4, colorSpace,
      kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast, provider,
      nullptr, false, kCGRenderingIntentDefault);
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
  [[self window] makeFirstResponder:self];
  [self sendMouse:event message:MB_MSG_RBUTTONDOWN];
}

- (void)rightMouseUp:(NSEvent*)event {
  [self sendMouse:event message:MB_MSG_RBUTTONUP];
}

- (void)otherMouseDown:(NSEvent*)event {
  [[self window] makeFirstResponder:self];
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
  mbFireKeyDownEvent(_webView, [self keyForEvent:event], 0, FALSE);
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
  NSString* value = PasteboardPlainText();
  if (![value length]) {
    NSArray* objects = [[NSPasteboard generalPasteboard]
        readObjectsForClasses:@[ [NSString class] ]
                      options:@{}];
    if ([objects count])
      value = objects[0];
  }
  if (![value length]) {
    std::fputs("[electron-demo] paste: clipboard empty\n", stderr);
    return;
  }
  std::fprintf(stderr, "[electron-demo] paste: inserting %lu chars\n",
               (unsigned long)[value length]);
  [self insertCommittedText:value];
}

- (void)insertText:(id)string {
  [self insertText:string replacementRange:NSMakeRange(NSNotFound, 0)];
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
    return;
  }
  if (selector == @selector(insertNewline:)) {
    [self insertCommittedText:@"\n"];
    return;
  }
  if (selector == @selector(deleteBackward:)) {
    mbFireKeyDownEvent(_webView, 0x08, 0, FALSE);
    mbFireKeyUpEvent(_webView, 0x08, 0, FALSE);
  }
}

- (void)exerciseTypingAndHover {
  const NSSize size = self.bounds.size;
  const int x = std::max(1, static_cast<int>(size.width * 0.5));
  const int y = std::max(1, static_cast<int>(size.height * 0.5));
  mbSetFocus(_webView);
  mbFireMouseEvent(_webView, MB_MSG_MOUSEMOVE, x, y, 0);
  mbFireMouseEvent(_webView, MB_MSG_LBUTTONDOWN, x, y, MB_LBUTTON);
  mbFireMouseEvent(_webView, MB_MSG_LBUTTONUP, x, y, 0);
  [self insertCommittedText:@"x"];
  NSPasteboard* board = [NSPasteboard generalPasteboard];
  NSString* previous = [board stringForType:NSPasteboardTypeString];
  [board clearContents];
  [board declareTypes:@[ NSPasteboardTypeString ] owner:nil];
  [board setString:@"PASTE_OK" forType:NSPasteboardTypeString];
  [self paste:nil];
  [board clearContents];
  [board declareTypes:@[ NSPasteboardTypeString ] owner:nil];
  if (previous)
    [board setString:previous forType:NSPasteboardTypeString];
  mbFireMouseEvent(_webView, MB_MSG_MOUSEMOVE, x + 1, y + 1, 0);
}
@end

@implementation ElectronAppDelegate {
  miniblink::electron::AppPlan _plan;
  NSString* _screenshotPath;
  BOOL _interactive;
  BOOL _exerciseInput;
  NSMutableArray<NSWindow*>* _windows;
  BOOL _verificationFinished;
  OmpDesktopRuntime* _runtime;
}

- (instancetype)initWithPlan:(miniblink::electron::AppPlan)plan
              screenshotPath:(NSString*)screenshotPath
                 interactive:(BOOL)interactive
               exerciseInput:(BOOL)exerciseInput
                     runtime:(OmpDesktopRuntime*)runtime {
  self = [super init];
  if (self) {
    _plan = std::move(plan);
    _screenshotPath = [screenshotPath copy];
    _interactive = interactive;
    _exerciseInput = exerciseInput;
#if !__has_feature(objc_arc)
    _runtime = [runtime retain];
#else
    _runtime = runtime;
#endif
    _windows = [[NSMutableArray alloc] init];
  }
  return self;
}

- (void)dealloc {
#if !__has_feature(objc_arc)
  [_screenshotPath release];
  [_windows release];
  [_runtime release];
  [super dealloc];
#endif
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  NSMenu* menuBar = [[NSMenu alloc] init];
  NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
  [menuBar addItem:appMenuItem];
  NSMenu* appMenu = [[NSMenu alloc] init];
  [appMenu addItemWithTitle:_runtime ? @"Quit OMP Desktop"
                                 : @"Quit Electron Demo"
                     action:@selector(terminate:)
              keyEquivalent:@"q"];
  [appMenuItem setSubmenu:appMenu];
  NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
  [menuBar addItem:editMenuItem];
  NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
  [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
  [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
  [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [editMenu addItemWithTitle:@"Select All"
                     action:@selector(selectAll:)
              keyEquivalent:@"a"];
  [editMenuItem setSubmenu:editMenu];
  [NSApp setMainMenu:menuBar];

  if (_plan.quit_requested || _plan.windows.empty()) {
    std::fputs("ELECTRON_DEMO_ERROR main script created no windows\n", stderr);
    [NSApp terminate:nil];
    return;
  }

  for (const auto& options : _plan.windows) {
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                              NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable;
    if (options.resizable)
      style |= NSWindowStyleMaskResizable;
    if (_runtime)
      style |= NSWindowStyleMaskFullSizeContentView;
    NSRect frame = NSMakeRect(0, 0, options.width, options.height);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    [window setTitle:[NSString stringWithUTF8String:options.title.c_str()]];
    if (_runtime) {
      [window setTitleVisibility:NSWindowTitleHidden];
      [window setTitlebarAppearsTransparent:YES];
      [window setTitlebarSeparatorStyle:NSTitlebarSeparatorStyleNone];
      [window setMovableByWindowBackground:YES];
    }
    [window setDelegate:self];
    [window setAcceptsMouseMovedEvents:YES];
    [window center];

    mbWebView webView = mbCreateWebView();
    if (_runtime)
      [_runtime configureWebView:webView window:window];
    ElectronBlinkView* view =
        [[ElectronBlinkView alloc] initWithFrame:[[window contentView] bounds]
                                        webView:webView
                                          owner:self];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [[window contentView] addSubview:view];
    [_windows addObject:window];
    if (options.visible)
      [window makeKeyAndOrderFront:nil];
    [window makeFirstResponder:view];

    NSString* source = [NSString stringWithUTF8String:options.url.c_str()];
    NSURL* url = nil;
    if ([source hasPrefix:@"http://"] || [source hasPrefix:@"https://"] ||
        [source hasPrefix:@"file://"])
      url = [NSURL URLWithString:source];
    else
      url = [NSURL fileURLWithPath:source];
    if (!url) {
      std::fprintf(stderr, "ELECTRON_DEMO_ERROR invalid URL: %s\n",
                   options.url.c_str());
      [NSApp terminate:nil];
      return;
    }
    mbLoadURL(webView, [[url absoluteString] UTF8String]);
#if !__has_feature(objc_arc)
    [view release];
    [window release];
#endif
  }
  [NSApp activateIgnoringOtherApps:YES];

  if (!_runtime) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
      if (!self->_verificationFinished) {
        std::fputs("ELECTRON_DEMO_ERROR timed out waiting for Blink frame\n",
                   stderr);
        [NSApp terminate:nil];
      }
    });
  }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}

- (void)viewDidFinishLoading:(ElectronBlinkView*)view url:(NSString*)url {
  if (_verificationFinished)
    return;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                 dispatch_get_main_queue(), ^{
    if (self->_verificationFinished)
      return;
    if (![view hasFrame]) {
      [self viewDidFinishLoading:view url:url];
      return;
    }
    if (self->_runtime) {
      self->_verificationFinished = YES;
      void (^finishVerification)(void) = ^{
        if ([self->_screenshotPath length]) {
          NSData* png = [view snapshotPNG];
          if (![png writeToFile:self->_screenshotPath atomically:YES]) {
            std::fputs("OMP_DESKTOP_ERROR cannot write snapshot\n", stderr);
            [NSApp terminate:nil];
            return;
          }
        }
        std::printf("OMP_DESKTOP_READY url=%s screenshot=%s\n", [url UTF8String],
                    [self->_screenshotPath length]
                        ? [self->_screenshotPath UTF8String]
                        : "none");
        std::fflush(stdout);
      };
      if ([self->_screenshotPath length]) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), finishVerification);
      } else {
        finishVerification();
      }
      return;
    }
    NSData* png = [view snapshotPNG];
    if (![png writeToFile:self->_screenshotPath atomically:YES]) {
      std::fputs("ELECTRON_DEMO_ERROR cannot write snapshot\n", stderr);
      [NSApp terminate:nil];
      return;
    }
    self->_verificationFinished = YES;
    std::printf("ELECTRON_DEMO_READY api=app,BrowserWindow windows=%lu url=%s screenshot=%s\n",
                (unsigned long)[self->_windows count], [url UTF8String],
                [self->_screenshotPath UTF8String]);
    std::fflush(stdout);
    if (self->_exerciseInput) {
      [view exerciseTypingAndHover];
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                     dispatch_get_main_queue(), ^{
        NSData* after = [view snapshotPNG];
        if (after)
          [after writeToFile:self->_screenshotPath atomically:YES];
        std::fputs("ELECTRON_DEMO_INPUT_OK\n", stdout);
        std::fflush(stdout);
        if (!self->_interactive)
          [NSApp terminate:nil];
      });
      return;
    }
    if (!self->_interactive)
      [NSApp terminate:nil];
  });
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
  ElectronBlinkView* view = (__bridge ElectronBlinkView*)parameter;
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
    std::fprintf(stderr, "ELECTRON_DEMO_ERROR navigation failed: %s\n",
                 failed_reason ? failed_reason : "unknown");
    return;
  }
  ElectronBlinkView* view = (__bridge ElectronBlinkView*)parameter;
  NSString* loadedURL = url ? [NSString stringWithUTF8String:url] : @"";
  dispatch_async(dispatch_get_main_queue(), ^{
    [view didFinishLoading:loadedURL];
  });
}

int main(int argc, char** argv) {
  @autoreleasepool {
    NSString* resourcesPath = NSBundle.mainBundle.resourcePath;
    NSString* packagedIndex =
        [resourcesPath stringByAppendingPathComponent:@"app-dist/index.html"];
    BOOL ompDesktopMode =
        [[NSFileManager defaultManager] fileExistsAtPath:packagedIndex];

    const char* script = argc >= 2
                             ? argv[1]
                             : "platform/macos/resources/electron-demo/main.js";
    const char* screenshot = argc >= 3
                                 ? argv[2]
                                 : "/tmp/miniblink132-electron-demo.png";
    BOOL interactive = ompDesktopMode;
    BOOL exerciseInput = NO;
    for (int i = 3; i < argc; ++i) {
      if (std::strcmp(argv[i], "--interactive") == 0)
        interactive = YES;
      if (std::strcmp(argv[i], "--exercise-input") == 0)
        exerciseInput = YES;
    }
    if (!ompDesktopMode &&
        ![[NSFileManager defaultManager]
            fileExistsAtPath:[NSString stringWithUTF8String:script]]) {
      std::fprintf(stderr, "ELECTRON_DEMO_ERROR script not found: %s\n", script);
      return 2;
    }

    NSApplication* application = [NSApplication sharedApplication];
    mbInit(nullptr);
    std::optional<miniblink::electron::AppPlan> plan;
    OmpDesktopRuntime* runtime = nil;
    if (ompDesktopMode) {
      runtime = [[OmpDesktopRuntime alloc] initWithResourcesPath:resourcesPath];
      miniblink::electron::AppPlan appPlan;
      miniblink::electron::BrowserWindowOptions options;
      options.width = 1280;
      options.height = 800;
      options.title = "OMP Desktop";
      options.url = [[runtime applicationURL] UTF8String];
      appPlan.windows.push_back(std::move(options));
      plan = std::move(appPlan);
    } else {
      std::string error;
      plan = miniblink::electron::EvaluateMainScript(script, &error);
      if (!plan) {
        std::fprintf(stderr, "ELECTRON_DEMO_ERROR %s\n", error.c_str());
        mbUninit();
        return 3;
      }
    }
    NSString* screenshotPath =
        ompDesktopMode
            ? NSProcessInfo.processInfo.environment[@"OMP_DESKTOP_SCREENSHOT"]
            : [NSString stringWithUTF8String:screenshot];
    __attribute__((objc_precise_lifetime)) ElectronAppDelegate* delegate =
        [[ElectronAppDelegate alloc] initWithPlan:std::move(*plan)
                                  screenshotPath:screenshotPath
                                     interactive:interactive
                                   exerciseInput:exerciseInput
                                         runtime:runtime];
#if !__has_feature(objc_arc)
    [runtime release];
#endif
    [application setDelegate:delegate];
    [application run];
    mbUninit();
  }
  return 0;
}
