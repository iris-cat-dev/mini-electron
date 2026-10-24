#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include "macos-owned-window-capture.h"
#import <CoreVideo/CoreVideo.h>

#include <fcntl.h>
#include <libproc.h>
#include <node_api.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "src/runtime/electron/common/renderer_protocol.h"

namespace {
using atom::renderer_protocol::FrameBufferHeader;
NSMutableDictionary<NSNumber*, NSNumber*>* screen_changes;
id screen_observer;
mach_timebase_info_data_t cpu_timebase;
double cpu_tick_seconds;

struct Timing {
  CVDisplayLinkRef link = nullptr;
  std::atomic<uint64_t> count{0};
  std::atomic<double> period_ms{0};
  std::atomic<uint64_t> first_host{0};
  std::atomic<uint64_t> last_host{0};
  ~Timing() {
    if (link) { CVDisplayLinkStop(link); CVDisplayLinkRelease(link); }
  }
};
std::unique_ptr<Timing> timing;

napi_value Fail(napi_env env, const char* message) {
  napi_throw_error(env, nullptr, message);
  return nullptr;
}
void Number(napi_env env, napi_value object, const char* key, double number) {
  napi_value value;
  napi_create_double(env, number, &value);
  napi_set_named_property(env, object, key, value);
}
void Text(napi_env env, napi_value object, const char* key, const char* text) {
  napi_value value;
  napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &value);
  napi_set_named_property(env, object, key, value);
}
napi_value Object(napi_env env) { napi_value value; napi_create_object(env, &value); return value; }
NSWindow* Window(napi_env env, napi_value value) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    Fail(env, "Expected unique owned window title"); return nil;
  }
  std::string title(length + 1, '\0');
  napi_get_value_string_utf8(env, value, title.data(), title.size(), &length);
  NSString* expected = [[NSString alloc] initWithBytes:title.data() length:length encoding:NSUTF8StringEncoding];
  NSWindow* found = nil;
  for (NSWindow* candidate in NSApp.windows) {
    if (![candidate.title isEqualToString:expected]) continue;
    if (found) { Fail(env, "Ambiguous owned window title"); return nil; }
    found = candidate;
  }
  if (!found) Fail(env, "Owned native window not found");
  return found;
}
CGDirectDisplayID DisplayId(NSScreen* screen) {
  return [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}
NSScreen* Screen(CGDirectDisplayID display) {
  for (NSScreen* screen in NSScreen.screens) if (DisplayId(screen) == display) return screen;
  return nil;
}
napi_value ScreenValue(napi_env env, NSScreen* screen) {
  napi_value result = Object(env);
  const CGDirectDisplayID display = DisplayId(screen);
  Number(env, result, "displayId", display);
  Number(env, result, "scale", screen.backingScaleFactor);
  Number(env, result, "x", screen.frame.origin.x);
  Number(env, result, "y", screen.frame.origin.y);
  Number(env, result, "width", screen.frame.size.width);
  Number(env, result, "height", screen.frame.size.height);
  Number(env, result, "maximumFramesPerSecond", screen.maximumFramesPerSecond);
  Number(env, result, "minimumRefreshIntervalMs", screen.minimumRefreshInterval * 1000);
  Number(env, result, "maximumRefreshIntervalMs", screen.maximumRefreshInterval * 1000);
  CVDisplayLinkRef reference = nullptr;
  const CVReturn cv_status = CVDisplayLinkCreateWithCGDisplay(display, &reference);
  Number(env, result, "cvCreateReturn", cv_status);
  if (reference) {
    const CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(reference);
    Number(env, result, "cvNominalHz",
           nominal.timeValue > 0 && !(nominal.flags & kCVTimeIsIndefinite)
               ? static_cast<double>(nominal.timeScale) / nominal.timeValue : 0);
    CVDisplayLinkRelease(reference);
  }
  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
  Number(env, result, "modeHz", mode ? CGDisplayModeGetRefreshRate(mode) : 0);
  if (mode) CGDisplayModeRelease(mode);
  Text(env, result, "name", screen.localizedName.UTF8String ?: "");
  Text(env, result, "ownership", "attached-display");
  return result;
}
napi_value Screens(napi_env env, napi_callback_info info) {
  napi_value result; napi_create_array(env, &result);
  uint32_t index = 0;
  for (NSScreen* screen in NSScreen.screens) napi_set_element(env, result, index++, ScreenValue(env, screen));
  return result;
}
napi_value State(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value args[1];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 1) return Fail(env, "state(title) requires a title");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  if (!window.screen) return Fail(env, "Owned native window has no screen");
  napi_value result = ScreenValue(env, window.screen);
  Number(env, result, "windowNumber", window.windowNumber);
  Number(env, result, "windowWidth", window.frame.size.width);
  Number(env, result, "windowHeight", window.frame.size.height);
  Number(env, result, "windowScale", window.backingScaleFactor);
  Number(env, result, "screenChangeNotifications", [screen_changes[@(window.windowNumber)] unsignedLongLongValue]);
  Number(env, result, "primaryDisplayId", CGMainDisplayID());
  return result;
}
napi_value Move(napi_env env, napi_callback_info info) {
  size_t count = 2; napi_value args[2]; uint32_t display = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 2 || napi_get_value_uint32(env, args[1], &display) != napi_ok)
    return Fail(env, "move(title, availableDisplayId) requires an actual display");
  NSWindow* window = Window(env, args[0]);
  NSScreen* screen = Screen(display);
  if (!window) return nullptr;
  if (!screen) return Fail(env, "Target display is no longer available");
  if (window.frame.size.width > screen.frame.size.width || window.frame.size.height > screen.frame.size.height)
    return Fail(env, "Target screen cannot contain the unchanged 1000x700 owned window");
  NSRect frame = window.frame;
  frame.origin = NSMakePoint(NSMidX(screen.frame) - frame.size.width / 2,
                            NSMidY(screen.frame) - frame.size.height / 2);
  [window setFrame:frame display:YES];
  napi_value result; napi_get_undefined(env, &result); return result;
}
CVReturn Tick(CVDisplayLinkRef link, const CVTimeStamp* now, const CVTimeStamp* output,
              CVOptionFlags flags, CVOptionFlags* out_flags, void* context) {
  auto* sample = static_cast<Timing*>(context);
  if (output->videoTimeScale > 0 && output->videoRefreshPeriod > 0)
    sample->period_ms.store(1000.0 * output->videoRefreshPeriod / output->videoTimeScale);
  uint64_t zero = 0;
  const uint64_t host = CVGetCurrentHostTime();
  sample->first_host.compare_exchange_strong(zero, host);
  sample->last_host.store(host);
  sample->count.fetch_add(1);
  return kCVReturnSuccess;
}
napi_value StartTiming(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value args[1]; uint32_t display = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 1 || napi_get_value_uint32(env, args[0], &display) != napi_ok || !Screen(display))
    return Fail(env, "startTiming requires an available actual display ID");
  timing.reset();
  timing = std::make_unique<Timing>();
  CVReturn status = CVDisplayLinkCreateWithCGDisplay(display, &timing->link);
  if (status == kCVReturnSuccess) status = CVDisplayLinkSetOutputCallback(timing->link, Tick, timing.get());
  if (status == kCVReturnSuccess) status = CVDisplayLinkStart(timing->link);
  napi_value result = Object(env);
  Number(env, result, "cvReturn", status);
  if (status != kCVReturnSuccess) timing.reset();
  return result;
}
napi_value TimingValue(napi_env env, napi_callback_info info) {
  napi_value result = Object(env);
  if (!timing) { Text(env, result, "status", "unavailable"); return result; }
  const uint64_t count = timing->count.load();
  const uint64_t first = timing->first_host.load(), last = timing->last_host.load();
  Text(env, result, "status", count > 2 ? "observed" : "no-callbacks");
  Number(env, result, "callbacks", count);
  Number(env, result, "periodMs", timing->period_ms.load());
  Number(env, result, "callbackHz", last > first && count > 1 ? (count - 1) * CVGetHostClockFrequency() / (last - first) : 0);
  return result;
}
napi_value StopTiming(napi_env env, napi_callback_info info) {
  timing.reset(); napi_value result; napi_get_undefined(env, &result); return result;
}
napi_value Frames(napi_env env, napi_callback_info info) {
  // Only inspect this isolated test application's own mappings. Never stop a
  // renderer, acquire its writer lock, or alter a generation to manufacture work.
  napi_value result; napi_create_array(env, &result);
  const int needed = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
  if (needed <= 0) return Fail(env, "Cannot enumerate owned shared frame descriptors");
  std::vector<proc_fdinfo> descriptors(needed / sizeof(proc_fdinfo) + 32);
  const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, descriptors.data(), static_cast<int>(descriptors.size() * sizeof(proc_fdinfo)));
  if (bytes <= 0) return Fail(env, "Cannot read owned shared frame descriptors");
  uint32_t index = 0;
  for (int item = 0; item < bytes / static_cast<int>(sizeof(proc_fdinfo)); ++item) {
    const int fd = descriptors[item].proc_fd;
    if (descriptors[item].proc_fdtype != PROX_FDTYPE_PSHM || (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR) continue;
    struct stat metadata = {};
    if (fstat(fd, &metadata) || metadata.st_size < static_cast<off_t>(sizeof(FrameBufferHeader))) continue;
    void* mapping = mmap(nullptr, sizeof(FrameBufferHeader), PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) continue;
    const auto* header = static_cast<const FrameBufferHeader*>(mapping);
    if (header->magic == atom::renderer_protocol::kFrameMagic && header->capacity == atom::renderer_protocol::kMaxFrameBytes) {
      napi_value frame = Object(env);
      Number(env, frame, "fd", fd);
      Number(env, frame, "generation", header->generation.load(std::memory_order_acquire));
      napi_set_element(env, result, index++, frame);
    }
    munmap(mapping, sizeof(FrameBufferHeader));
  }
  return result;
}
napi_value ProcessStats(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value args[1]; int32_t pid = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 1 || napi_get_value_int32(env, args[0], &pid) != napi_ok || pid <= 0)
    return Fail(env, "processStats requires a process ID");
  napi_value result = Object(env);
  Number(env, result, "pid", pid);
  Number(env, result, "wallSeconds", NSProcessInfo.processInfo.systemUptime);
  struct rusage_info_v4 usage = {};
  if (proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage))) {
    Text(env, result, "status", "unavailable"); return result;
  }
  Text(env, result, "status", "observed");
  // proc_pid_rusage reports Mach absolute-time ticks, not nanoseconds on
  // Apple Silicon. Cache the conversion and keep its calibration in evidence.
  Number(env, result, "cpuSeconds",
         (static_cast<double>(usage.ri_user_time) + usage.ri_system_time) * cpu_tick_seconds);
  Number(env, result, "cpuTimebaseNumerator", cpu_timebase.numer);
  Number(env, result, "cpuTimebaseDenominator", cpu_timebase.denom);
  if (pid == getpid()) {
    struct rusage self = {};
    if (!getrusage(RUSAGE_SELF, &self)) {
      Number(env, result, "selfRusageCpuSeconds",
             self.ru_utime.tv_sec + self.ru_stime.tv_sec +
                 (self.ru_utime.tv_usec + self.ru_stime.tv_usec) / 1e6);
    }
  }
  Number(env, result, "packageIdleWakeups", usage.ri_pkg_idle_wkups);
  Number(env, result, "interruptWakeups", usage.ri_interrupt_wkups);
  Number(env, result, "residentBytes", usage.ri_resident_size);
  return result;
}
void CompleteSnapshot(napi_env env, napi_deferred deferred, CGImageRef image,
                      NSString* destination, NSString* error) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);
  NSData* png = nil;
  if (image && !error) {
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
    png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (!png || ![png writeToFile:destination atomically:YES])
      error = @"Cannot write composed owned-window PNG.";
  }
  if (!image || error) {
    NSString* message = [@"Actual composed owned-window capture is incomplete. Use a visible owned window in an unlocked graphical macOS session. " stringByAppendingString:error ?: @"No owned-window image returned."];
    napi_value text, exception;
    napi_create_string_utf8(env, message.UTF8String, NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, nullptr, text, &exception);
    napi_reject_deferred(env, deferred, exception);
  } else {
    napi_value result;
    napi_create_double(env, png.length, &result);
    napi_resolve_deferred(env, deferred, result);
  }
  napi_close_handle_scope(env, scope);
}
napi_value Snapshot(napi_env env, napi_callback_info info) {
  size_t count = 2; napi_value args[2];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 2) return Fail(env, "snapshot(title, path) requires two arguments");
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  if (!window.visible || window.miniaturized || !window.screen)
    return Fail(env, "Composed owned-window evidence requires a visible window on an attached display");
  size_t length = 0;
  if (napi_get_value_string_utf8(env, args[1], nullptr, 0, &length) != napi_ok)
    return Fail(env, "Expected snapshot destination path");
  std::string path(length + 1, '\0');
  napi_get_value_string_utf8(env, args[1], path.data(), path.size(), &length);
  NSString* destination = [[NSString alloc] initWithBytes:path.data() length:length encoding:NSUTF8StringEncoding];
  napi_value promise;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);
  // Observe the existing native presentation without invalidating a renderer or
  // forcing a display/transaction that could pollute cadence and idle counters.
  mini_electron::test::CaptureOwnedWindow(window, ^(CGImageRef image, CGRect screen_rect, NSString* error) {
    if (image) CGImageRetain(image);
    dispatch_async(dispatch_get_main_queue(), ^{
      CompleteSnapshot(env, deferred, image, destination, error);
      if (image) CGImageRelease(image);
    });
  });
  return promise;
}
void Cleanup(void*) {
  timing.reset();
  if (screen_observer) [NSNotificationCenter.defaultCenter removeObserver:screen_observer];
  screen_observer = nil;
}
napi_value Close(napi_env env, napi_callback_info info) {
  Cleanup(nullptr); napi_value result; napi_get_undefined(env, &result); return result;
}
}  // namespace

