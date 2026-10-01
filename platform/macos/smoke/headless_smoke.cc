// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>

#include "platform/macos/smoke/engine_probe.h"

namespace {

struct ExecutionState {
  const char* executable_path;
  int exit_code = 1;
};

void RunJavaScriptSource(void* context) {
  auto* state = static_cast<ExecutionState*>(context);
  auto output = mini_electron::mac::RunEngineProbe(state->executable_path);
  if (output) {
    std::printf("%s\n", output->c_str());
    state->exit_code = 0;
  } else {
    std::fputs("JavaScript execution failed\n", stderr);
  }
  CFRunLoopStop(CFRunLoopGetCurrent());
}

}  // namespace

int main(int argc, char** argv) {
  ExecutionState state{argv[0]};
  CFRunLoopSourceContext source_context = {};
  source_context.info = &state;
  source_context.perform = RunJavaScriptSource;
  CFRunLoopSourceRef source =
      CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &source_context);
  if (!source) {
    std::fputs("CFRunLoop source creation failed\n", stderr);
    return 1;
  }

  CFRunLoopRef run_loop = CFRunLoopGetCurrent();
  CFRunLoopAddSource(run_loop, source, kCFRunLoopDefaultMode);
  CFRunLoopSourceSignal(source);
  CFRunLoopWakeUp(run_loop);
  CFRunLoopRun();
  CFRunLoopRemoveSource(run_loop, source, kCFRunLoopDefaultMode);
  CFRelease(source);
  return state.exit_code;
}
