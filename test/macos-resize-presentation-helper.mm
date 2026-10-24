#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#include "macos-owned-window-capture.h"
#include <node_api.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "src/runtime/electron/common/renderer_protocol.h"

using atom::renderer_protocol::FrameBufferHeader;
static pid_t stopped_pid = 0;

static napi_value Fail(napi_env env, const char* message) {
  napi_throw_error(env, nullptr, message);
  return nullptr;
}
static void Number(napi_env env, napi_value object, const char* key, double number) {
  napi_value value;
  napi_create_double(env, number, &value);
  napi_set_named_property(env, object, key, value);
}
static void Text(napi_env env, napi_value object, const char* key, const char* text) {
  napi_value value;
  napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &value);
  napi_set_named_property(env, object, key, value);
}
static napi_value RectValue(napi_env env, CGRect rect) {
  napi_value result;
  napi_create_object(env, &result);
  Number(env, result, "x", rect.origin.x);
  Number(env, result, "y", rect.origin.y);
  Number(env, result, "width", rect.size.width);
  Number(env, result, "height", rect.size.height);
  return result;
}
static NSWindow* Window(napi_env env, napi_value value) {
  size_t size = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &size) != napi_ok) {
    Fail(env, "Expected unique owned title"); return nil;
  }
  std::string title(size + 1, '\0');
  napi_get_value_string_utf8(env, value, title.data(), title.size(), &size);
  NSString* expected = [[NSString alloc] initWithBytes:title.data() length:size encoding:NSUTF8StringEncoding];
  NSWindow* found = nil;
  for (NSWindow* window in [NSApplication sharedApplication].windows) {
    if (![window.title isEqualToString:expected]) continue;
    if (found) { Fail(env, "Ambiguous owned title"); return nil; }
    found = window;
  }
  if (!found) Fail(env, "Owned window not found");
  return found;
}
static NSView* Renderer(NSView* view) {
  if ([NSStringFromClass(view.class) isEqualToString:@"MiniElectronRendererView"]) return view;
  for (NSView* child in view.subviews) {
    NSView* found = Renderer(child);
    if (found) return found;
  }
  return nil;
}
static napi_value Event(napi_env env, napi_callback_info info) {
  size_t count = 4; napi_value args[4];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 4) return Fail(env, "event(title, type, screenX, screenY) requires four arguments");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  int32_t kind = -1; double x = NAN, y = NAN;
  napi_get_value_int32(env, args[1], &kind);
  napi_get_value_double(env, args[2], &x); napi_get_value_double(env, args[3], &y);
  if (kind < 0 || kind > 2 || !std::isfinite(x) || !std::isfinite(y)) return Fail(env, "Invalid native event");
  NSEventType types[] = {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseDragged, NSEventTypeLeftMouseUp};
  NSEvent* event = [NSEvent mouseEventWithType:types[kind]
      location:[window convertPointFromScreen:NSMakePoint(x, y)] modifierFlags:0
      timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber
      context:nil eventNumber:88421 clickCount:1 pressure:kind == 2 ? 0 : 1];
  // Exactly one actual dispatch. JS yields between calls; no prequeued drag sequence.
  const double began = NSProcessInfo.processInfo.systemUptime;
  [window sendEvent:event];
  napi_value result = RectValue(env, window.frame);
  Number(env, result, "eventDurationMs", (NSProcessInfo.processInfo.systemUptime - began) * 1000);
  return result;
}
enum class CaptureKind { Snapshot, Sample, Geometry };
struct CaptureRequest {
  CaptureKind kind;
  CGRect renderer;
  bool flip;
  double x;
  double y;
  double began;
};
static napi_value PixelValue(napi_env env, const uint8_t* bytes) {
  napi_value result;
  napi_create_array_with_length(env, 4, &result);
  for (uint32_t channel = 0; channel < 4; ++channel) {
    napi_value value;
    napi_create_uint32(env, bytes[channel], &value);
    napi_set_element(env, result, channel, value);
  }
  return result;
}
static bool ReadPixel(CGImageRef image, size_t x, size_t y, uint8_t* bytes) {
  CGImageRef pixel = CGImageCreateWithImageInRect(image, CGRectMake(x, y, 1, 1));
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(bytes, 1, 1, 8, 4, space,
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(space);
  if (pixel && context) CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), pixel);
  if (context) CGContextRelease(context);
  if (pixel) CGImageRelease(pixel);
  return pixel && context;
}
static void CompleteCapture(napi_env env, napi_deferred deferred, napi_value result,
                            CGImageRef image, CGRect screen_rect, const CaptureRequest& request, NSString* error) {
  auto reject = [&](NSString* reason) {
    napi_value text, exception;
    NSString* message = [@"INCOMPLETE: actual composed owned-window capture failed. Use a visible owned window in an unlocked graphical session. " stringByAppendingString:reason];
    napi_create_string_utf8(env, message.UTF8String, NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, nullptr, text, &exception);
    napi_reject_deferred(env, deferred, exception);
  };
  if (!image || error) { reject(error ?: @"No composed owned-window image returned."); return; }
  if (CGRectIsEmpty(screen_rect)) { reject(@"Captured window rectangle is empty."); return; }
  const double scaleX = CGImageGetWidth(image) / screen_rect.size.width;
  const double scaleY = CGImageGetHeight(image) / screen_rect.size.height;
  const double originX = (request.renderer.origin.x - screen_rect.origin.x) * scaleX;
  const double originY = (request.renderer.origin.y - screen_rect.origin.y) * scaleY;
  // ScreenCaptureKit excludes AppKit's outer border on titled/full-size-content
  // windows. Decode only real visible pixels and retain the renderer offset.
  const CGRect visible = CGRectIntersection(
      CGRectMake(originX, originY, request.renderer.size.width * scaleX,
          request.renderer.size.height * scaleY),
      CGRectMake(0, 0, CGImageGetWidth(image), CGImageGetHeight(image)));
  if (CGRectIsEmpty(visible)) {
    reject(@"Renderer has no visible pixels in the captured owned window."); return;
  }
  const size_t left = static_cast<size_t>(std::floor(CGRectGetMinX(visible)));
  const size_t top = static_cast<size_t>(std::floor(CGRectGetMinY(visible)));
  const size_t width = static_cast<size_t>(std::ceil(CGRectGetMaxX(visible))) - left;
  const size_t height = static_cast<size_t>(std::ceil(CGRectGetMaxY(visible))) - top;
  Number(env, result, "scale", scaleX);
  Number(env, result, "pixelsPerDIP", scaleX);
  Number(env, result, "pixelsPerDIPY", scaleY);
  Number(env, result, "bitmapWidth", width); Number(env, result, "bitmapHeight", height);
  Number(env, result, "windowPixelWidth", CGImageGetWidth(image));
  Number(env, result, "windowPixelHeight", CGImageGetHeight(image));
  napi_set_named_property(env, result, "capturedWindowScreenRect", RectValue(env, screen_rect));
  napi_set_named_property(env, result, "rendererScreenRect", RectValue(env, request.renderer));
  if (request.kind == CaptureKind::Sample) {
    uint8_t bytes[4] = {};
    const double pixelX = std::floor(originX + request.x * scaleX);
    const double pixelY = std::floor(originY + request.y * scaleY);
    if (pixelX < 0 || pixelY < 0 || pixelX >= CGImageGetWidth(image) || pixelY >= CGImageGetHeight(image)) {
      reject(@"Requested renderer sample is outside the visible owned window."); return;
    }
    const size_t x = static_cast<size_t>(pixelX);
    const size_t y = static_cast<size_t>(pixelY);
    if (!ReadPixel(image, x, y, bytes)) { reject(@"Cannot decode composed owned-window sample."); return; }
    napi_resolve_deferred(env, deferred, PixelValue(env, bytes)); return;
  }
  if (request.kind == CaptureKind::Snapshot) {
    napi_value samples; napi_create_object(env, &samples);
    const char* names[] = {"topLeft", "top", "topRight", "left", "center", "right", "bottomLeft", "bottom", "bottomRight"};
    // Individual real composed owned-window pixels, not a resized/averaged screenshot. The
    // slight edge inset excludes AppKit's border; corner samples also exclude
    // rounded native mask and overlaid titlebar controls. Edge centers cover all sides.
    constexpr double edgeInset = 2;
    constexpr double cornerInset = 24;
    Number(env, result, "sampleEdgeInsetDIP", edgeInset);
    Number(env, result, "sampleCornerInsetDIP", cornerInset);
    for (size_t row = 0; row < 3; ++row) for (size_t column = 0; column < 3; ++column) {
      const double inset = row != 1 && column != 1 ? cornerInset : edgeInset;
      const size_t pixelX = static_cast<size_t>(std::floor(column == 0 ? CGRectGetMinX(visible) + inset * scaleX
          : column == 1 ? CGRectGetMidX(visible) : CGRectGetMaxX(visible) - inset * scaleX));
      const size_t pixelY = static_cast<size_t>(std::floor(row == 0 ? CGRectGetMinY(visible) + inset * scaleY
          : row == 1 ? CGRectGetMidY(visible) : CGRectGetMaxY(visible) - inset * scaleY));
      uint8_t bytes[4] = {};
      if (!ReadPixel(image, pixelX, pixelY, bytes)) {
        reject([NSString stringWithFormat:@"Cannot decode composed owned-window samples. sample=%zux%zu image=%zux%zu renderer=%@ captured=%@",
            pixelX, pixelY, CGImageGetWidth(image), CGImageGetHeight(image),
            NSStringFromRect(request.renderer), NSStringFromRect(screen_rect)]); return;
      }
      napi_set_named_property(env, samples, names[row * 3 + column], PixelValue(env, bytes));
    }
    napi_set_named_property(env, result, "samples", samples);
  } else {
    CGImageRef crop = CGImageCreateWithImageInRect(image, CGRectMake(left, top, width, height));
    std::vector<uint8_t> pixels(width * height * 4, 0);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4, space,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (!crop || !context) {
      if (crop) CGImageRelease(crop);
      if (context) CGContextRelease(context);
      reject(@"Cannot decode native-resolution composed owned-window geometry."); return;
    }
    // Decode only: identical source/destination pixel dimensions, no resampling.
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), crop);
    CGContextRelease(context); CGImageRelease(crop);
    const char* names[] = {"fixed", "responsive", "circle", "animated"};
    const int colors[][3] = {{241, 43, 67}, {31, 227, 137}, {47, 113, 251}, {249, 205, 37}};
    size_t minX[4] = {width, width, width, width}, minY[4] = {height, height, height, height};
    size_t maxX[4] = {}, maxY[4] = {}, hits[4] = {};
    for (size_t row = 0; row < height; ++row) for (size_t x = 0; x < width; ++x) {
      const uint8_t* pixel = pixels.data() + (row * width + x) * 4;
      if (pixel[3] < 250) continue;
      for (size_t marker = 0; marker < 4; ++marker) {
        if (std::abs(int(pixel[0]) - colors[marker][0]) > 4 ||
            std::abs(int(pixel[1]) - colors[marker][1]) > 4 ||
            std::abs(int(pixel[2]) - colors[marker][2]) > 4) continue;
        const size_t y = request.flip ? height - 1 - row : row;
        minX[marker] = std::min(minX[marker], x); minY[marker] = std::min(minY[marker], y);
        maxX[marker] = std::max(maxX[marker], x); maxY[marker] = std::max(maxY[marker], y);
        ++hits[marker];
      }
    }
    napi_value markers; napi_create_object(env, &markers);
    for (size_t marker = 0; marker < 4; ++marker) {
      napi_value box = RectValue(env, hits[marker] ? CGRectMake(
          (left + minX[marker] - originX) / scaleX, (top + minY[marker] - originY) / scaleY,
          (maxX[marker] - minX[marker] + 1) / scaleX, (maxY[marker] - minY[marker] + 1) / scaleY) : CGRectZero);
      Number(env, box, "pixels", hits[marker]);
      napi_set_named_property(env, markers, names[marker], box);
    }
    napi_set_named_property(env, result, "markers", markers);
    Text(env, result, "coordinates", request.flip ? "composed owned-window pixels, reversed decoded rows; view top-left DIPs" : "composed owned-window pixels, unchanged decoded rows; view top-left DIPs");
  }
  Number(env, result, "sampleDurationMs", (NSProcessInfo.processInfo.systemUptime - request.began) * 1000);
  napi_resolve_deferred(env, deferred, result);
}
static napi_value Capture(napi_env env, napi_callback_info info, CaptureKind kind) {
  size_t count = 3; napi_value args[3];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count < 1 || (kind == CaptureKind::Snapshot && count != 1) ||
      (kind == CaptureKind::Sample && count != 3) || (kind == CaptureKind::Geometry && count > 2))
    return Fail(env, "Expected snapshot(title), sample(title, viewX, viewY), or geometry(title, flipRows=false)");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  NSView* view = Renderer(window.contentView);
  if (!view || !view.layer || !window.screen || !window.visible)
    return Fail(env, "INCOMPLETE: a visible layer-backed renderer on an attached display is required");
  bool flip = false; double x = NAN, y = NAN;
  if (kind == CaptureKind::Geometry && count == 2 && napi_get_value_bool(env, args[1], &flip) != napi_ok)
    return Fail(env, "flipRows must be boolean");
  if (kind == CaptureKind::Sample) {
    napi_get_value_double(env, args[1], &x); napi_get_value_double(env, args[2], &y);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 ||
        x >= view.bounds.size.width || y >= view.bounds.size.height)
      return Fail(env, "Composed owned-window sample must be inside the renderer in top-left DIPs");
  }
  const double began = NSProcessInfo.processInfo.systemUptime;
  [window displayIfNeeded];
  [CATransaction flush];
  const double displayed = NSProcessInfo.processInfo.systemUptime;
  CGRect renderer = [window convertRectToScreen:[view convertRect:view.bounds toView:nil]];
  renderer.origin.y = NSMaxY(NSScreen.screens.firstObject.frame) - CGRectGetMaxY(renderer);
  if (CGRectIsEmpty(renderer)) return Fail(env, "INCOMPLETE: renderer has empty native bounds");
  const CaptureRequest request{kind, renderer, flip, x, y, displayed};
  napi_value promise, result; napi_deferred deferred; napi_ref reference;
  napi_create_object(env, &result);
  CALayer* layer = view.layer.presentationLayer ?: view.layer;
  napi_set_named_property(env, result, "frame", RectValue(env, window.frame));
  napi_set_named_property(env, result, "view", RectValue(env, view.bounds));
  napi_set_named_property(env, result, "layer", RectValue(env, layer.bounds));
  CGRect visible = window.screen.visibleFrame;
  visible.origin.y = NSMaxY(NSScreen.screens.firstObject.frame) - CGRectGetMaxY(visible);
  napi_set_named_property(env, result, "screen", RectValue(env, visible));
  Number(env, result, "displayDurationMs", (displayed - began) * 1000);
  napi_create_promise(env, &deferred, &promise);
  napi_create_reference(env, result, 1, &reference);
  mini_electron::test::CaptureOwnedWindow(window, ^(CGImageRef image, CGRect screen_rect, NSString* error) {
    if (image) CGImageRetain(image);
    dispatch_async(dispatch_get_main_queue(), ^{
      napi_handle_scope scope; napi_open_handle_scope(env, &scope);
      napi_value retained; napi_get_reference_value(env, reference, &retained);
      CompleteCapture(env, deferred, retained, image, screen_rect, request, error);
      napi_delete_reference(env, reference);
      if (image) CGImageRelease(image);
      napi_close_handle_scope(env, scope);
    });
  });
  Text(env, result, "source", "WindowServer.composedOwnedWindow");
  return promise;
}
static napi_value Snapshot(napi_env env, napi_callback_info info) {
  return Capture(env, info, CaptureKind::Snapshot);
}
static napi_value Sample(napi_env env, napi_callback_info info) {
  return Capture(env, info, CaptureKind::Sample);
}
static napi_value Geometry(napi_env env, napi_callback_info info) {
  return Capture(env, info, CaptureKind::Geometry);
}
static bool OwnedChild(pid_t pid) {
  proc_bsdinfo info = {};
  return pid > 0 && pid != getpid() &&
      proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) && info.pbi_ppid == getpid();
}
static void ResumeOwned() {
  if (stopped_pid && OwnedChild(stopped_pid)) kill(stopped_pid, SIGCONT);
  stopped_pid = 0;
}
static napi_value Resume(napi_env env, napi_callback_info) {
  ResumeOwned(); napi_value result; napi_get_undefined(env, &result); return result;
}
static napi_value Pause(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value args[1]; int32_t pid = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 1 || napi_get_value_int32(env, args[0], &pid) != napi_ok || !OwnedChild(pid))
    return Fail(env, "pauseRenderer requires the owned direct-child renderer PID");
  if (stopped_pid) return Fail(env, "Renderer already paused");
  proc_fdinfo descriptors[4096];
  int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, descriptors, sizeof(descriptors));
  if (bytes <= 0 || bytes == sizeof(descriptors)) return Fail(env, "Cannot enumerate framebuffer descriptors completely");
  FrameBufferHeader* header = nullptr;
  for (int i = 0; i < bytes / static_cast<int>(sizeof(descriptors[0])); ++i) {
    if (descriptors[i].proc_fdtype != PROX_FDTYPE_PSHM) continue;
    int fd = descriptors[i].proc_fd;
    if ((fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR) continue;
    void* mapping = mmap(nullptr, sizeof(FrameBufferHeader), PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) continue;
    auto* candidate = static_cast<FrameBufferHeader*>(mapping);
    if (candidate->magic == atom::renderer_protocol::kFrameMagic && candidate->capacity == atom::renderer_protocol::kMaxFrameBytes) {
      if (header) { munmap(mapping, sizeof(FrameBufferHeader)); munmap(header, sizeof(FrameBufferHeader)); return Fail(env, "Multiple framebuffers: ownership is ambiguous"); }
      header = candidate;
    } else munmap(mapping, sizeof(FrameBufferHeader));
  }
  if (!header) return Fail(env, "No unique owned shared framebuffer");
  uint64_t generation = 0;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (!OwnedChild(pid) || kill(pid, SIGSTOP)) break;
    stopped_pid = pid;
    int status = 0;
    if (waitpid(pid, &status, WUNTRACED) != pid || !WIFSTOPPED(status)) break;
    generation = header->generation.load(std::memory_order_acquire);
    if (generation && !(generation & 1)) break;
    ResumeOwned(); usleep(1000);
  }
  munmap(header, sizeof(FrameBufferHeader));
  if (!stopped_pid || !generation || (generation & 1)) { ResumeOwned(); return Fail(env, "Cannot stop renderer on complete publication"); }
  napi_value result; napi_create_double(env, generation, &result); return result;
}
NAPI_MODULE_INIT() {
  napi_add_env_cleanup_hook(env, [](void*) { ResumeOwned(); }, nullptr);
  struct Entry { const char* name; napi_callback callback; } entries[] = {
    {"event", Event}, {"snapshot", Snapshot}, {"sample", Sample}, {"geometry", Geometry}, {"pauseRenderer", Pause}, {"resumeRenderer", Resume}};
  for (const auto& entry : entries) {
    napi_value function; napi_create_function(env, entry.name, NAPI_AUTO_LENGTH, entry.callback, nullptr, &function);
    napi_set_named_property(env, exports, entry.name, function);
  }
  return exports;
}
