// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "gen/services/network/public/mojom/url_loader_factory.mojom-blink.h"
#include "mojo/public/c/system/functions.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "src/runtime/engine/renderer/blob_registry_impl.h"
#include "src/runtime/engine/renderer/blob_url_store_impl.h"
#include "src/runtime/network/loader/blob_resource_loader.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

class NetworkCompletionProbe final : public network::mojom::URLLoaderClient {
 public:
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr) override {}
  void OnReceiveResponse(network::mojom::URLResponseHeadPtr,
                         mojo::ScopedDataPipeConsumerHandle,
                         std::optional<mojo_base::BigBuffer>) override {}
  void OnReceiveRedirect(const net::RedirectInfo&,
                         network::mojom::URLResponseHeadPtr) override {}
  void OnUploadProgress(int64_t,
                        int64_t,
                        OnUploadProgressCallback callback) override {
    std::move(callback).Run();
  }
  void OnTransferSizeUpdated(int32_t) override {}
  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    ++completion_count;
    error = status.error_code;
  }

  int completion_count = 0;
  int error = net::ERR_IO_PENDING;
  mojo::Receiver<network::mojom::URLLoaderClient> receiver{this};
};

class ResourceCompletionProbe final : public blink::URLLoaderClient {
 public:
  ~ResourceCompletionProbe() override = default;

  void DidFinishLoading(base::TimeTicks,
                        int64_t,
                        uint64_t,
                        int64_t) override {
    ++finish_count;
  }

  void DidFail(const blink::WebURLError& error,
               base::TimeTicks,
               int64_t,
               uint64_t,
               int64_t) override {
    ++failure_count;
    failure = error.reason();
  }

  int finish_count = 0;
  int failure_count = 0;
  int failure = net::ERR_IO_PENDING;
};

void ClosedBlobURLFactoryCompletesWithError() {
  constexpr char kOrigin[] = "blob-endpoint-factory";
  constexpr char kURL[] =
      "blob:https://endpoint.invalid/00000000-0000-0000-0000-000000000001";
  content::BlobURLStoreImpl store(kOrigin);

  mojo::PendingRemote<blink::mojom::blink::Blob> blob;
  auto blob_receiver = blob.InitWithNewPipeAndPassReceiver();
  content::BlobURLStoreSet::get()->addUrlBlob(kURL, kOrigin, std::move(blob));
  blob_receiver.reset();

  mojo::Remote<network::mojom::URLLoaderFactory> factory;
  bool resolved = false;
  store.ResolveAsURLLoaderFactory(
      blink::KURL(GURL(kURL)),
      mojo::PendingReceiver<network::mojom::blink::URLLoaderFactory>(
          factory.BindNewPipeAndPassReceiver().PassPipe()),
      base::BindOnce(
          [](bool* resolved,
             const std::optional<base::UnguessableToken>&,
             const std::optional<blink::BlinkSchemefulSite>&) {
            *resolved = true;
          },
          &resolved));
  Require(resolved, "blob URL factory resolution callback completes");

  auto* factory_endpoint = factory.TryGet();
  Require(factory_endpoint, "resolved blob URL factory has a live endpoint");
  network::ResourceRequest request;
  request.url = GURL(kURL);
  NetworkCompletionProbe client;
  mojo::Remote<network::mojom::URLLoader> loader;
  factory_endpoint->CreateLoaderAndStart(
      loader.BindNewPipeAndPassReceiver(), 1, 0, request,
      client.receiver.BindNewPipeAndPassRemote(),
      net::MutableNetworkTrafficAnnotationTag());
  base::RunLoop().RunUntilIdle();

  Require(client.completion_count == 1,
          "closed URL-store Blob completes its consumer exactly once");
  Require(client.error == net::ERR_FILE_NOT_FOUND,
          "closed URL-store Blob reports ERR_FILE_NOT_FOUND");
  base::RunLoop().RunUntilIdle();
  Require(client.completion_count == 1,
          "closed URL-store Blob does not send duplicate completion");
}

