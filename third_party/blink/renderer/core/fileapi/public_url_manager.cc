/*
 * Copyright (C) 2012 Motorola Mobility Inc.
 * Copyright (C) 2013 Google Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/fileapi/public_url_manager.h"

#include <memory>

#include "base/notreached.h"
#include "base/types/pass_key.h"
#include "base/unguessable_token.h"
#if defined(MINI_UI_QUICKJS)
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "url/gurl.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/resource_request.h"
#include "base/task/thread_pool.h"
#include "services/network/public/mojom/url_loader.mojom-blink.h"
#include "services/network/public/mojom/url_loader_factory.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#endif
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/blob/blob_url_store.mojom-blink.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/mojom/use_counter/metrics/web_feature.mojom-blink.h"
#endif
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/fileapi/url_registry.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/workers/worker_global_scope.h"
#include "third_party/blink/renderer/core/workers/worklet_global_scope.h"
#endif
#include "third_party/blink/renderer/platform/blob/blob_url.h"
#include "third_party/blink/renderer/platform/blob/blob_url_null_origin_map.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/network/blink_schemeful_site.h"
#include "third_party/blink/renderer/platform/scheduler/main_thread/task_type_names.h"
#endif
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/platform/wtf/casting.h"
#endif
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

namespace {

static void RemoveFromNullOriginMapIfNecessary(const KURL& blob_url)
{
    DCHECK(blob_url.ProtocolIs("blob"));
    if (BlobURL::GetOrigin(blob_url) == "null")
        BlobURLNullOriginMap::GetInstance()->Remove(blob_url);
}

} // namespace
#if defined(MINI_UI_QUICKJS)
using NativeBlobURLMap =
    HashMap<String,
        std::unique_ptr<mojo::Remote<mojom::blink::Blob>>>;

static const scoped_refptr<base::SingleThreadTaskRunner>&
NativeBlobURLTaskRunner()
{
    static const scoped_refptr<base::SingleThreadTaskRunner> runner =
        base::ThreadPool::CreateSingleThreadTaskRunner(
            {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
    return runner;
}

NativeBlobURLMap& GetNativeBlobURLMap()
{
    DEFINE_STATIC_LOCAL(NativeBlobURLMap, urls, ());
    return urls;
}

class NativeBlobURLLoaderFactory final
    : public network::mojom::blink::URLLoaderFactory {
public:
    NativeBlobURLLoaderFactory(
        mojo::PendingRemote<mojom::blink::Blob> blob,
        GURL url)
        : blob_pending_(std::move(blob)),
          url_(std::move(url)) {}
    void CreateLoaderAndStart(
        mojo::PendingReceiver<network::mojom::blink::URLLoader> loader,
        int32_t, uint32_t, const network::ResourceRequest& request,
        mojo::PendingRemote<network::mojom::blink::URLLoaderClient> client,
        const net::MutableNetworkTrafficAnnotationTag&) override
    {
        if (request.url.GetWithoutRef() != url_) {
            mojo::Remote<network::mojom::blink::URLLoaderClient>(
                std::move(client))
                ->OnComplete(network::URLLoaderCompletionStatus(
                    net::ERR_INVALID_URL));
            return;
        }
        Blob()->Load(std::move(loader), String::FromUTF8(request.method),
            request.headers, std::move(client));
    }

    void Clone(
        mojo::PendingReceiver<network::mojom::blink::URLLoaderFactory>
            receiver) override
    {
        mojo::PendingRemote<mojom::blink::Blob> blob;
        Blob()->Clone(blob.InitWithNewPipeAndPassReceiver());
        mojo::MakeSelfOwnedReceiver(
            std::make_unique<NativeBlobURLLoaderFactory>(
                std::move(blob), url_),
            std::move(receiver));
    }

private:
    mojom::blink::Blob* Blob()
    {
        if (!blob_.is_bound())
            blob_.Bind(std::move(blob_pending_));
        return blob_.get();
    }
    mojo::PendingRemote<mojom::blink::Blob> blob_pending_;
    mojo::Remote<mojom::blink::Blob> blob_;
    const GURL url_;
};

class NativeBlobURLToken final : public mojom::blink::BlobURLToken {
public:
    explicit NativeBlobURLToken(
        mojo::PendingRemote<mojom::blink::Blob> blob)
        : blob_pending_(std::move(blob)),
          token_(base::UnguessableToken::Create()) {}
    NativeBlobURLToken(mojo::PendingRemote<mojom::blink::Blob> blob,
        const base::UnguessableToken& token)
        : blob_pending_(std::move(blob)), token_(token) {}

    void Clone(
        mojo::PendingReceiver<mojom::blink::BlobURLToken> receiver)
        override
    {
        mojo::PendingRemote<mojom::blink::Blob> blob;
        Blob()->Clone(blob.InitWithNewPipeAndPassReceiver());
        mojo::MakeSelfOwnedReceiver(
            std::make_unique<NativeBlobURLToken>(
                std::move(blob), token_),
            std::move(receiver));
    }
    void GetToken(GetTokenCallback callback) override
    {
        std::move(callback).Run(token_);
    }

private:
    mojom::blink::Blob* Blob()
    {
        if (!blob_.is_bound())
            blob_.Bind(std::move(blob_pending_));
        return blob_.get();
    }
    mojo::PendingRemote<mojom::blink::Blob> blob_pending_;
    mojo::Remote<mojom::blink::Blob> blob_;
    const base::UnguessableToken token_;
};
#endif

PublicURLManager::PublicURLManager(ExecutionContext* execution_context)
    : ExecutionContextLifecycleObserver(execution_context)
#if !defined(MINI_UI_QUICKJS)
    , frame_url_store_(execution_context)
    , worker_url_store_(execution_context)
#endif
{
#if defined(MINI_UI_QUICKJS)
    return;
#else
    if (auto* window = DynamicTo<LocalDOMWindow>(execution_context)) {
        LocalFrame* frame = window->GetFrame();
        if (!frame) {
            is_stopped_ = true;
            return;
        }

        frame->GetRemoteNavigationAssociatedInterfaces()->GetInterface(
            frame_url_store_.BindNewEndpointAndPassReceiver(execution_context->GetTaskRunner(TaskType::kFileReading)));

    } else if (auto* worker_global_scope = DynamicTo<WorkerGlobalScope>(execution_context)) {
        if (worker_global_scope->IsClosing()) {
            is_stopped_ = true;
            return;
        }

        worker_global_scope->GetBrowserInterfaceBroker().GetInterface(
            worker_url_store_.BindNewPipeAndPassReceiver(execution_context->GetTaskRunner(TaskType::kFileReading)));

    } else if (auto* worklet_global_scope = DynamicTo<WorkletGlobalScope>(execution_context)) {
        if (worklet_global_scope->IsClosing()) {
            is_stopped_ = true;
            return;
        }

        if (worklet_global_scope->IsMainThreadWorkletGlobalScope()) {
            LocalFrame* frame = worklet_global_scope->GetFrame();
            if (!frame) {
                is_stopped_ = true;
                return;
            }

            frame->GetRemoteNavigationAssociatedInterfaces()->GetInterface(
                frame_url_store_.BindNewEndpointAndPassReceiver(execution_context->GetTaskRunner(TaskType::kFileReading)));
        } else {
            // For threaded worklets we don't have a frame accessible here, so
            // instead we'll use a PendingRemote provided by the frame that created
            // this worklet.
            mojo::PendingRemote<mojom::blink::BlobURLStore> pending_remote = worklet_global_scope->TakeBlobUrlStorePendingRemote();
            DCHECK(pending_remote.is_valid());
            worker_url_store_.Bind(std::move(pending_remote), execution_context->GetTaskRunner(TaskType::kFileReading));
        }
    } else {
        NOTREACHED();
    }
#endif
}

PublicURLManager::PublicURLManager(
    base::PassKey<GlobalStorageAccessHandle>,
    ExecutionContext* execution_context,
    mojo::PendingAssociatedRemote<mojom::blink::BlobURLStore>
        frame_url_store_remote)
    : ExecutionContextLifecycleObserver(execution_context)
#if !defined(MINI_UI_QUICKJS)
    , frame_url_store_(execution_context)
    , worker_url_store_(execution_context)
#endif
{
#if !defined(MINI_UI_QUICKJS)
    frame_url_store_.Bind(std::move(frame_url_store_remote),
        execution_context->GetTaskRunner(TaskType::kFileReading));
#endif
}

#if !defined(MINI_UI_QUICKJS)
mojom::blink::BlobURLStore& PublicURLManager::GetBlobURLStore()
{
    DCHECK_NE(frame_url_store_.is_bound(), worker_url_store_.is_bound());
    if (frame_url_store_.is_bound()) {
        return *frame_url_store_.get();
    } else {
        return *worker_url_store_.get();
    }
}
#endif

String PublicURLManager::RegisterURL(URLRegistrable* registrable)
{
    if (is_stopped_)
        return String();

    const KURL& url = BlobURL::CreatePublicURL(GetExecutionContext()->GetSecurityOrigin());
    DCHECK(!url.IsEmpty());
    const String& url_string = url.GetString();

    if (registrable->IsMojoBlob()) {
        mojo::PendingRemote<mojom::blink::Blob> blob_remote;
        auto blob_receiver =
            blob_remote.InitWithNewPipeAndPassReceiver();
#if defined(MINI_UI_QUICKJS)
        auto remote =
            std::make_unique<mojo::Remote<mojom::blink::Blob>>(
                std::move(blob_remote));
        GetNativeBlobURLMap().Set(url_string, std::move(remote));
#else
        std::optional<BlinkSchemefulSite> top_level_site;
        if (GetExecutionContext()->IsWindow()) {
            auto* window =
                To<LocalDOMWindow>(GetExecutionContext());
            if (window->top() && window->top()->GetFrame()) {
                top_level_site = BlinkSchemefulSite(
                    window->top()->GetFrame()
                        ->GetSecurityContext()->GetSecurityOrigin());
            }
        }
        GetBlobURLStore().Register(std::move(blob_remote), url,
            GetExecutionContext()->GetAgentClusterID(),
            top_level_site);
#endif
        mojo_urls_.insert(url_string);
        registrable->CloneMojoBlob(std::move(blob_receiver));
    } else {
        URLRegistry* registry = &registrable->Registry();
        registry->RegisterURL(url, registrable);
        url_to_registry_.insert(url_string, registry);
    }

    SecurityOrigin* mutable_origin = GetExecutionContext()->GetMutableSecurityOrigin();
    if (mutable_origin->SerializesAsNull()) {
        BlobURLNullOriginMap::GetInstance()->Add(url, mutable_origin);
    }

    return url_string;
}

void PublicURLManager::Revoke(const KURL& url)
{
    if (is_stopped_)
        return;
    // Don't bother trying to revoke URLs that can't have been registered anyway.
    if (!url.ProtocolIs("blob") || url.HasFragmentIdentifier())
        return;
    // Don't support revoking cross-origin blob URLs.
    if (!SecurityOrigin::Create(url)->IsSameOriginWith(GetExecutionContext()->GetSecurityOrigin()))
        return;

#if defined(MINI_UI_QUICKJS)
    GetNativeBlobURLMap().erase(url.GetString());
#else
    GetBlobURLStore().Revoke(url);
#endif
    mojo_urls_.erase(url.GetString());

    RemoveFromNullOriginMapIfNecessary(url);
    auto it = url_to_registry_.find(url.GetString());
    if (it == url_to_registry_.end())
        return;
    it->value->UnregisterURL(url);
    url_to_registry_.erase(it);
}

void PublicURLManager::Resolve(const KURL& url, mojo::PendingReceiver<network::mojom::blink::URLLoaderFactory> factory_receiver)
{
    if (is_stopped_)
        return;

    DCHECK(url.ProtocolIs("blob"));
#if defined(MINI_UI_QUICKJS)
    if (!SecurityOrigin::Create(url)->IsSameOriginWith(
            GetExecutionContext()->GetSecurityOrigin()))
        return;
    KURL lookup_url = url;
    lookup_url.RemoveFragmentIdentifier();
    auto it = GetNativeBlobURLMap().find(lookup_url.GetString());
    if (it == GetNativeBlobURLMap().end())
        return;
    mojo::PendingRemote<mojom::blink::Blob> blob;
    (*it->value)->Clone(blob.InitWithNewPipeAndPassReceiver());
    NativeBlobURLTaskRunner()->PostTask(FROM_HERE, base::BindOnce(
        [](mojo::PendingRemote<mojom::blink::Blob> blob, GURL url,
            mojo::PendingReceiver<network::mojom::blink::URLLoaderFactory> receiver) {
            mojo::MakeSelfOwnedReceiver(
                std::make_unique<NativeBlobURLLoaderFactory>(
                    std::move(blob), std::move(url)),
                std::move(receiver));
        }, std::move(blob), GURL(lookup_url.GetString().Utf8()), std::move(factory_receiver)));
    return;
#else

    auto metrics_callback = [](ExecutionContext* execution_context, const std::optional<base::UnguessableToken>& unsafe_agent_cluster_id,
                                const std::optional<BlinkSchemefulSite>& unsafe_top_level_site) {
        if (execution_context->GetAgentClusterID() != unsafe_agent_cluster_id) {
            execution_context->CountUse(WebFeature::kBlobStoreAccessAcrossAgentClustersInResolveAsURLLoaderFactory);
        }
        // Determining top-level site in a worker is non-trivial. Since this is only
        // used to calculate metrics it should be okay to not track top-level site
        // in that case, as long as the count for unknown top-level sites ends up
        // low enough compared to overall usage.
        std::optional<BlinkSchemefulSite> top_level_site;
        if (execution_context->IsWindow()) {
            auto* window = To<LocalDOMWindow>(execution_context);
            if (window->top() && window->top()->GetFrame()) {
                top_level_site = BlinkSchemefulSite(window->top()->GetFrame()->GetSecurityContext()->GetSecurityOrigin());
            }
        }
        if ((!top_level_site || !unsafe_top_level_site) && execution_context->GetAgentClusterID() != unsafe_agent_cluster_id) {
            // Either the registration or resolve happened in a context where it's not
            // easy to determine the top-level site, and agent cluster doesn't match
            // either (if agent cluster matches, by definition top-level site would
            // also match, so this only records page loads where there is a chance
            // that top-level site doesn't match).
            execution_context->CountUse(WebFeature::kBlobStoreAccessUnknownTopLevelSite);
        } else if (top_level_site != unsafe_top_level_site) {
            // Blob URL lookup happened with a different top-level site than Blob URL
            // registration.
            execution_context->CountUse(WebFeature::kBlobStoreAccessAcrossTopLevelSite);
        }
    };

    GetBlobURLStore().ResolveAsURLLoaderFactory(url, std::move(factory_receiver), WTF::BindOnce(metrics_callback, WrapPersistent(GetExecutionContext())));
#endif
}

void PublicURLManager::Resolve(const KURL& url, mojo::PendingReceiver<mojom::blink::BlobURLToken> token_receiver)
{
    if (is_stopped_)
        return;

    DCHECK(url.ProtocolIs("blob"));
#if defined(MINI_UI_QUICKJS)
    if (!SecurityOrigin::Create(url)->IsSameOriginWith(
            GetExecutionContext()->GetSecurityOrigin()))
        return;
    KURL lookup_url = url;
    lookup_url.RemoveFragmentIdentifier();
    auto it = GetNativeBlobURLMap().find(lookup_url.GetString());
    if (it == GetNativeBlobURLMap().end())
        return;
    mojo::PendingRemote<mojom::blink::Blob> blob;
    (*it->value)->Clone(blob.InitWithNewPipeAndPassReceiver());
    NativeBlobURLTaskRunner()->PostTask(FROM_HERE, base::BindOnce(
        [](mojo::PendingRemote<mojom::blink::Blob> blob,
            mojo::PendingReceiver<mojom::blink::BlobURLToken> receiver) {
            mojo::MakeSelfOwnedReceiver(
                std::make_unique<NativeBlobURLToken>(std::move(blob)),
                std::move(receiver));
        }, std::move(blob), std::move(token_receiver)));
    return;
#else

    auto metrics_callback = [](ExecutionContext* execution_context, const std::optional<base::UnguessableToken>& unsafe_agent_cluster_id) {
        if (execution_context->GetAgentClusterID() != unsafe_agent_cluster_id) {
            execution_context->CountUse(WebFeature::kBlobStoreAccessAcrossAgentClustersInResolveForNavigation);
        }
    };

    GetBlobURLStore().ResolveForNavigation(url, std::move(token_receiver), WTF::BindOnce(metrics_callback, WrapPersistent(GetExecutionContext())));
#endif
}

void PublicURLManager::ResolveForWorkerScriptFetch(const KURL& url, mojo::PendingReceiver<mojom::blink::BlobURLToken> token_receiver)
{
    if (is_stopped_) {
        return;
    }

    DCHECK(url.ProtocolIs("blob"));
#if defined(MINI_UI_QUICKJS)
    Resolve(url, std::move(token_receiver));
    return;
#else

    // This code used to be executed as part of the ResolveForNavigation flow, so
    // it is repeated to preserve the existing functionality.
    auto metrics_callback = [](ExecutionContext* execution_context, const std::optional<base::UnguessableToken>& unsafe_agent_cluster_id) {
        if (execution_context->GetAgentClusterID() != unsafe_agent_cluster_id) {
            execution_context->CountUse(WebFeature::kBlobStoreAccessAcrossAgentClustersInResolveForNavigation);
        }
    };

    GetBlobURLStore().ResolveForWorkerScriptFetch(url, std::move(token_receiver), WTF::BindOnce(metrics_callback, WrapPersistent(GetExecutionContext())));
#endif
}

void PublicURLManager::ContextDestroyed()
{
    if (is_stopped_)
        return;

    is_stopped_ = true;
    for (auto& url_registry : url_to_registry_) {
        url_registry.value->UnregisterURL(KURL(url_registry.key));
        RemoveFromNullOriginMapIfNecessary(KURL(url_registry.key));
    }
    for (const auto& url : mojo_urls_)
        RemoveFromNullOriginMapIfNecessary(KURL(url));

    url_to_registry_.clear();
#if defined(MINI_UI_QUICKJS)
    for (const auto& url : mojo_urls_)
        GetNativeBlobURLMap().erase(url);
#endif
    mojo_urls_.clear();
}

void PublicURLManager::Trace(Visitor* visitor) const
{
#if !defined(MINI_UI_QUICKJS)
    visitor->Trace(frame_url_store_);
    visitor->Trace(worker_url_store_);
#endif
    ExecutionContextLifecycleObserver::Trace(visitor);
}

} // namespace blink
