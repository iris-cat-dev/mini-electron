/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "runtime/network/loader/blob_resource_loader.h"

#include "runtime/engine/renderer/blob_registry_impl.h"
#include "runtime/engine/renderer/blob_url_store_impl.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/browser/web_view_host.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "net/base/net_errors.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include <limits>
#include <utility>

namespace mini_electron {

BlobResourceLoader* BlobResourceLoader::createAsync(int64_t mbwebviewId, const GURL& url, blink::URLLoaderClient* client)
{
    BlobResourceLoader* ret = new BlobResourceLoader(mbwebviewId, url, client, true);
    return ret;
}

void BlobResourceLoader::loadSynchronously(int64_t webviewId, const GURL& url, blink::URLLoaderClient* client)
{
    base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
    BlobResourceLoader loader(webviewId, url, client, false);
    loader.m_syncQuit = loop.QuitClosure();
    loader.start();
    loop.Run();
}

BlobResourceLoader::BlobResourceLoader(int64_t webviewId, const GURL& url, blink::URLLoaderClient* client, bool async)
    : m_webviewId(webviewId), m_client(client), m_url(url), m_async(async)
{
}

BlobResourceLoader::~BlobResourceLoader() = default;

void BlobResourceLoader::start()
{
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&BlobResourceLoader::doStart, m_weakFactory.GetWeakPtr()));
}

void BlobResourceLoader::doStart()
{
    if (m_webviewId && !common::LiveIdDetect::getWebViewIds()->getPtr(m_webviewId)) {
        failed(net::ERR_ABORTED);
        return;
    }
    auto* blob = content::BlobURLStoreSet::get()->getBlobByUrl(m_url.possibly_invalid_spec());
    auto* blobEndpoint = blob ? blob->TryGet() : nullptr;
    if (!blobEndpoint) {
        failed(net::ERR_FILE_NOT_FOUND);
        return;
    }
    // Hold our own remote: revoking the URL must not invalidate an active load.
    blobEndpoint->Clone(m_blob.BindNewPipeAndPassReceiver());
    m_blob.set_disconnect_handler(base::BindOnce(&BlobResourceLoader::failed,
        m_weakFactory.GetWeakPtr(), net::ERR_FAILED));
    auto* clonedEndpoint = m_blob.TryGet();
    if (!clonedEndpoint) {
        failed(net::ERR_FAILED);
        return;
    }
    clonedEndpoint->GetInternalUUID(base::BindOnce(&BlobResourceLoader::didGetUUID,
        m_weakFactory.GetWeakPtr()));
}

void BlobResourceLoader::didGetUUID(const WTF::String& uuid)
{
    content::BlobEntry* entry = content::BlobEntry::findByUuid(uuid.Utf8());
    if (!entry) {
        failed(net::ERR_FILE_NOT_FOUND);
        return;
    }
    auto* blobEndpoint = m_blob.TryGet();
    if (!blobEndpoint) {
        failed(net::ERR_FAILED);
        return;
    }
    m_contentType = entry->m_contentType;
    mojo::ScopedDataPipeProducerHandle producer;
    if (mojo::CreateDataPipe(nullptr, producer, m_consumer) != MOJO_RESULT_OK) {
        failed(net::ERR_INSUFFICIENT_RESOURCES);
        return;
    }
    auto client = m_receiver.BindNewPipeAndPassRemote();
    m_receiver.set_disconnect_handler(base::BindOnce(&BlobResourceLoader::failed,
        m_weakFactory.GetWeakPtr(), net::ERR_FAILED));
    blobEndpoint->ReadAll(std::move(producer), std::move(client));
}

void BlobResourceLoader::OnCalculatedSize(uint64_t totalSize, uint64_t expectedSize)
{
    if (!m_client)
        return;
    if (m_receivedResponse || expectedSize > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        failed(net::ERR_FAILED);
        return;
    }
    m_receivedResponse = true;
    m_expectedSize = expectedSize;
    blink::WebURLResponse response;
    response.SetCurrentRequestUrl(blink::WebURL(blink::KURL(m_url)));
    response.SetMimeType(blink::WebString::FromUTF8(m_contentType));
    response.SetExpectedContentLength(expectedSize);
    response.SetHttpStatusCode(200);
    response.SetHttpStatusText(blink::WebString::FromUTF8("OK"));
    if (m_async) {
        m_client->DidReceiveResponse(response, std::move(m_consumer), std::nullopt);
        return;
    }
    auto weak = m_weakFactory.GetWeakPtr();
    m_client->DidReceiveResponse(response, WTF::SegmentedBuffer(), std::nullopt);
    if (weak)
        m_drainer = std::make_unique<mojo::DataPipeDrainer>(this, std::move(m_consumer));
}

void BlobResourceLoader::OnDataAvailable(base::span<const uint8_t> data)
{
    if (!m_client)
        return;
    if (data.size() > m_expectedSize - m_receivedSize) {
        failed(net::ERR_FAILED);
        return;
    }
    m_receivedSize += data.size();
    m_client->DidReceiveDataForTesting(base::span<const char>(
        reinterpret_cast<const char*>(data.data()), data.size()));
}

void BlobResourceLoader::OnDataComplete()
{
    m_pipeComplete = true;
    finishIfReady();
}

void BlobResourceLoader::OnComplete(int32_t status, uint64_t dataLength)
{
    if (!m_client)
        return;
    if (status != net::OK || !m_receivedResponse || dataLength != m_expectedSize) {
        failed(status != net::OK ? status : net::ERR_FAILED);
        return;
    }
    m_blobComplete = true;
    // Completion can arrive before the synchronous drainer observes EOF.
    m_receiver.reset();
    m_blob.reset();
    finishIfReady();
}

void BlobResourceLoader::finishIfReady()
{
    if (!m_client || !m_blobComplete || (!m_async && !m_pipeComplete))
        return;
    if (!m_async && m_receivedSize != m_expectedSize) {
        failed(net::ERR_FAILED);
        return;
    }
    auto* client = std::exchange(m_client, nullptr);
    if (m_syncQuit)
        std::move(m_syncQuit).Run();
    client->DidFinishLoading(base::TimeTicks::Now(), m_expectedSize, m_expectedSize, m_expectedSize);
}

void BlobResourceLoader::failed(int errorCode)
{
    if (!m_client)
        return;
    m_weakFactory.InvalidateWeakPtrs();
    m_receiver.reset();
    m_blob.reset();
    m_consumer.reset();
    auto* client = std::exchange(m_client, nullptr);
    if (m_syncQuit)
        std::move(m_syncQuit).Run();
    client->DidFail(blink::WebURLError(errorCode, blink::KURL(m_url)), base::TimeTicks::Now(), 0, 0, 0);
}

} // namespace net
