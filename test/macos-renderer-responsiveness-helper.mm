#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <dlfcn.h>
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  @autoreleasepool {
    const pid_t parent = std::atoi(argv[1]);
    const pid_t renderer = std::atoi(argv[2]);
    proc_bsdinfo process = {};
    if (parent != getppid() || renderer <= 1 || renderer == parent ||
        proc_pidinfo(renderer, PROC_PIDTBSDINFO, 0, &process, sizeof(process)) != sizeof(process) ||
        process.pbi_ppid != parent) {
      std::fputs("Refusing to target a process outside this smoke's renderer children\n", stderr);
      return 2;
    }
    using MainConnection = int32_t (*)();
    using IsUnresponsive = bool (*)(int32_t, const ProcessSerialNumber*);
    void* library = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
    auto connection = reinterpret_cast<MainConnection>(dlsym(library, "CGSMainConnectionID"));
    auto unresponsive = reinterpret_cast<IsUnresponsive>(dlsym(library, "CGSEventIsAppUnresponsive"));
    if (!connection || !unresponsive) {
      std::fputs("The macOS WindowServer responsiveness probe is unavailable\n", stderr);
      return 2;
    }
    const int32_t cid = connection();
    ProcessSerialNumber psn = {};
    NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:renderer];
    if (!app || GetProcessForPID(renderer, &psn) != noErr) {
      std::fputs("Renderer is not registered with macOS\n", stderr);
      return 1;
    }

    // These target only the owned renderer. No mouse or keyboard input is sent
    // to the user's active application. A plain NSRunLoop leaves them pending.
    [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
    CGEventRef event = CGEventCreate(nullptr);
    CGEventPostToPid(renderer, event);
    CFRelease(event);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    do {
      if (kill(renderer, 0) != 0 || unresponsive(cid, &psn)) {
        std::fprintf(stderr, "Renderer %d exited or became unresponsive after native application events\n", renderer);
        return 1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    } while (std::chrono::steady_clock::now() < deadline);
    std::printf("PASS: renderer %d remains responsive to macOS application events\n", renderer);
    dlclose(library);
  }
}
