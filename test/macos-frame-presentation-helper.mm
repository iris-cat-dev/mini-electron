#import <AppKit/AppKit.h>

#include <fcntl.h>
#include <libproc.h>
#include <node_api.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/runtime/electron/common/renderer_protocol.h"

using atom::renderer_protocol::FrameBufferHeader;

static NSBitmapImageRep* DrawNativeView(NSView* view) {
  NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc]
      initWithBitmapDataPlanes:nullptr
                   pixelsWide:static_cast<NSInteger>(view.bounds.size.width)
                   pixelsHigh:static_cast<NSInteger>(view.bounds.size.height)
                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
               colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
  [NSGraphicsContext saveGraphicsState];
  [NSGraphicsContext setCurrentContext:
      [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];
  [view drawRect:view.bounds];
  [NSGraphicsContext restoreGraphicsState];
  return bitmap;
}

static napi_value Probe(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t pid = 0;
  napi_get_value_int32(env, args[0], &pid);
  proc_bsdinfo child = {};
  if (pid <= 0 || pid == getpid() ||
      proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &child, sizeof(child)) != sizeof(child) ||
      child.pbi_ppid != getpid()) {
    napi_throw_error(env, nullptr, "Expected the owned renderer process");
    return nullptr;
  }
  NSView* view = nil;
  for (NSWindow* window in NSApp.windows) {
    if (window.contentView.bounds.size.width <= 600) continue;
    for (NSView* child in window.contentView.subviews) {
      if ([NSStringFromClass(child.class) isEqualToString:@"MiniElectronRendererView"]) {
        view = child;
        break;
      }
    }
    if (view) break;
  }
  if (!view) {
    napi_throw_error(env, nullptr, "No native renderer view");
    return nullptr;
  }

  FrameBufferHeader* header = nullptr;
  proc_fdinfo descriptors[1024];
  const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, descriptors, sizeof(descriptors));
  for (int index = 0; bytes > 0 && index < bytes / static_cast<int>(sizeof(descriptors[0])); ++index) {
    if (descriptors[index].proc_fdtype != PROX_FDTYPE_PSHM) continue;
    const int fd = descriptors[index].proc_fd;
    if ((fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR) continue;
    void* mapping = mmap(nullptr, sizeof(FrameBufferHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) continue;
    auto* candidate = static_cast<FrameBufferHeader*>(mapping);
    if (candidate->magic == atom::renderer_protocol::kFrameMagic &&
        candidate->capacity == atom::renderer_protocol::kMaxFrameBytes) {
      header = candidate;
      break;
    }
    munmap(mapping, sizeof(FrameBufferHeader));
  }
  if (!header) {
    napi_throw_error(env, nullptr, "No owned shared framebuffer descriptor");
    return nullptr;
  }
  // This legacy synthetic seqlock contention probe is software-only. Metal
  // coverage pauses the real producer and captures the composed owned window in JS.
  if (header->output_format != 0) {
    munmap(header, sizeof(FrameBufferHeader));
    napi_throw_error(env, nullptr, "CPU contention probe must not mutate a native Metal publication");
    return nullptr;
  }

  bool stopped = false;
  uint64_t generation = 0;
  @try {
    for (int attempt = 0; attempt < 30; ++attempt) {
      if (kill(pid, SIGSTOP)) break;
      int status = 0;
      if (waitpid(pid, &status, WUNTRACED) != pid || !WIFSTOPPED(status)) break;
      stopped = true;
      generation = header->generation.load(std::memory_order_acquire);
      if (!(generation & 1)) break;
      kill(pid, SIGCONT);
      stopped = false;
      usleep(1000);
    }
    if (!stopped || (generation & 1)) {
      napi_throw_error(env, nullptr, "Could not pause the renderer on a complete frame");
      return nullptr;
    }
    NSBitmapImageRep* before = DrawNativeView(view);
    // Deterministically hold the same in-progress publication that a hover or
    // scroll animation can race, without changing pixels or disabling checks.
    header->generation.store(generation + 1, std::memory_order_release);
    napi_value receiver, ignored;
    napi_get_global(env, &receiver);
    napi_call_function(env, receiver, args[1], 0, nullptr, &ignored);
    NSBitmapImageRep* during = DrawNativeView(view);
    const NSUInteger size = before.bytesPerRow * before.pixelsHigh;
    NSUInteger differences = 0;
    for (NSUInteger index = 0; index < size; ++index)
      if (before.bitmapData[index] != during.bitmapData[index]) ++differences;
    napi_value result, value, pixel;
    napi_create_object(env, &result);
    napi_create_double(env, differences, &value);
    napi_set_named_property(env, result, "differentBytes", value);
    napi_create_double(env, generation, &value);
    napi_set_named_property(env, result, "generation", value);
    napi_create_array_with_length(env, 4, &pixel);
    for (uint32_t channel = 0; channel < 4; ++channel) {
      napi_create_uint32(env, before.bitmapData[channel], &value);
      napi_set_element(env, pixel, channel, value);
    }
    napi_set_named_property(env, result, "pixel", pixel);
    return result;
  } @finally {
    if (stopped) {
      header->generation.store(generation, std::memory_order_release);
      kill(pid, SIGCONT);
    }
    munmap(header, sizeof(FrameBufferHeader));
  }
}

NAPI_MODULE_INIT() {
  napi_value function;
  napi_create_function(env, "probe", NAPI_AUTO_LENGTH, Probe, nullptr, &function);
  napi_set_named_property(env, exports, "probe", function);
  return exports;
}
