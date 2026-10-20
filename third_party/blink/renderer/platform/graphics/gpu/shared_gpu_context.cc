// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"

#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "gpu/config/gpu_driver_bug_workaround_type.h"
#include "gpu/config/gpu_feature_info.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "third_party/blink/renderer/platform/scheduler/public/main_thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"

namespace blink {


SharedGpuContext* SharedGpuContext::GetInstanceForCurrentThread()
{
    DEFINE_THREAD_SAFE_STATIC_LOCAL(ThreadSpecific<SharedGpuContext>, thread_specific_instance, ());
    return thread_specific_instance;
}

SharedGpuContext::SharedGpuContext() = default;

// static
bool SharedGpuContext::IsGpuCompositingEnabled()
{
    SharedGpuContext* this_ptr = GetInstanceForCurrentThread();
    if (IsMainThread()) {
        // On the main thread we have the opportunity to keep
        // is_gpu_compositing_disabled_ up to date continuously without locking
        // up the thread, so we do it. This allows user code to adapt immediately
        // when there is a fallback to software compositing.
        this_ptr->is_gpu_compositing_disabled_ = Platform::Current()->IsGpuCompositingDisabled();
    } else {
        // The check for gpu compositing enabled implies a context will be
        // desired, so we combine them into a single trip to the main thread.
        //
        // TODO(crbug.com/1486981): It is possible for the value of
        // this_ptr->is_gpu_compositing_disabled_ to become stale without notice
        // if the compositor falls back to software compositing after this
        // initialization. There are currently no known observable bugs caused by
        // this, but in theory, we'd need a mechanism for propagating changes in
        // GPU compositing availability to worker threads.
        this_ptr->CreateContextProviderIfNeeded(/*only_if_gpu_compositing=*/true);
    }
    return !this_ptr->is_gpu_compositing_disabled_;
}

base::WeakPtr<WebGraphicsContext3DProviderWrapper> SharedGpuContext::ContextProviderWrapper()
{
    SharedGpuContext* instance = GetInstanceForCurrentThread();
    instance->CreateContextProviderIfNeeded(/*only_if_gpu_compositing=*/true);
    return instance->context_provider_wrapper_ ? instance->context_provider_wrapper_->GetWeakPtr() : nullptr;
}

WebGraphicsSharedImageInterfaceProvider* SharedGpuContext::SharedImageInterfaceProvider()
{
    // This runtime has no GPU channel/SharedImage service. Local WebGL contexts
    // own ordinary GL textures and do not provide this unrelated interface.
    return nullptr;
}

gpu::GpuMemoryBufferManager* SharedGpuContext::GetGpuMemoryBufferManager()
{
    auto* instance = GetInstanceForCurrentThread();
    instance->CreateContextProviderIfNeeded(/*only_if_gpu_compositing=*/true);
    return instance->gpu_memory_buffer_manager_;
}

void SharedGpuContext::SetGpuMemoryBufferManagerForTesting(gpu::GpuMemoryBufferManager* manager)
{
    auto* instance = GetInstanceForCurrentThread();
    DCHECK(!instance->gpu_memory_buffer_manager_ || !manager);
    instance->gpu_memory_buffer_manager_ = manager;
}

static void ReadCompositingPolicyOnMainThread(bool* gpu_compositing_disabled, base::WaitableEvent* event)
{
    DCHECK(IsMainThread());
    *gpu_compositing_disabled = Platform::Current()->IsGpuCompositingDisabled();
    event->Signal();
}

void SharedGpuContext::CreateContextProviderIfNeeded(bool only_if_gpu_compositing)
{
    if (is_gpu_compositing_disabled_ && only_if_gpu_compositing)
        return;
    if (context_provider_wrapper_ && !context_provider_wrapper_->ContextProvider()->IsContextLost())
        return;
    context_provider_wrapper_.reset();
    if (context_provider_factory_) {
        auto provider = context_provider_factory_.Run();
        if (provider)
            context_provider_wrapper_ = std::make_unique<WebGraphicsContext3DProviderWrapper>(std::move(provider));
    } else if (IsMainThread()) {
        is_gpu_compositing_disabled_ = Platform::Current()->IsGpuCompositingDisabled();
        if (is_gpu_compositing_disabled_ && only_if_gpu_compositing)
            return;
        auto provider = Platform::Current()->CreateSharedOffscreenGraphicsContext3DProvider();
        if (provider)
            context_provider_wrapper_ = std::make_unique<WebGraphicsContext3DProviderWrapper>(std::move(provider));
        gpu_memory_buffer_manager_ = Platform::Current()->GetGpuMemoryBufferManager();
    } else {
        // The platform's compositing policy is main-thread state. Query it there
        // even when software mode means no shared GPU context is needed.
        base::WaitableEvent event;
        auto task_runner = Thread::MainThread()->GetTaskRunner(MainThreadTaskRunnerRestricted());
        PostCrossThreadTask(*task_runner, FROM_HERE,
            CrossThreadBindOnce(&ReadCompositingPolicyOnMainThread, CrossThreadUnretained(&is_gpu_compositing_disabled_), CrossThreadUnretained(&event)));
        event.Wait();
    }
}


void SharedGpuContext::SetContextProviderFactoryForTesting(ContextProviderFactory factory)
{
    auto* instance = GetInstanceForCurrentThread();
    DCHECK(!instance->context_provider_wrapper_);
    instance->context_provider_factory_ = std::move(factory);
}

void SharedGpuContext::Reset()
{
    auto* instance = GetInstanceForCurrentThread();
    instance->is_gpu_compositing_disabled_ = false;
    instance->shared_image_interface_provider_.reset();
    instance->context_provider_wrapper_.reset();
    instance->context_provider_factory_.Reset();
    instance->gpu_memory_buffer_manager_ = nullptr;
}

bool SharedGpuContext::IsValidWithoutRestoring()
{
    auto* instance = GetInstanceForCurrentThread();
    return instance->context_provider_wrapper_ && !instance->context_provider_wrapper_->ContextProvider()->IsContextLost();
}

bool SharedGpuContext::AllowSoftwareToAcceleratedCanvasUpgrade()
{
    auto wrapper = ContextProviderWrapper();
    return wrapper && wrapper->ContextProvider()->RasterInterface()
        && !wrapper->ContextProvider()->GetGpuFeatureInfo().IsWorkaroundEnabled(gpu::DISABLE_SOFTWARE_TO_ACCELERATED_CANVAS_UPGRADE);
}

#if BUILDFLAG(IS_ANDROID)
bool SharedGpuContext::MaySupportImageChromium()
{
    SharedGpuContext* this_ptr = GetInstanceForCurrentThread();
    this_ptr->CreateContextProviderIfNeeded(/*only_if_gpu_compositing=*/true);
    if (!this_ptr->context_provider_wrapper_) {
        return false;
    }
    const gpu::GpuFeatureInfo& gpu_feature_info = this_ptr->context_provider_wrapper_->ContextProvider()->GetGpuFeatureInfo();
    return gpu_feature_info.status_values[gpu::GPU_FEATURE_TYPE_ANDROID_SURFACE_CONTROL] == gpu::kGpuFeatureStatusEnabled;
}
#endif // BUILDFLAG(IS_ANDROID)

} // blink
