// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webgpu/gpu_external_texture.h"

#include "media/base/video_frame.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_external_texture_descriptor.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_texture_view_descriptor.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_union_htmlvideoelement_videoframe.h"
#include "third_party/blink/renderer/core/dom/scripted_animation_controller.h"
#include "third_party/blink/renderer/core/html/canvas/predefined_color_space.h"
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#include "third_party/blink/renderer/modules/webcodecs/video_frame.h"
#include "third_party/blink/renderer/modules/webgpu/dawn_conversions.h"
#include "third_party/blink/renderer/modules/webgpu/external_texture_helper.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_device.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_queue.h"
#include "third_party/blink/renderer/platform/graphics/gpu/dawn_control_client_holder.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"

namespace blink {
ExternalTextureCache::ExternalTextureCache(GPUDevice* device)
    : device_(device)
{
}

GPUExternalTexture* ExternalTextureCache::Import(const GPUExternalTextureDescriptor* descriptor, ExceptionState& exception_state)
{
    PredefinedColorSpace color_space;
    if (!ValidateAndConvertColorSpace(descriptor->colorSpace(), color_space, exception_state))
        return nullptr;
    // Ensure the GPUExternalTexture created from a destroyed GPUDevice will be
    // expired immediately.
    if (device()->destroyed() || device()->GetDawnControlClient()->IsContextLost()) {
        return GPUExternalTexture::CreateExpired(this, descriptor, exception_state);
    }

    auto& sources = GetOrCreateColorSpaceCache(color_space);
    GPUExternalTexture* external_texture = nullptr;
    switch (descriptor->source()->GetContentType()) {
    case V8UnionHTMLVideoElementOrVideoFrame::ContentType::kHTMLVideoElement: {
        HTMLVideoElement* video = descriptor->source()->GetAsHTMLVideoElement();
        auto cache = sources.from_html_video_element.find(video);
        if (cache != sources.from_html_video_element.end()) {
            external_texture = cache->value;

            // If we got a cache miss, or `ContinueCheckingCurrentVideoFrame`
            // returned false, make a new external texture.
            // `ContinueCheckingCurrentVideoFrame` returns false if the frame has
            // expired and it no longer needs to be checked for expiry.
            if (external_texture->NeedsToUpdate()) {
                external_texture = GPUExternalTexture::FromHTMLVideoElement(this, video, descriptor, exception_state);
            }
        } else {
            external_texture = GPUExternalTexture::FromHTMLVideoElement(this, video, descriptor, exception_state);
        }

        // GPUExternalTexture imported from HTMLVideoElement should be expired
        // at the end of task.
        if (external_texture) {
            external_texture->Refresh();
            ExpireAtEndOfTask(external_texture);
        }
        break;
    }
    case V8UnionHTMLVideoElementOrVideoFrame::ContentType::kVideoFrame: {
        VideoFrame* frame = descriptor->source()->GetAsVideoFrame();
        // Do not revive a cached texture whose cross-thread close callback is
        // still pending on this sequence.
        if (!GetExternalTextureSourceFromVideoFrame(frame, exception_state).valid)
            return nullptr;

        auto cache = sources.from_video_frame.find(frame);
        if (cache != sources.from_video_frame.end()) {
            external_texture = cache->value;
        } else {
            external_texture = GPUExternalTexture::FromVideoFrame(this, frame, descriptor, exception_state);
        }
        break;
    }
    }

    return external_texture;
}

void ExternalTextureCache::Destroy()
{
    // Skip pending expiry tasks to destroy all pending external textures.
    expire_task_scheduled_ = false;

    for (auto& color_space : by_color_space_) {
        for (auto& source : color_space.value->from_html_video_element)
            source.value->Destroy();
        for (auto& source : color_space.value->from_video_frame)
            source.value->Destroy();
    }
    by_color_space_.clear();
    // Old pending expiry callbacks are inert once expire_task_scheduled_ is false.
    expire_set_.clear();
}

ExternalTextureCache::PerColorSpaceCache& ExternalTextureCache::GetOrCreateColorSpaceCache(PredefinedColorSpace color_space)
{
    auto result = by_color_space_.insert(color_space, nullptr);
    if (result.is_new_entry)
        result.stored_value->value = MakeGarbageCollected<PerColorSpaceCache>();
    return *result.stored_value->value;
}

void ExternalTextureCache::PerColorSpaceCache::Trace(Visitor* visitor) const
{
    visitor->Trace(from_html_video_element);
    visitor->Trace(from_video_frame);
}

void ExternalTextureCache::Add(HTMLVideoElement* video, GPUExternalTexture* external_texture)
{
    GetOrCreateColorSpaceCache(external_texture->ColorSpace()).from_html_video_element.Set(video, external_texture);
}

void ExternalTextureCache::Remove(HTMLVideoElement* video, GPUExternalTexture* external_texture)
{
    auto color_space = by_color_space_.find(external_texture->ColorSpace());
    if (color_space == by_color_space_.end())
        return;
    auto& sources = color_space->value->from_html_video_element;
    auto source = sources.find(video);
    if (source != sources.end() && source->value == external_texture)
        sources.erase(source);
}

void ExternalTextureCache::Add(VideoFrame* frame, GPUExternalTexture* external_texture)
{
    GetOrCreateColorSpaceCache(external_texture->ColorSpace()).from_video_frame.Set(frame, external_texture);
}

void ExternalTextureCache::Remove(VideoFrame* frame, GPUExternalTexture* external_texture)
{
    auto color_space = by_color_space_.find(external_texture->ColorSpace());
    if (color_space == by_color_space_.end())
        return;
    auto& sources = color_space->value->from_video_frame;
    auto source = sources.find(frame);
    if (source != sources.end() && source->value == external_texture)
        sources.erase(source);
}

void ExternalTextureCache::Trace(Visitor* visitor) const
{
    visitor->Trace(by_color_space_);
    visitor->Trace(expire_set_);
    visitor->Trace(device_);
}

GPUDevice* ExternalTextureCache::device() const
{
    return device_.Get();
}

void ExternalTextureCache::ExpireAtEndOfTask(GPUExternalTexture* external_texture)
{
    CHECK(external_texture);
    expire_set_.insert(external_texture);

    if (expire_task_scheduled_) {
        return;
    }

    device()
        ->GetExecutionContext()
        ->GetTaskRunner(TaskType::kWebGPU)
        ->PostTask(FROM_HERE, WTF::BindOnce(&ExternalTextureCache::ExpireTask, WrapWeakPersistent(this)));
    expire_task_scheduled_ = true;
}

void ExternalTextureCache::ExpireTask()
{
    // GPUDevice.destroy() call has destroyed all pending external textures.
    if (!expire_task_scheduled_) {
        return;
    }

    expire_task_scheduled_ = false;

    auto external_textures = std::move(expire_set_);
    for (auto& external_texture : external_textures) {
        external_texture->Expire();
    }
}


// static
GPUExternalTexture* GPUExternalTexture::CreateImpl(ExternalTextureCache* cache, const GPUExternalTextureDescriptor* webgpu_desc,
    scoped_refptr<media::VideoFrame> media_video_frame, media::PaintCanvasVideoRenderer* video_renderer,
    std::optional<media::VideoFrame::ID> media_video_frame_unique_id, ExceptionState& exception_state)
{
    CHECK(media_video_frame);

    PredefinedColorSpace dst_predefined_color_space;
    if (!ValidateAndConvertColorSpace(webgpu_desc->colorSpace(), dst_predefined_color_space, exception_state)) {
        return nullptr;
    }

    ExternalTexture external_texture = CreateExternalTexture(cache->device(), dst_predefined_color_space, media_video_frame, video_renderer);

    if (external_texture.wgpu_external_texture == nullptr || external_texture.texture == nullptr) {
        exception_state.ThrowDOMException(DOMExceptionCode::kOperationError, "Video frame is not CPU-readable, has an unsupported pixel format, or could not be uploaded");
        return nullptr;
    }

    GPUExternalTexture* gpu_external_texture
        = MakeGarbageCollected<GPUExternalTexture>(cache, std::move(external_texture.wgpu_external_texture), std::move(external_texture.texture),
            media_video_frame_unique_id, dst_predefined_color_space, webgpu_desc->label());

    return gpu_external_texture;
}

// static
GPUExternalTexture* GPUExternalTexture::CreateExpired(
    ExternalTextureCache* cache, const GPUExternalTextureDescriptor* webgpu_desc, ExceptionState& exception_state)
{
    // Validate GPUExternalTextureDescriptor.
    PredefinedColorSpace color_space;
    if (!ValidateAndConvertColorSpace(webgpu_desc->colorSpace(), color_space, exception_state))
        return nullptr;
    ExternalTextureSource source;
    switch (webgpu_desc->source()->GetContentType()) {
    case V8UnionHTMLVideoElementOrVideoFrame::ContentType::kHTMLVideoElement: {
        HTMLVideoElement* video = webgpu_desc->source()->GetAsHTMLVideoElement();
        source = GetExternalTextureSourceFromVideoElement(video, exception_state);
        break;
    }
    case V8UnionHTMLVideoElementOrVideoFrame::ContentType::kVideoFrame: {
        VideoFrame* frame = webgpu_desc->source()->GetAsVideoFrame();
        source = GetExternalTextureSourceFromVideoFrame(frame, exception_state);
        break;
    }
    }
    if (!source.valid)
        return nullptr;

    // Bypass importing video frame into Dawn.
    GPUExternalTexture* external_texture
        = MakeGarbageCollected<GPUExternalTexture>(cache, cache->device()->GetHandle().CreateErrorExternalTexture(), nullptr,
            std::nullopt, color_space, webgpu_desc->label());

    return external_texture;
}

// static
GPUExternalTexture* GPUExternalTexture::FromHTMLVideoElement(
    ExternalTextureCache* cache, HTMLVideoElement* video, const GPUExternalTextureDescriptor* webgpu_desc, ExceptionState& exception_state)
{
    ExternalTextureSource source = GetExternalTextureSourceFromVideoElement(video, exception_state);
    if (!source.valid)
        return nullptr;

    // Ensure that video playback remains unaffected by preventing any
    // throttling when the video is not visible on the screen.
    DCHECK(video);
    if (auto* wmp = video->GetWebMediaPlayer()) {
        wmp->RequestVideoFrameCallback();
    }

    GPUExternalTexture* external_texture = GPUExternalTexture::CreateImpl(
        cache, webgpu_desc, source.media_video_frame, source.video_renderer, source.media_video_frame_unique_id, exception_state);

    // WebGPU Spec requires that If the latest presented frame of video is not
    // the same frame from which texture was imported, set expired to true and
    // releasing ownership of the underlying resource and remove the texture from
    // active list. Listen to HTMLVideoElement and insert the texture into active
    // list for management.
    if (external_texture) {
        external_texture->SetVideo(video);
        cache->Add(video, external_texture);
    }

    return external_texture;
}

// static
GPUExternalTexture* GPUExternalTexture::FromVideoFrame(
    ExternalTextureCache* cache, VideoFrame* frame, const GPUExternalTextureDescriptor* webgpu_desc, ExceptionState& exception_state)
{
    ExternalTextureSource source = GetExternalTextureSourceFromVideoFrame(frame, exception_state);
    if (!source.valid)
        return nullptr;

    GPUExternalTexture* external_texture
        = GPUExternalTexture::CreateImpl(cache, webgpu_desc, source.media_video_frame, source.video_renderer, std::nullopt, exception_state);

    // If the webcodec video frame has been closed or destroyed, set expired to
    // true, releasing ownership of the underlying resource and remove the texture
    // from active list. Listen to the VideoFrame and insert the texture into
    // active list for management.
    if (external_texture) {
        if (!external_texture->ListenToVideoFrame(frame)) {
            return nullptr;
        }

        cache->Add(frame, external_texture);
    }

    return external_texture;
}

GPUExternalTexture::GPUExternalTexture(ExternalTextureCache* cache, wgpu::ExternalTexture external_texture, wgpu::Texture texture,
    std::optional<media::VideoFrame::ID> media_video_frame_unique_id, PredefinedColorSpace color_space, const String& label)
    : DawnObject<wgpu::ExternalTexture>(cache->device(), external_texture, label)
    , texture_(std::move(texture))
    , color_space_(color_space)
    , media_video_frame_unique_id_(media_video_frame_unique_id)
    , cache_(cache)
{
    task_runner_ = device()->GetExecutionContext()->GetTaskRunner(TaskType::kWebGPU);

    // Mark GPUExternalTexture without back resources as destroyed because no need
    // to do real resource releasing.
    if (!texture_)
        status_ = Status::Destroyed;
}

void GPUExternalTexture::Refresh()
{
    CHECK(status_ != Status::Destroyed);

    if (active()) {
        return;
    }

    GetHandle().Refresh();
    status_ = Status::Active;
}

void GPUExternalTexture::Expire()
{
    if (expired() || destroyed()) {
        return;
    }

    GetHandle().Expire();
    status_ = Status::Expired;
}

void GPUExternalTexture::Destroy()
{
    if (destroyed())
        return;
    Expire();
    status_ = Status::Destroyed;
    if (texture_) {
        texture_.Destroy();
        texture_ = nullptr;
    }
}

void GPUExternalTexture::SetVideo(HTMLVideoElement* video)
{
    CHECK(video);
    video_ = video;
}

bool GPUExternalTexture::NeedsToUpdate()
{
    CHECK(media_video_frame_unique_id_.has_value());
    CHECK(video_);

    if (IsCurrentFrameFromHTMLVideoElementValid()) {
        return false;
    }

    // Schedule source invalid task to remove GPUExternalTexture
    // from cache.
    OnSourceInvalidated();

    // If GPUExternalTexture is used in current task scope, don't do
    // reimport until current task scope finished.
    if (active()) {
        return false;
    }

    return true;
}

void GPUExternalTexture::Trace(Visitor* visitor) const
{
    visitor->Trace(frame_);
    visitor->Trace(video_);
    visitor->Trace(cache_);
    DawnObject<wgpu::ExternalTexture>::Trace(visitor);
}

bool GPUExternalTexture::IsCurrentFrameFromHTMLVideoElementValid()
{
    CHECK(video_);
    CHECK(media_video_frame_unique_id_.has_value());

    WebMediaPlayer* media_player = video_->GetWebMediaPlayer();

    // HTMLVideoElement transition from having a WMP to not having one.
    if (!media_player) {
        return false;
    }

    // VideoFrame unique id is unique in the same process. Compare the unique id
    // with current video frame from compositor to detect a new presented
    // video frame and expire the GPUExternalTexture.
    if (media_video_frame_unique_id_ != media_player->CurrentFrameId()) {
        return false;
    }

    return true;
}

void GPUExternalTexture::OnSourceInvalidated()
{
    CHECK(task_runner_);
    CHECK(task_runner_->BelongsToCurrentThread());

    // OnSourceInvalidated is called for both VideoFrame and HTMLVE.
    // VideoFrames are invalidated with and explicit close() call that
    // should mark the ExternalTexture destroyed immediately.
    // However HTMLVE could decide to advance in the middle of the task
    // that imported the ExternalTexture. In that case defer the invalidation
    // until the end of the task to preserve the semantic of ExternalTexture.
    if (status_ == Status::Active && video_) {
        if (!remove_from_cache_task_scheduled_) {
            task_runner_->PostTask(FROM_HERE, WTF::BindOnce(&GPUExternalTexture::RemoveFromCache, WrapWeakPersistent(this)));
        }
        remove_from_cache_task_scheduled_ = true;
    } else {
        RemoveFromCache();
    }
}

void GPUExternalTexture::RemoveFromCache()
{
    if (video_) {
        cache_->Remove(video_, this);
    } else if (frame_) {
        cache_->Remove(frame_, this);
    }

    Destroy();
}

bool GPUExternalTexture::ListenToVideoFrame(VideoFrame* frame)
{
    if (!frame->handle()->WebGPURegisterExternalTextureExpireCallback(
            CrossThreadBindOnce(&GPUExternalTexture::OnVideoFrameClosed, WrapCrossThreadWeakPersistent(this)))) {
        OnSourceInvalidated();
        return false;
    }

    frame_ = frame;
    return true;
}

void GPUExternalTexture::OnVideoFrameClosed()
{
    CHECK(task_runner_);

    if (destroyed())
        return;

    if (!task_runner_->BelongsToCurrentThread()) {
        // Only the atomic state may be touched on the source's closing thread.
        // The wire itself belongs to the creating Blink sequence.
        status_ = Status::Expired;
        task_runner_->PostTask(
            FROM_HERE, ConvertToBaseOnceCallback(CrossThreadBindOnce(&GPUExternalTexture::OnVideoFrameClosed, WrapCrossThreadWeakPersistent(this))));
        return;
    }
    GetHandle().Expire();
    status_ = Status::Expired;
    OnSourceInvalidated();
}

bool GPUExternalTexture::active() const
{
    return status_ == Status::Active;
}

bool GPUExternalTexture::expired() const
{
    return status_ == Status::Expired;
}

bool GPUExternalTexture::destroyed() const
{
    return status_ == Status::Destroyed;
}

} // namespace blink
