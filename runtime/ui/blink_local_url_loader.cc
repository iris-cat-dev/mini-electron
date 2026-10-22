// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/blink_local_url_loader.h"

#include <cstdint>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "net/base/net_errors.h"
#include "runtime/ui/local_resources.h"
#include "services/network/public/cpp/resource_request.h"
#include "third_party/blink/public/platform/resource_load_info_notifier_wrapper.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "third_party/blink/renderer/platform/network/http_names.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/shared_buffer.h"

namespace mini_ui {
namespace {

blink::WebURL ToWebURL(std::string_view value)
{
    return blink::WebURL(blink::KURL(WTF::String::FromUTF8(value)));
}

int NetErrorFor(const Error& error)
{
    switch (error.code) {
    case ErrorCode::resource_denied:
        return net::ERR_ACCESS_DENIED;
    case ErrorCode::not_found:
        return net::ERR_FILE_NOT_FOUND;
    case ErrorCode::io:
        return net::ERR_FAILED;
    case ErrorCode::invalid_argument:
        return net::ERR_INVALID_ARGUMENT;
    default:
        return net::ERR_FAILED;
    }
}

blink::WebURLResponse MakeResponse(const Resource& resource)
{
    blink::WebURLResponse response(ToWebURL(resource.url));
    response.SetHttpStatusCode(200);
    const auto mime_type = blink::WebString::FromUTF8(resource.mime_type);
    response.SetMimeType(mime_type);
    response.SetHttpHeaderField(blink::http_names::kContentType, mime_type);
    response.SetExpectedContentLength(static_cast<int64_t>(resource.bytes.size()));
    if (resource.mime_type.rfind("text/", 0) == 0
        || resource.mime_type == "application/javascript"
        || resource.mime_type == "application/json"
        || resource.mime_type == "image/svg+xml") {
        response.SetTextEncodingName(blink::WebString::FromUTF8("UTF-8"));
    }
    return response;
}

} // namespace

BlinkLocalURLLoader::BlinkLocalURLLoader(
    std::shared_ptr<LocalResources> resources, std::string base_url)
    : resources_(std::move(resources))
    , base_url_(std::move(base_url))
{
}

BlinkLocalURLLoader::~BlinkLocalURLLoader() = default;

void BlinkLocalURLLoader::LoadSynchronously(
    std::unique_ptr<network::ResourceRequest> request,
    scoped_refptr<const blink::SecurityOrigin>, bool download_to_blob,
    bool, base::TimeDelta, blink::URLLoaderClient*,
    blink::WebURLResponse& response, std::optional<blink::WebURLError>& error,
    scoped_refptr<WTF::SharedBuffer>& data, int64_t& encoded_data_length,
    uint64_t& encoded_body_length,
    scoped_refptr<blink::BlobDataHandle>& downloaded_blob,
    std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>)
{
    const blink::WebURL request_url = ToWebURL(request->url.possibly_invalid_spec());

    Resource resource;
    Error local_error;
    if (!resources_->Read(request->url.possibly_invalid_spec(), base_url_,
            &resource, &local_error)) {
        error.emplace(NetErrorFor(local_error), request_url);
        return;
    }

    response = MakeResponse(resource);
    encoded_data_length = static_cast<int64_t>(resource.bytes.size());
    encoded_body_length = resource.bytes.size();
    if (download_to_blob) {
        auto blob_data = std::make_unique<blink::BlobData>();
        blob_data->SetContentType(
            WTF::String::FromUTF8(resource.mime_type));
        blob_data->AppendBytes(base::span(resource.bytes));
        downloaded_blob = blink::BlobDataHandle::Create(
            std::move(blob_data), resource.bytes.size());
        data = nullptr;
    } else {
        data = WTF::SharedBuffer::Create(base::span(resource.bytes));
        downloaded_blob = nullptr;
    }
}

void BlinkLocalURLLoader::LoadAsynchronously(
    std::unique_ptr<network::ResourceRequest> request,
    scoped_refptr<const blink::SecurityOrigin>, bool,
    std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>,
    blink::URLLoaderClient* client)
{
    if (freeze_mode_ != blink::LoaderFreezeMode::kNone) {
        pending_request_ = std::move(request);
        pending_client_ = client;
        return;
    }
    DeliverAsync(std::move(request), client);
}

void BlinkLocalURLLoader::DeliverAsync(
    std::unique_ptr<network::ResourceRequest> request,
    blink::URLLoaderClient* client)
{
    const blink::WebURL request_url = ToWebURL(request->url.possibly_invalid_spec());
    Resource resource;
    Error local_error;
    if (!resources_->Read(request->url.possibly_invalid_spec(), base_url_,
            &resource, &local_error)) {
        client->DidFail(blink::WebURLError(NetErrorFor(local_error), request_url),
            base::TimeTicks::Now(), 0, 0, 0);
        return;
    }

    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
        client->DidFail(blink::WebURLError(net::ERR_INSUFFICIENT_RESOURCES, request_url),
            base::TimeTicks::Now(), 0, 0, 0);
        return;
    }
    blink::WebURLResponse response = MakeResponse(resource);
    body_bytes_ = std::move(resource.bytes);
    body_request_url_ = request_url;
    body_client_ = client;
    body_offset_ = 0;
    body_producer_ = std::move(producer);
    base::WeakPtr<BlinkLocalURLLoader> alive = weak_factory_.GetWeakPtr();
    const MojoResult watched = body_watcher_.Watch(body_producer_.get(),
        MOJO_HANDLE_SIGNAL_WRITABLE, MOJO_WATCH_CONDITION_SATISFIED,
        base::BindRepeating(&BlinkLocalURLLoader::PumpBody, alive));
    if (watched != MOJO_RESULT_OK) {
        FinishBody(watched);
        return;
    }
    // SegmentedBuffer denotes background-processed data, not an ordinary body.
    // Use the real byte-stream path for every local resource, including images.
    client->DidReceiveResponse(response, std::move(consumer), std::nullopt);
    if (alive)
        body_watcher_.ArmOrNotify();
}

