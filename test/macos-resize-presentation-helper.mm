#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
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
#include "runtime/electron/common/renderer_protocol.h"

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
static napi_value Snapshot(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value args[1];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 1) return Fail(env, "snapshot(title) requires one argument");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  NSView* view = Renderer(window.contentView);
  if (!view || !view.layer) return Fail(env, "No layer-backed renderer view; presentation extraction unsupported");
  // Exercise AppKit's normal backing-store display and commit. Never invoke drawRect.
  const double began = NSProcessInfo.processInfo.systemUptime;
  [window displayIfNeeded];
  [CATransaction flush];
  const double displayed = NSProcessInfo.processInfo.systemUptime;
  CALayer* model = view.layer;
  CALayer* layer = model.presentationLayer ?: model;
  const CGFloat scale = window.backingScaleFactor;
  if (view.bounds.size.width <= 0 || view.bounds.size.height <= 0)
    return Fail(env, "Unsupported layer dimensions");
  // A small composite keeps observation from slowing a rapid drag into a
  // low-frequency resize. Uniform-background samples include all four edges.
  constexpr size_t width = 32;
  constexpr size_t height = 32;
  uint8_t pixels[width * height * 4] = {};
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(pixels, width, height, 8, width * 4,
      space, static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(space);
  if (!context) return Fail(env, "Cannot create CALayer extraction context");
  CGContextScaleCTM(context, width / view.bounds.size.width,
                     height / view.bounds.size.height);
  CGContextTranslateCTM(context, -layer.bounds.origin.x, -layer.bounds.origin.y);
  [layer renderInContext:context];
  CGContextRelease(context);
  napi_value result, samples;
  napi_create_object(env, &result);
  napi_set_named_property(env, result, "frame", RectValue(env, window.frame));
  napi_set_named_property(env, result, "view", RectValue(env, view.bounds));
  napi_set_named_property(env, result, "layer", RectValue(env, layer.bounds));
  Number(env, result, "scale", scale);
  Number(env, result, "bitmapWidth", width); Number(env, result, "bitmapHeight", height);
  Number(env, result, "displayDurationMs", (displayed - began) * 1000);
  Number(env, result, "sampleDurationMs", (NSProcessInfo.processInfo.systemUptime - displayed) * 1000);
  Text(env, result, "source", model.presentationLayer ? "presentationLayer.renderInContext" : "committedLayer.renderInContext");
  id contents = layer.contents;
  if (contents && CFGetTypeID((__bridge CFTypeRef)contents) == CGImageGetTypeID()) {
    CGImageRef image = (__bridge CGImageRef)contents;
    Number(env, result, "contentsWidth", CGImageGetWidth(image));
    Number(env, result, "contentsHeight", CGImageGetHeight(image));
    Text(env, result, "contentsType", "CGImage");
  } else {
    Number(env, result, "contentsWidth", 0); Number(env, result, "contentsHeight", 0);
    Text(env, result, "contentsType", contents ? NSStringFromClass([contents class]).UTF8String : "nil");
  }
  napi_create_object(env, &samples);
  const char* names[] = {"bottomLeft", "bottom", "bottomRight", "left", "center", "right", "topLeft", "top", "topRight"};
  const size_t xs[] = {0, width / 2, width - 1};
  const size_t ys[] = {0, height / 2, height - 1};
  for (size_t row = 0; row < 3; ++row) for (size_t column = 0; column < 3; ++column) {
    napi_value pixel, value; napi_create_array_with_length(env, 4, &pixel);
    size_t offset = (ys[row] * width + xs[column]) * 4;
    for (uint32_t channel = 0; channel < 4; ++channel) {
      napi_create_uint32(env, pixels[offset + channel], &value); napi_set_element(env, pixel, channel, value);
    }
    napi_set_named_property(env, samples, names[row * 3 + column], pixel);
  }
  napi_set_named_property(env, result, "samples", samples);
  return result;
}
static napi_value Sample(napi_env env, napi_callback_info info) {
  size_t count = 3; napi_value args[3];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 3) return Fail(env, "sample(title, layerX, layerY) requires three arguments");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  NSView* view = Renderer(window.contentView);
  double x = NAN, y = NAN;
  napi_get_value_double(env, args[1], &x); napi_get_value_double(env, args[2], &y);
  if (!view || !view.layer || !std::isfinite(x) || !std::isfinite(y))
    return Fail(env, "Invalid presentation sample");
  [window displayIfNeeded];
  [CATransaction flush];
  CALayer* layer = view.layer.presentationLayer ?: view.layer;
  uint8_t pixel[4] = {};
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(pixel, 1, 1, 8, 4, space,
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(space);
  if (!context) return Fail(env, "Cannot create pixel extraction context");
  CGContextTranslateCTM(context, -x, -y);
  [layer renderInContext:context];
  CGContextRelease(context);
  napi_value result, value;
  napi_create_array_with_length(env, 4, &result);
  for (uint32_t channel = 0; channel < 4; ++channel) {
    napi_create_uint32(env, pixel[channel], &value);
    napi_set_element(env, result, channel, value);
  }
  return result;
}
// One pixel per DIP. The caller calibrates bitmap row orientation against the
// asymmetric initial DOM markers, then supplies that same flip for every frame.
static napi_value Geometry(napi_env env, napi_callback_info info) {
  size_t count = 2; napi_value args[2]; bool flip = false;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count < 1) return Fail(env, "geometry(title, flipRows=false) requires a title");
  if (count == 2 && napi_get_value_bool(env, args[1], &flip) != napi_ok)
    return Fail(env, "flipRows must be boolean");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  NSView* view = Renderer(window.contentView);
  if (!view || !view.layer) return Fail(env, "No layer-backed renderer geometry");
  [window displayIfNeeded];
  [CATransaction flush];
  CALayer* model = view.layer;
  CALayer* layer = model.presentationLayer ?: model;
  const size_t width = static_cast<size_t>(std::ceil(view.bounds.size.width));
  const size_t height = static_cast<size_t>(std::ceil(view.bounds.size.height));
  if (!width || !height || width > 8192 || height > 8192)
    return Fail(env, "Unsupported geometry bitmap dimensions");
  std::vector<uint8_t> pixels(width * height * 4, 0);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4,
      space, static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(space);
  if (!context) return Fail(env, "Cannot create geometry extraction context");
  // No scaling of the layer into the capture: one context unit equals one DIP.
  CGContextTranslateCTM(context, -layer.bounds.origin.x, -layer.bounds.origin.y);
  [layer renderInContext:context];
  CGContextRelease(context);
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
      const size_t y = flip ? height - 1 - row : row;
      minX[marker] = std::min(minX[marker], x); minY[marker] = std::min(minY[marker], y);
      maxX[marker] = std::max(maxX[marker], x); maxY[marker] = std::max(maxY[marker], y);
      ++hits[marker];
    }
  }
  napi_value result, markers; napi_create_object(env, &result); napi_create_object(env, &markers);
  for (size_t marker = 0; marker < 4; ++marker) {
    napi_value box = RectValue(env, hits[marker] ? CGRectMake(minX[marker], minY[marker],
        maxX[marker] - minX[marker] + 1, maxY[marker] - minY[marker] + 1) : CGRectZero);
    Number(env, box, "pixels", hits[marker]);
    napi_set_named_property(env, markers, names[marker], box);
  }
  napi_set_named_property(env, result, "markers", markers);
  napi_set_named_property(env, result, "frame", RectValue(env, window.frame));
  napi_set_named_property(env, result, "view", RectValue(env, view.bounds));
  napi_set_named_property(env, result, "layer", RectValue(env, layer.bounds));
  napi_set_named_property(env, result, "screen", RectValue(env, window.screen.visibleFrame));
  Number(env, result, "bitmapWidth", width); Number(env, result, "bitmapHeight", height);
  Number(env, result, "pixelsPerDIP", 1);
  Text(env, result, "coordinates", flip ? "bitmap rows reversed; baseline-calibrated view top-left" : "bitmap rows unchanged; baseline-calibrated view top-left");
  Text(env, result, "source", model.presentationLayer ? "presentationLayer.renderInContext" : "committedLayer.renderInContext");
  return result;
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
