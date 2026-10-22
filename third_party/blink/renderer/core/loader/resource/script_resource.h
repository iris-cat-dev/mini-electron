/*
    Copyright (C) 1998 Lars Knoll (knoll@mpi-hd.mpg.de)
    Copyright (C) 2001 Dirk Mueller <mueller@kde.org>
    Copyright (C) 2006 Samuel Weinig (sam.weinig@gmail.com)
    Copyright (C) 2004, 2005, 2006, 2007, 2008 Apple Inc. All rights reserved.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.

    This class provides all functionality needed for loading images, style
    sheets and html pages from the web. It has a memory cache for these objects.
*/

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_RESOURCE_SCRIPT_RESOURCE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_RESOURCE_SCRIPT_RESOURCE_H_

#if !defined(MINI_UI_QUICKJS)
#include "base/task/single_thread_task_runner.h"
#endif
#include "third_party/blink/public/mojom/script/script_type.mojom-blink-forward.h"
#include "third_party/blink/public/mojom/script/script_type.mojom-shared.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/bindings/core/v8/script_cache_consumer.h"
#include "third_party/blink/renderer/bindings/core/v8/script_streamer.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_compile_hints_common.h"
#endif
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/loader/resource/text_resource.h"
#include "third_party/blink/renderer/platform/bindings/parkable_string.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/platform/loader/fetch/integrity_metadata.h"
#endif
#include "third_party/blink/renderer/platform/loader/fetch/resource_client.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_loader_options.h"
#include "third_party/blink/renderer/platform/loader/fetch/text_resource_decoder_options.h"

namespace v8 {
class Isolate;
}

namespace blink {

#if !defined(MINI_UI_QUICKJS)
class CachedMetadataHandler;
class ScriptCachedMetadataHandler;
#endif
class FetchParameters;
class KURL;
class ResourceFetcher;

#if !defined(MINI_UI_QUICKJS)
namespace v8_compile_hints {
class V8CrowdsourcedCompileHintsConsumer;
class V8CrowdsourcedCompileHintsProducer;
} // namespace v8_compile_hints
#endif

// ScriptResource is a resource representing a JavaScript, either a classic or
// module script. Based on discussions (crbug.com/1178198) ScriptResources are
// shared between classic and module scripts.
//
// In addition to loading the script, a ScriptResource can optionally stream the
// script to the JavaScript parser/compiler, using a ScriptStreamer. In this
// case, clients of the ScriptResource will not receive the finished
// notification until the streaming completes.
// Note: ScriptStreamer is only used for "classic" scripts, i.e. not modules.
//
// See also:
// https://docs.google.com/document/d/143GOPl_XVgLPFfO-31b_MdBcnjklLEX2OIg_6eN6fQ4
class CORE_EXPORT ScriptResource final : public TextResource {
public:
#if defined(MINI_UI_QUICKJS)
    static ScriptResource* Fetch(
        FetchParameters&, ResourceFetcher*, ResourceClient*, v8::Isolate*);
#else
    // The script resource will always try to start streaming if kAllowStreaming
    // is passed in.
    enum StreamingAllowed { kNoStreaming, kAllowStreaming };
    static ScriptResource* Fetch(FetchParameters&, ResourceFetcher*, ResourceClient*, v8::Isolate*, StreamingAllowed,
        v8_compile_hints::V8CrowdsourcedCompileHintsProducer*, v8_compile_hints::V8CrowdsourcedCompileHintsConsumer*,
        v8_compile_hints::MagicCommentMode magic_comment_mode);
#endif

    // Public for testing
    static ScriptResource* CreateForTest(
        v8::Isolate* isolate, const KURL& url, const WTF::TextEncoding& encoding, mojom::blink::ScriptType = mojom::blink::ScriptType::kClassic);

#if defined(MINI_UI_QUICKJS)
    ScriptResource(
        const ResourceRequest&, const ResourceLoaderOptions&,
        const TextResourceDecoderOptions&, mojom::blink::ScriptType);
#else
    ScriptResource(const ResourceRequest&, const ResourceLoaderOptions&, const TextResourceDecoderOptions&, v8::Isolate*, StreamingAllowed,
        v8_compile_hints::V8CrowdsourcedCompileHintsProducer*, v8_compile_hints::V8CrowdsourcedCompileHintsConsumer*,
        v8_compile_hints::MagicCommentMode magic_comment_mode, mojom::blink::ScriptType);
#endif
    ~ScriptResource() override;

#if !defined(MINI_UI_QUICKJS)
    size_t CodeCacheSize() const override;
    void ResponseReceived(const ResourceResponse&) override;
    void ResponseBodyReceived(ResponseBodyLoaderDrainableInterface& body_loader, scoped_refptr<base::SingleThreadTaskRunner> loader_task_runner) override;
    void DidReceiveDecodedData(const String& data, std::unique_ptr<ParkableStringImpl::SecureDigest> digest) override;
#endif