void ClosedBlobResourceLoadCompletesWithError() {
  constexpr char kOrigin[] = "blob-endpoint-resource";
  constexpr char kURL[] =
      "blob:https://endpoint.invalid/00000000-0000-0000-0000-000000000002";
  content::BlobURLStoreImpl store(kOrigin);

  mojo::PendingRemote<blink::mojom::blink::Blob> blob;
  auto blob_receiver = blob.InitWithNewPipeAndPassReceiver();
  content::BlobURLStoreSet::get()->addUrlBlob(kURL, kOrigin, std::move(blob));

  ResourceCompletionProbe client;
  auto* loader = mini_electron::BlobResourceLoader::createAsync(
      0, GURL(kURL), &client);
  loader->start();
  blob_receiver.reset();
  base::RunLoop().RunUntilIdle();

  Require(client.failure_count == 1 && client.finish_count == 0,
          "closed resource-loader Blob fails its consumer exactly once");
  Require(client.failure == net::ERR_FILE_NOT_FOUND,
          "closed resource-loader Blob reports ERR_FILE_NOT_FOUND");
  base::RunLoop().RunUntilIdle();
  Require(client.failure_count == 1 && client.finish_count == 0,
          "closed resource-loader Blob does not send duplicate completion");
  delete loader;
}

void ClosedBytesProviderRejectsRegistration() {
  content::BlobRegistryImpl registry;
  mojo::PendingRemote<blink::mojom::blink::BytesProvider> provider;
  auto provider_receiver = provider.InitWithNewPipeAndPassReceiver();
  provider_receiver.reset();

  WTF::Vector<blink::mojom::blink::DataElementPtr> elements;
  elements.push_back(blink::mojom::blink::DataElement::NewBytes(
      blink::mojom::blink::DataElementBytes::New(
          3, std::nullopt, std::move(provider))));
  mojo::Remote<blink::mojom::blink::Blob> blob;
  int callback_count = 0;
  registry.Register(
      blob.BindNewPipeAndPassReceiver(),
      WTF::String::FromUTF8("closed-bytes-provider"),
      WTF::String::FromUTF8("application/octet-stream"), WTF::String(),
      std::move(elements),
      base::BindOnce([](int* count) { ++*count; }, &callback_count));

  Require(callback_count == 1,
          "closed BytesProvider completes registration callback exactly once");
  Require(!blob.TryGet(),
          "closed BytesProvider closes the rejected Blob endpoint");
  Require(!content::BlobEntry::findByUuid("closed-bytes-provider"),
          "closed BytesProvider cannot register fake empty blob content");
  base::RunLoop().RunUntilIdle();
  Require(callback_count == 1,
          "closed BytesProvider does not duplicate registration completion");
}

void EmbeddedBlobStillRegistersWithExactBytes() {
  content::BlobRegistryImpl registry;
  WTF::Vector<uint8_t> payload;
  payload.push_back('o');
  payload.push_back('k');
  WTF::Vector<blink::mojom::blink::DataElementPtr> elements;
  elements.push_back(blink::mojom::blink::DataElement::NewBytes(
      blink::mojom::blink::DataElementBytes::New(
          payload.size(), std::optional<WTF::Vector<uint8_t>>(payload),
          mojo::PendingRemote<blink::mojom::blink::BytesProvider>())));

  mojo::Remote<blink::mojom::blink::Blob> blob;
  int callback_count = 0;
  registry.Register(
      blob.BindNewPipeAndPassReceiver(), WTF::String::FromUTF8("live-bytes"),
      WTF::String::FromUTF8("text/plain"), WTF::String(),
      std::move(elements),
      base::BindOnce([](int* count) { ++*count; }, &callback_count));
  Require(callback_count == 1 && blob.TryGet(),
          "live embedded Blob registers and completes exactly once");

  content::BlobEntry* entry = content::BlobEntry::findByUuid("live-bytes");
  std::vector<uint8_t> bytes;
  int64_t total_size = 0;
  Require(entry && entry->read(&bytes, 0, -1, &total_size) == 2,
          "live embedded Blob remains readable");
  Require(total_size == 2 && bytes == std::vector<uint8_t>({'o', 'k'}),
          "live embedded Blob preserves exact payload");

  blob.reset();
  base::RunLoop().RunUntilIdle();
  Require(!content::BlobEntry::findByUuid("live-bytes"),
          "live Blob entry follows endpoint lifetime");
}

}  // namespace

int main() {
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::DEFAULT);
  WTF::Partitions::Initialize();
  WTF::Initialize();
  Require(MojoInitialize(nullptr) == MOJO_RESULT_OK,
          "initialize actual lite transport");
  ClosedBlobURLFactoryCompletesWithError();
  ClosedBlobResourceLoadCompletesWithError();
  ClosedBytesProviderRejectsRegistration();
  EmbeddedBlobStillRegistersWithExactBytes();
  base::RunLoop().RunUntilIdle();
  std::puts("PASS: closed Blob and BytesProvider endpoints complete without crashes or hangs");
  return 0;
}