NAPI_MODULE_INIT() {
  if (mach_timebase_info(&cpu_timebase) != KERN_SUCCESS || !cpu_timebase.denom)
    return Fail(env, "Cannot determine Mach CPU timebase");
  cpu_tick_seconds = static_cast<double>(cpu_timebase.numer) / cpu_timebase.denom / 1e9;
  screen_changes = [NSMutableDictionary dictionary];
  screen_observer = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidChangeScreenNotification object:nil queue:nil usingBlock:^(NSNotification* note) {
    NSWindow* window = note.object;
    NSNumber* key = @(window.windowNumber);
    screen_changes[key] = @([screen_changes[key] unsignedLongLongValue] + 1);
  }];
  napi_add_env_cleanup_hook(env, Cleanup, nullptr);
  struct { const char* name; napi_callback callback; } methods[] = {
      {"screens", Screens}, {"state", State}, {"move", Move},
      {"startTiming", StartTiming}, {"timing", TimingValue}, {"stopTiming", StopTiming},
      {"frames", Frames}, {"processStats", ProcessStats}, {"snapshot", Snapshot}, {"cleanup", Close}};
  for (const auto& method : methods) {
    napi_value function;
    napi_create_function(env, method.name, NAPI_AUTO_LENGTH, method.callback, nullptr, &function);
    napi_set_named_property(env, exports, method.name, function);
  }
  return exports;
}
