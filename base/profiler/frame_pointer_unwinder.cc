// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// TODO(crbug.com/40284755): Remove this and spanify to fix the errors.
#pragma allow_unsafe_buffers
#endif

#include "base/profiler/frame_pointer_unwinder.h"

#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/notreached.h"
#include "base/numerics/clamped_math.h"
#include "base/profiler/module_cache.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_APPLE)
#include <pthread/stack_np.h>
#endif

namespace {

// Given a frame pointer, returns the frame pointer of the calling stack
// frame and places the return address of the calling stack frame into
// `return_address`. Shim around `pthread_stack_frame_decode_np` where
// available since it handles pointer authentication on supported platforms.
// NB: The caller *must* ensure that there are 2+ uintptr_t's worth of memory at
// `frame_pointer`.
uintptr_t DecodeFrame(uintptr_t frame_pointer, uintptr_t* return_address)
{
#if BUILDFLAG(IS_APPLE)
    if (__builtin_available(iOS 12, *)) {
        return pthread_stack_frame_decode_np(frame_pointer, return_address);
    }
#endif
    const uintptr_t* fp = reinterpret_cast<uintptr_t*>(frame_pointer);

    // MSAN does not consider the frame pointers and return addresses to have
    // have been initialized in the normal sense, but they are actually
    // initialized.
    MSAN_UNPOISON(fp, sizeof(uintptr_t) * 2);

    uintptr_t next_frame = *fp;
    *return_address = *(fp + 1);
    return next_frame;
}

} // namespace

namespace base {

FramePointerUnwinder::FramePointerUnwinder() = default;

bool FramePointerUnwinder::CanUnwindFrom(const Frame& current_frame) const
{
    return current_frame.module && current_frame.module->IsNative();
}

UnwindResult FramePointerUnwinder::TryUnwind(
    UnwinderStateCapture* capture_state, RegisterContext* thread_context, uintptr_t stack_top, std::vector<Frame>* stack)
{
    // The initial frame describes the captured register context.
    DCHECK_GT(stack->size(), 0u);
#if defined(ARCH_CPU_ARM64)
    constexpr uintptr_t align_mask = 0x1;
#elif defined(ARCH_CPU_X86_64)
    constexpr uintptr_t align_mask = 0xf;
#elif defined(ARCH_CPU_X86)
    constexpr uintptr_t align_mask = alignof(uintptr_t) - 1;
#endif
    uintptr_t next_frame = RegisterContextFramePointer(thread_context);
    uintptr_t frame_lower_bound = RegisterContextStackPointer(thread_context);
    const auto is_fp_valid = [&](uintptr_t fp) {
        return fp >= frame_lower_bound && fp <= stack_top &&
            stack_top - fp >= sizeof(uintptr_t) * 2 && (fp & align_mask) == 0;
    };
    if (!is_fp_valid(next_frame))
        return UnwindResult::kAborted;
    for (;;) {
        if (!stack->back().module)
            return UnwindResult::kAborted;
        if (!stack->back().module->IsNative())
            return UnwindResult::kUnrecognizedFrame;
        uintptr_t retaddr;
        uintptr_t frame = next_frame;
        next_frame = DecodeFrame(frame, &retaddr);
        frame_lower_bound = frame + 1;
        if (next_frame == 0)
            return UnwindResult::kCompleted;
        const ModuleCache::Module* module = module_cache()->GetModuleForAddress(retaddr);
        const bool is_non_native_module = module && !module->IsNative();
        if (!is_non_native_module && !is_fp_valid(next_frame))
            return UnwindResult::kAborted;
        RegisterContextFramePointer(thread_context) = next_frame;
        RegisterContextInstructionPointer(thread_context) = retaddr;
        RegisterContextStackPointer(thread_context) = frame + sizeof(uintptr_t) * 2;
        stack->emplace_back(retaddr, module);
    }
}

} // namespace base
