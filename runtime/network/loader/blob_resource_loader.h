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

#ifndef MINI_ELECTRON_NETWORK_LOADER_BLOB_RESOURCE_LOADER_H_
#define MINI_ELECTRON_NETWORK_LOADER_BLOB_RESOURCE_LOADER_H_

#include "url/gurl.h"
#include "base/memory/weak_ptr.h"
#include "base/functional/callback.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include <string>
#include <memory>

namespace blink {
class WebURLRequest;
class WebURLResponse;
class URLLoaderClient;
class WebURLLoader;
struct WebURLError;
}

namespace mini_electron {

class BlobResourceLoader : public blink::mojom::blink::BlobReaderClient,
                           public mojo::DataPipeDrainer::Client {
public:
    static BlobResourceLoader* createAsync(int64_t mbwebviewId, const GURL& url, blink::URLLoaderClient* client);
    static void loadSynchronously(int64_t mbwebviewId, const GURL& url, blink::URLLoaderClient* client);
    void start();
    ~BlobResourceLoader() override;

private:
    BlobResourceLoader(int64_t mbwebviewId, const GURL& url, blink::URLLoaderClient* client, bool async);
    void doStart();
    void didGetUUID(const WTF::String& uuid);
    void failed(int errorCode);
    void finishIfReady();
    void OnCalculatedSize(uint64_t totalSize, uint64_t expectedSize) override;
    void OnComplete(int32_t status, uint64_t dataLength) override;
    void OnDataAvailable(base::span<const uint8_t> data) override;
    void OnDataComplete() override;

    int64_t m_webviewId;
    blink::URLLoaderClient* m_client;
    GURL m_url;
    bool m_async;
    bool m_receivedResponse = false;
    bool m_blobComplete = false;
    bool m_pipeComplete = false;
    uint64_t m_expectedSize = 0;
    uint64_t m_receivedSize = 0;
    std::string m_contentType;
    base::OnceClosure m_syncQuit;
    mojo::Remote<blink::mojom::blink::Blob> m_blob;
    mojo::Receiver<blink::mojom::blink::BlobReaderClient> m_receiver { this };
    mojo::ScopedDataPipeConsumerHandle m_consumer;
    std::unique_ptr<mojo::DataPipeDrainer> m_drainer;
    base::WeakPtrFactory<BlobResourceLoader> m_weakFactory { this };
};

} // namespace WebCore

#endif // MINI_ELECTRON_NETWORK_LOADER_BLOB_RESOURCE_LOADER_H_
