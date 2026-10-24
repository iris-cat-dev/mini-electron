#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include "macos-owned-window-capture.h"
#include <node_api.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cmath>
#include <string>
#include <vector>
#include "src/runtime/electron/common/renderer_protocol.h"

namespace {
using atom::renderer_protocol::FrameBufferHeader;
pid_t stopped_pid = 0;
napi_value Fail(napi_env env, const char* message) {
  napi_throw_error(env, nullptr, message); return nullptr;
}
void Number(napi_env env, napi_value object, const char* key, double number) {
  napi_value value; napi_create_double(env, number, &value);
  napi_set_named_property(env, object, key, value);
}
bool OwnedChild(pid_t pid) {
  proc_bsdinfo info = {};
  return pid > 0 && pid != getpid() &&
      proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) && info.pbi_ppid == getpid();
}
void ResumeOwned() {
  if (stopped_pid && OwnedChild(stopped_pid)) kill(stopped_pid, SIGCONT);
  stopped_pid = 0;
}
const FrameBufferHeader* Map(int fd) {
  struct stat metadata = {};
  if (fstat(fd, &metadata) || metadata.st_size < static_cast<off_t>(sizeof(FrameBufferHeader))) return nullptr;
  void* memory = mmap(nullptr, sizeof(FrameBufferHeader), PROT_READ, MAP_SHARED, fd, 0);
  if (memory == MAP_FAILED) return nullptr;
  const auto* header = static_cast<const FrameBufferHeader*>(memory);
  if (header->magic == atom::renderer_protocol::kFrameMagic &&
      header->capacity == atom::renderer_protocol::kMaxFrameBytes) return header;
  munmap(memory, sizeof(FrameBufferHeader)); return nullptr;
}
napi_value Frame(napi_env env, int fd, const FrameBufferHeader* header) {
  // No writer locks or synthetic publications: retry only genuinely concurrent metadata.
  for (int attempt = 0; attempt < 100; ++attempt) {
    const uint64_t generation = header->generation.load(std::memory_order_acquire);
    if (generation & 1) continue;
    const uint32_t width = header->width, height = header->height;
    const uint32_t format = header->output_format, context = header->native_context_id;
    const float scale = header->native_scale_factor;
    const uint64_t presented = header->presented_generation;
    const uint32_t data_size = header->data_size;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (generation != header->generation.load(std::memory_order_acquire)) continue;
    napi_value result; napi_create_object(env, &result);
    Number(env, result, "fd", fd); Number(env, result, "generation", generation);
    Number(env, result, "outputFormat", format); Number(env, result, "nativeContextId", context);
    Number(env, result, "nativeScaleFactor", scale); Number(env, result, "presentedGeneration", presented);
    Number(env, result, "width", width); Number(env, result, "height", height);
    Number(env, result, "dataSize", data_size);
    return result;
  }
  return Fail(env, "Could not observe stable framebuffer metadata");
}
napi_value Frames(napi_env env, napi_callback_info) {
  const int needed = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
  if (needed <= 0) return Fail(env, "Cannot enumerate owned frame descriptors");
  std::vector<proc_fdinfo> descriptors(needed / sizeof(proc_fdinfo) + 32);
  const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, descriptors.data(), descriptors.size() * sizeof(proc_fdinfo));
  if (bytes <= 0 || bytes == static_cast<int>(descriptors.size() * sizeof(proc_fdinfo)))
    return Fail(env, "Cannot enumerate owned frame descriptors completely");
  napi_value result; napi_create_array(env, &result); uint32_t index = 0;
  for (int item = 0; item < bytes / static_cast<int>(sizeof(proc_fdinfo)); ++item) {
    const int fd = descriptors[item].proc_fd;
    if (descriptors[item].proc_fdtype != PROX_FDTYPE_PSHM || (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR) continue;
    const auto* header = Map(fd);
    if (!header) continue;
    napi_value frame = Frame(env, fd, header);
    munmap(const_cast<FrameBufferHeader*>(header), sizeof(FrameBufferHeader));
    if (!frame) return nullptr;
    napi_set_element(env, result, index++, frame);
  }
  return result;
}
napi_value Pause(napi_env env, napi_callback_info info) {
  size_t count = 2; napi_value args[2]; int32_t pid = 0, fd = -1;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 2 || napi_get_value_int32(env, args[0], &pid) != napi_ok ||
      napi_get_value_int32(env, args[1], &fd) != napi_ok || !OwnedChild(pid) || stopped_pid)
    return Fail(env, "pauseRenderer requires an owned direct-child renderer, its frame fd, and no existing pause");
  const auto* header = Map(fd);
  if (!header) return Fail(env, "Not an owned frame mapping");
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
  munmap(const_cast<FrameBufferHeader*>(header), sizeof(FrameBufferHeader));
  if (!stopped_pid || !generation || (generation & 1)) {
    ResumeOwned(); return Fail(env, "Cannot pause owned producer on a complete publication");
  }
  napi_value result; napi_create_double(env, generation, &result); return result;
}
napi_value Resume(napi_env env, napi_callback_info) {
  ResumeOwned(); napi_value value; napi_get_undefined(env, &value); return value;
}
NSWindow* Window(napi_env env, napi_value arg) {
  size_t size = 0;
  if (napi_get_value_string_utf8(env, arg, nullptr, 0, &size) != napi_ok) {
    Fail(env, "Expected a unique owned window title"); return nil;
  }
  std::string title(size + 1, '\0');
  napi_get_value_string_utf8(env, arg, title.data(), title.size(), &size);
  NSString* expected = [[NSString alloc] initWithBytes:title.data() length:size encoding:NSUTF8StringEncoding];
  NSWindow* found = nil;
  for (NSWindow* window in NSApp.windows) {
    if (![window.title isEqualToString:expected]) continue;
    if (found) { Fail(env, "Ambiguous owned title"); return nil; }
    found = window;
  }
  if (!found) Fail(env, "Owned window not found");
  return found;
}
napi_value WindowState(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value args[1];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 1) return Fail(env, "windowState(title) requires one argument");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  napi_value result; napi_create_object(env, &result);
  Number(env, result, "nativeVisible", window.visible);
  Number(env, result, "minimized", window.miniaturized);
  Number(env, result, "occlusionState", window.occlusionState);
  Number(env, result, "exposed", (window.occlusionState & NSWindowOcclusionStateVisible) != 0);
  return result;
}
bool ReadPoints(napi_env env, napi_value input, double width, double height,
                std::vector<CGPoint>* points) {
  bool array = false; uint32_t length = 0;
  napi_is_array(env, input, &array);
  if (!array || napi_get_array_length(env, input, &length) != napi_ok || !length || length > 1000) {
    Fail(env, "Expected 1..1000 image-relative sample points");
    return false;
  }
  points->reserve(length);
  for (uint32_t i = 0; i < length; ++i) {
    napi_value point, value; double x = NAN, y = NAN;
    napi_get_element(env, input, i, &point);
    napi_get_named_property(env, point, "x", &value); napi_get_value_double(env, value, &x);
    napi_get_named_property(env, point, "y", &value); napi_get_value_double(env, value, &y);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= width || y >= height) {
      Fail(env, "Point lies outside image bounds");
      return false;
    }
    points->push_back(CGPointMake(x, y));
  }
  return true;
}
napi_value SamplePixels(napi_env env, CGImageRef image, CGRect content,
                        CGRect capture_rect, const std::vector<CGPoint>& points,
                        NSString** error) {
  const size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  napi_value samples; napi_create_array_with_length(env, points.size(), &samples);
  for (uint32_t i = 0; i < points.size(); ++i) {
    const size_t x = std::floor((content.origin.x + points[i].x - capture_rect.origin.x) * width / capture_rect.size.width);
    const size_t y = std::floor((content.origin.y + points[i].y - capture_rect.origin.y) * height / capture_rect.size.height);
    if (x >= width || y >= height) { *error = @"Sample lies outside captured image"; break; }
    CGImageRef pixel_image = CGImageCreateWithImageInRect(image, CGRectMake(x, y, 1, 1));
    uint8_t bytes[4] = {};
    CGContextRef context = CGBitmapContextCreate(bytes, 1, 1, 8, 4, space,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    if (!context || !pixel_image) *error = @"Cannot decode image pixel";
    else {
      CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), pixel_image);
      napi_value pixel; napi_create_array_with_length(env, 4, &pixel);
      for (uint32_t channel = 0; channel < 4; ++channel) {
        napi_value number; napi_create_uint32(env, bytes[channel], &number);
        napi_set_element(env, pixel, channel, number);
      }
      napi_set_element(env, samples, i, pixel);
    }
    if (context) CGContextRelease(context);
    if (pixel_image) CGImageRelease(pixel_image);
    if (*error) break;
  }
  CGColorSpaceRelease(space);
  return samples;
}
napi_value ImageSamples(napi_env env, napi_callback_info info) {
  size_t count = 2; napi_value args[2];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  bool buffer = false; void* bytes = nullptr; size_t length = 0;
  if (count != 2 || napi_is_buffer(env, args[0], &buffer) != napi_ok || !buffer ||
      napi_get_buffer_info(env, args[0], &bytes, &length) != napi_ok || !length)
    return Fail(env, "imageSamples(PNG buffer, [{x,y}, ...]) requires encoded pixels");
  CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
      static_cast<const UInt8*>(bytes), length, kCFAllocatorNull);
  CGImageSourceRef source = data ? CGImageSourceCreateWithData(data, nullptr) : nullptr;
  CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
  if (source) CFRelease(source);
  if (data) CFRelease(data);
  if (!image) return Fail(env, "Cannot decode explicit capture PNG");
  const size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
  std::vector<CGPoint> points;
  if (!ReadPoints(env, args[1], width, height, &points)) {
    CGImageRelease(image);
    return nullptr;
  }
  const CGRect bounds = CGRectMake(0, 0, width, height);
  NSString* error = nil;
  napi_value samples = SamplePixels(env, image, bounds, bounds, points, &error);
  CGImageRelease(image);
  if (error) return Fail(env, error.UTF8String);
  napi_value result; napi_create_object(env, &result);
  napi_set_named_property(env, result, "samples", samples);
  Number(env, result, "width", width); Number(env, result, "height", height);
  return result;
}
void Complete(napi_env env, napi_deferred deferred, CGImageRef image, NSString* error,
              CGRect content, CGRect capture_rect, const std::vector<CGPoint>& points) {
  napi_handle_scope scope; napi_open_handle_scope(env, &scope);
  napi_value result = nullptr;
  if (image && !error) {
    const size_t width = CGImageGetWidth(image);
    napi_value samples = SamplePixels(env, image, content, capture_rect, points, &error);
    napi_create_object(env, &result); napi_set_named_property(env, result, "samples", samples);
    Number(env, result, "width", content.size.width); Number(env, result, "height", content.size.height);
    Number(env, result, "displayScale", width / capture_rect.size.width);
    napi_value source; napi_create_string_utf8(env, "WindowServer composed owned window", NAPI_AUTO_LENGTH, &source);
    napi_set_named_property(env, result, "source", source);
  }
  if (!image || error) {
    NSString* message = [@"INCOMPLETE: actual composed owned-window capture unavailable. Use a visible owned window in an unlocked graphical session. " stringByAppendingString:error ?: @"No WindowServer image"];
    napi_value text, exception;
    napi_create_string_utf8(env, message.UTF8String, NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, nullptr, text, &exception); napi_reject_deferred(env, deferred, exception);
  } else napi_resolve_deferred(env, deferred, result);
  napi_close_handle_scope(env, scope);
}
napi_value Snapshot(napi_env env, napi_callback_info info) {
  size_t count = 2; napi_value args[2];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 2) return Fail(env, "snapshot(title, [{x,y}, ...]) requires two arguments");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  if (!window.visible || !window.screen)
    return Fail(env, "INCOMPLETE: owned window must be visible on an unlocked display");
  CGRect content = [window convertRectToScreen:[window.contentView convertRect:window.contentView.bounds toView:nil]];
  content.origin.y = NSMaxY(NSScreen.screens.firstObject.frame) - CGRectGetMaxY(content);
  std::vector<CGPoint> points;
  if (!ReadPoints(env, args[1], content.size.width, content.size.height, &points))
    return nullptr;
  napi_value promise; napi_deferred deferred; napi_create_promise(env, &deferred, &promise);
  mini_electron::test::CaptureOwnedWindow(window, ^(CGImageRef image, CGRect capture_rect, NSString* error) {
    if (image) CGImageRetain(image);
    dispatch_async(dispatch_get_main_queue(), ^{
      Complete(env, deferred, image, error, content, capture_rect, points);
      if (image) CGImageRelease(image);
    });
  });
  return promise;
}
}  // namespace
NAPI_MODULE_INIT() {
  napi_add_env_cleanup_hook(env, [](void*) { ResumeOwned(); }, nullptr);
  struct { const char* name; napi_callback callback; } methods[] = {
      {"frames", Frames}, {"snapshot", Snapshot}, {"imageSamples", ImageSamples},
      {"windowState", WindowState},
      {"pauseRenderer", Pause}, {"resumeRenderer", Resume}};
  for (const auto& method : methods) {
    napi_value function; napi_create_function(env, method.name, NAPI_AUTO_LENGTH, method.callback, nullptr, &function);
    napi_set_named_property(env, exports, method.name, function);
  }
  return exports;
}