void BlinkLocalURLLoader::PumpBody(MojoResult result, const mojo::HandleSignalsState&)
{
    if (result != MOJO_RESULT_OK) {
        FinishBody(result);
        return;
    }
    while (body_offset_ < body_bytes_.size()) {
        size_t written = 0;
        result = body_producer_->WriteData(base::span(body_bytes_).subspan(body_offset_),
            MOJO_WRITE_DATA_FLAG_NONE, written);
        if (result == MOJO_RESULT_SHOULD_WAIT) {
            body_watcher_.ArmOrNotify();
            return;
        }
        if (result != MOJO_RESULT_OK) {
            FinishBody(result);
            return;
        }
        body_offset_ += written;
    }
    FinishBody(MOJO_RESULT_OK);
}

void BlinkLocalURLLoader::FinishBody(MojoResult result)
{
    auto* client = std::exchange(body_client_, nullptr);
    const auto request_url = std::move(body_request_url_);
    const auto length = static_cast<int64_t>(body_bytes_.size());
    const auto delivered = static_cast<int64_t>(body_offset_);
    body_watcher_.Cancel();
    body_producer_.reset();
    std::vector<std::uint8_t>().swap(body_bytes_);
    body_offset_ = 0;
    if (result == MOJO_RESULT_OK) {
        client->DidFinishLoading(base::TimeTicks::Now(), length,
            static_cast<uint64_t>(length), length);
    } else {
        const int error = result == MOJO_RESULT_FAILED_PRECONDITION
            || result == MOJO_RESULT_CANCELLED ? net::ERR_ABORTED : net::ERR_FAILED;
        client->DidFail(blink::WebURLError(error, request_url), base::TimeTicks::Now(),
            delivered, static_cast<uint64_t>(delivered), delivered);
    }
}

void BlinkLocalURLLoader::Freeze(blink::LoaderFreezeMode mode)
{
    freeze_mode_ = mode;
    if (mode != blink::LoaderFreezeMode::kNone || !pending_request_)
        return;

    std::unique_ptr<network::ResourceRequest> request
        = std::move(pending_request_);
    blink::URLLoaderClient* client = pending_client_;
    pending_client_ = nullptr;
    DeliverAsync(std::move(request), client);
}

void BlinkLocalURLLoader::DidChangePriority(
    blink::WebURLRequest::Priority, int)
{
    // The file is already read. The local data-pipe writer has no transport
    // scheduling priority to change.
}

scoped_refptr<base::SingleThreadTaskRunner>
BlinkLocalURLLoader::GetTaskRunnerForBodyLoader()
{
    return base::SingleThreadTaskRunner::GetCurrentDefault();
}

} // namespace mini_ui