    void Trace(Visitor*) const override;

    void OnMemoryDump(WebMemoryDumpLevelOfDetail, WebProcessMemoryDump*) const override;

#if !defined(MINI_UI_QUICKJS)
    void SetSerializedCachedMetadata(mojo_base::BigBuffer data) override;
#endif

    const ParkableString& SourceText();

    // Get the resource's current text. This can return partial data, so should
    // not be used outside of the inspector.
    String TextForInspector() const;

#if !defined(MINI_UI_QUICKJS)
    CachedMetadataHandler* CacheHandler();
#endif

    mojom::blink::ScriptType GetInitialRequestScriptType() const
    {
        return initial_request_script_type_;
    }

#if !defined(MINI_UI_QUICKJS)
    // Gets the script streamer from the ScriptResource, clearing the resource's
    // streamer so that it cannot be used twice.
    ScriptStreamer* TakeStreamer();

    ScriptStreamer::NotStreamingReason NoStreamerReason() const
    {
        return no_streamer_reason_;
    }

    // Used in DCHECKs
    bool HasStreamer()
    {
        return !!streamer_;
    }
    bool HasRunningStreamer()
    {
        return streamer_ && streamer_->IsStreamingStarted() && !streamer_->IsFinished();
    }
    bool HasFinishedStreamer()
    {
        return streamer_ && streamer_->IsFinished();
    }

    // Gets the cache consumer from the ScriptResource, clearing it from the
    // resource so that it cannot be used twice.
    //
    // It's fine to return a non-null ScriptCacheConsumer for one user of
    // ScriptResource while returning null for others, as the ScriptCacheConsumer
    // is associated with individual ScriptResource users and not with the
    // ScriptResource itself.
    ScriptCacheConsumer* TakeCacheConsumer();

    v8_compile_hints::V8CrowdsourcedCompileHintsProducer* GetV8CrowdsourcedCompileHintsProducer() const
    {
        return v8_compile_hints_producer_.Get();
    }

    v8_compile_hints::V8CrowdsourcedCompileHintsConsumer* GetV8CrowdsourcedCompileHintsConsumer() const
    {
        return v8_compile_hints_consumer_.Get();
    }

    v8_compile_hints::MagicCommentMode GetV8CompileHintsMagicCommentMode() const
    {
        return magic_comment_mode_;
    }

    // Returns the Isolate if set. This may be null.
    v8::Isolate* GetIsolateOrNull()
    {
        return isolate_if_main_thread_;
    }

    std::unique_ptr<BackgroundResponseProcessorFactory> MaybeCreateBackgroundResponseProcessorFactory() override;
#endif
    // Visible for tests.
    void SetRevalidatingRequest(const ResourceRequestHead&) override;

protected:
#if !defined(MINI_UI_QUICKJS)
    void DestroyDecodedDataIfPossible() override;
#endif
    void DestroyDecodedDataForFailedRevalidation() override;

#if !defined(MINI_UI_QUICKJS)
    // ScriptResources are considered finished when either loading and streaming
    // complete, or loading completes after streaming was disabled.
    void NotifyFinished() override;
    void SetEncoding(const String& chs) override;
#endif

private:
#if !defined(MINI_UI_QUICKJS)
    // Valid state transitions:
    //
    //            kWaitingForDataPipe          DisableStreaming()
    //                    |---------------------------.
    //                    |                           |
    //                    v                           v
    //               kStreaming -----------------> kStreamingDisabled
    //
    enum class StreamingState {
        // Streaming is allowed on this resource, but we're waiting to receive a
        // data pipe.
        kWaitingForDataPipe,
        // The script streamer is active, and has the data pipe.
        kStreaming,

        // Streaming was disabled, either manually or because we got a body with
        // no data-pipe.
        kStreamingDisabled,
    };

    // Valid state transitions:
    //
    //            kWaitingForCache              DisableOffThreadConsumeCache()
    //                    |---------------------------.
    //                    |                           |
    //                    v                           v
    //            kRunningOffThread ----------> kOffThreadConsumeCacheDisabled
    //
    enum class ConsumeCacheState {
        // No cached data has been received.
        kWaitingForCache,
        // Cache is being consumed off-thread.
        kRunningOffThread,
        // Off-thread consume was disabled, either because it wasn't possible,
        // wasn't allowed, or had completed and the consumer has already been taken.
        kOffThreadConsumeCacheDisabled,
    };

