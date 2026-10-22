// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_BLINK_LOCAL_URL_LOADER_H_
#define MINI_UI_RUNTIME_UI_BLINK_LOCAL_URL_LOADER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "third_party/blink/public/platform/web_url.h"

#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"

namespace mini_ui {

class LocalResources;

// Blink's resource loader adapter for the library's local-only resource store.
// The adapter deliberately implements URLLoader itself so no network service,
// URLLoaderFactory, socket, TLS, or service-worker path enters the UI graph.
class BlinkLocalURLLoader final : public blink::URLLoader {
public:
    BlinkLocalURLLoader(std::shared_ptr<LocalResources>, std::string base_url);
    ~BlinkLocalURLLoader() override;

    void LoadSynchronously(std::unique_ptr<network::ResourceRequest>,
        scoped_refptr<const blink::SecurityOrigin> top_frame_origin,
        bool download_to_blob, bool no_mime_sniffing,
        base::TimeDelta timeout_interval, blink::URLLoaderClient*,
        blink::WebURLResponse&, std::optional<blink::WebURLError>&,
        scoped_refptr<WTF::SharedBuffer>&, int64_t& encoded_data_length,
        uint64_t& encoded_body_length,
        scoped_refptr<blink::BlobDataHandle>& downloaded_blob,
        std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>) override;

    void LoadAsynchronously(std::unique_ptr<network::ResourceRequest>,
        scoped_refptr<const blink::SecurityOrigin> top_frame_origin,
        bool no_mime_sniffing,
        std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>,
        blink::URLLoaderClient*) override;

    void Freeze(blink::LoaderFreezeMode) override;
    void DidChangePriority(blink::WebURLRequest::Priority, int) override;
    scoped_refptr<base::SingleThreadTaskRunner> GetTaskRunnerForBodyLoader() override;

private:
    void DeliverAsync(std::unique_ptr<network::ResourceRequest>,
        blink::URLLoaderClient*);
    void PumpBody(MojoResult, const mojo::HandleSignalsState&);
    void FinishBody(MojoResult);

    std::shared_ptr<LocalResources> resources_;
    std::string base_url_;
    blink::LoaderFreezeMode freeze_mode_ = blink::LoaderFreezeMode::kNone;
    std::unique_ptr<network::ResourceRequest> pending_request_;
    blink::URLLoaderClient* pending_client_ = nullptr;
    std::vector<std::uint8_t> body_bytes_;
    blink::WebURL body_request_url_;
    std::size_t body_offset_ = 0;
    blink::URLLoaderClient* body_client_ = nullptr;
    mojo::ScopedDataPipeProducerHandle body_producer_;
    mojo::SimpleWatcher body_watcher_{FROM_HERE,
        mojo::SimpleWatcher::ArmingPolicy::MANUAL};
    base::WeakPtrFactory<BlinkLocalURLLoader> weak_factory_ { this };
};

} // namespace mini_ui

#endif // MINI_UI_RUNTIME_UI_BLINK_LOCAL_URL_LOADER_H_
