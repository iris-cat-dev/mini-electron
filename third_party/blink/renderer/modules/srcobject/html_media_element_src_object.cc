// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/srcobject/html_media_element_src_object.h"

#include "base/check.h"
#include "third_party/abseil-cpp/absl/types/variant.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_typedefs.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_union_mediasourcehandle_mediastream.h"
#include "third_party/blink/renderer/core/html/media/html_media_element.h"
#include "third_party/blink/renderer/core/html/media/media_source_handle.h"
#include "third_party/blink/renderer/modules/mediasource/media_source_handle_impl.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_descriptor.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"

namespace blink {

// static
V8MediaProvider* HTMLMediaElementSrcObject::srcObject(HTMLMediaElement& element)
{
    const auto src_object_variant = element.GetSrcObjectVariant();
#if defined(MINI_ELECTRON_IMPLEMENTATION)
    // The selected embedder cannot attach a MediaStream or MediaSourceHandle.
    // All native writes go through setSrcObject below; null is real element
    // state, not an invented replacement for an attached media provider.
    const auto* descriptor = absl::get_if<MediaStreamDescriptor*>(&src_object_variant);
    CHECK(descriptor && !*descriptor);
    return nullptr;
#else
    if (auto* const* handle = absl::get_if<MediaSourceHandle*>(&src_object_variant)) {
        DCHECK(*handle);
        return MakeGarbageCollected<V8MediaProvider>(static_cast<MediaSourceHandleImpl*>(*handle));
    }
    auto* descriptor = absl::get<MediaStreamDescriptor*>(src_object_variant);
    return descriptor ? MakeGarbageCollected<V8MediaProvider>(ToMediaStream(descriptor)) : nullptr;
#endif
}

// static
void HTMLMediaElementSrcObject::setSrcObject(HTMLMediaElement& element, V8MediaProvider* media_provider, ExceptionState& exception_state)
{
    if (!media_provider) {
        element.SetSrcObjectVariant(HTMLMediaElement::SrcObjectVariant());
        return;
    }
#if defined(MINI_ELECTRON_IMPLEMENTATION)
    exception_state.ThrowDOMException(DOMExceptionCode::kNotSupportedError, "Media stream playback is not supported by this build");
#else
    switch (media_provider->GetContentType()) {
    case V8MediaProvider::ContentType::kMediaSourceHandle: {
        auto* handle = media_provider->GetAsMediaSourceHandle();
        DCHECK(handle);
        handle->mark_used();
        element.SetSrcObjectVariant(handle);
        break;
    }
    case V8MediaProvider::ContentType::kMediaStream: {
        auto* stream = media_provider->GetAsMediaStream();
        DCHECK(stream);
        element.SetSrcObjectVariant(stream->Descriptor());
        break;
    }
    }
#endif
}

} // namespace blink