    class ScriptResourceFactory : public ResourceFactory {
    public:
        explicit ScriptResourceFactory(v8::Isolate* isolate, StreamingAllowed streaming_allowed,
            v8_compile_hints::V8CrowdsourcedCompileHintsProducer* v8_compile_hints_producer,
            v8_compile_hints::V8CrowdsourcedCompileHintsConsumer* v8_compile_hints_consumer, v8_compile_hints::MagicCommentMode magic_comment_mode,
            mojom::blink::ScriptType initial_request_script_type)
            : ResourceFactory(ResourceType::kScript, TextResourceDecoderOptions::kPlainTextContent)
            , isolate_(isolate)
            , streaming_allowed_(streaming_allowed)
            , v8_compile_hints_producer_(v8_compile_hints_producer)
            , v8_compile_hints_consumer_(v8_compile_hints_consumer)
            , magic_comment_mode_(magic_comment_mode)
            , initial_request_script_type_(initial_request_script_type)
        {
        }

        Resource* Create(const ResourceRequest& request, const ResourceLoaderOptions& options, const TextResourceDecoderOptions& decoder_options) const override
        {
            return MakeGarbageCollected<ScriptResource>(request, options, decoder_options, isolate_, streaming_allowed_, v8_compile_hints_producer_,
                v8_compile_hints_consumer_, magic_comment_mode_, initial_request_script_type_);
        }

    private:
        v8::Isolate* isolate_;
        StreamingAllowed streaming_allowed_;
        v8_compile_hints::V8CrowdsourcedCompileHintsProducer* v8_compile_hints_producer_;
        v8_compile_hints::V8CrowdsourcedCompileHintsConsumer* v8_compile_hints_consumer_;
        // For transmitting the status of the runtime enabled feature to script
        // streaming, which can access the ScriptResource but not the
        // ExecutionContext.
        // TODO(42203853): Remove this once explicit compile hints have launched and
        // the feature is always on.
        v8_compile_hints::MagicCommentMode magic_comment_mode_;
        mojom::blink::ScriptType initial_request_script_type_;
    };


    void DisableStreaming(ScriptStreamer::NotStreamingReason no_streamer_reason);

    void AdvanceStreamingState(StreamingState new_state);

    // Check that invariants for the state hold.
    void CheckStreamingState() const;

    void DisableOffThreadConsumeCache();

    void AdvanceConsumeCacheState(ConsumeCacheState new_state);

    // Check that invariants for the state hold.
    void CheckConsumeCacheState() const;
#else
    class ScriptResourceFactory : public ResourceFactory {
    public:
        explicit ScriptResourceFactory(
            mojom::blink::ScriptType initial_request_script_type)
            : ResourceFactory(
                  ResourceType::kScript,
                  TextResourceDecoderOptions::kPlainTextContent)
            , initial_request_script_type_(initial_request_script_type)
        {
        }

        Resource* Create(
            const ResourceRequest& request,
            const ResourceLoaderOptions& options,
            const TextResourceDecoderOptions& decoder_options) const override
        {
            return MakeGarbageCollected<ScriptResource>(
                request, options, decoder_options,
                initial_request_script_type_);
        }

    private:
        mojom::blink::ScriptType initial_request_script_type_;
    };
#endif

    bool CanUseCacheValidator() const override;

    ParkableString source_text_;

#if !defined(MINI_UI_QUICKJS)
    // This isolate is retained only on the main thread for streaming/cache work.
    v8::Isolate* isolate_if_main_thread_;
    Member<ResourceScriptStreamer> streamer_;
    ScriptStreamer::NotStreamingReason no_streamer_reason_ = ScriptStreamer::NotStreamingReason::kInvalid;
    StreamingState streaming_state_ = StreamingState::kWaitingForDataPipe;
    Member<ScriptCachedMetadataHandler> cached_metadata_handler_;
    Member<ScriptCacheConsumer> cache_consumer_;
    ConsumeCacheState consume_cache_state_;
#endif
    const mojom::blink::ScriptType initial_request_script_type_;
#if !defined(MINI_UI_QUICKJS)
    std::unique_ptr<TextResourceDecoder> stream_text_decoder_;
    Member<v8_compile_hints::V8CrowdsourcedCompileHintsProducer> v8_compile_hints_producer_;
    Member<v8_compile_hints::V8CrowdsourcedCompileHintsConsumer> v8_compile_hints_consumer_;
    v8_compile_hints::MagicCommentMode magic_comment_mode_;
    Member<BackgroundResourceScriptStreamer> background_streamer_;
#endif
};

template <> struct DowncastTraits<ScriptResource> {
    static bool AllowFrom(const Resource& resource)
    {
        return resource.GetType() == ResourceType::kScript;
    }
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_RESOURCE_SCRIPT_RESOURCE_H_
