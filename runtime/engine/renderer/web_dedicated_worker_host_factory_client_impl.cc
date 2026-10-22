
#include "runtime/engine/renderer/web_dedicated_worker_host_factory_client_impl.h"

#include "runtime/engine/renderer/blob_url_store_impl.h"
#include "runtime/engine/renderer/web_worker_fetch_context_impl.h"
#include "runtime/engine/common/create_and_bind_templ.h"
#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "third_party/blink/public/platform/web_dedicated_worker.h"
#include "third_party/blink/public/common/loader/worker_main_script_load_parameters.h"
#include "services/network/public/cpp/cross_origin_embedder_policy.h"
#include "gen/third_party/blink/public/mojom/loader/code_cache.mojom-blink.h"
#include "gen/third_party/blink/public/mojom/reporting/reporting.mojom-blink.h"
#include <windows.h>

namespace content {

class CodeCacheHostImpl : public ::blink::mojom::blink::CodeCacheHost {
public:
    explicit CodeCacheHostImpl(mojo::ScopedMessagePipeHandle pipe)
        : m_receiver(this, mojo::PendingReceiver<blink::mojom::blink::CodeCacheHost>(std::move(pipe)))
    {
        m_receiver.set_disconnect_handler(base::BindOnce(
            [](CodeCacheHostImpl* self) { delete self; }, base::Unretained(this)));
    }

    void DidGenerateCacheableMetadata(
        ::blink::mojom::blink::CodeCacheType cache_type, const ::blink::KURL& url, ::base::Time expected_response_time, ::mojo_base::BigBuffer data) override
    {
        // Code caching is optional and this embedder does not store metadata.
        // Keep the host available so every fetch completes with a genuine miss.
    }

    void FetchCachedCode(::blink::mojom::blink::CodeCacheType cache_type, const ::blink::KURL& url,
        ::blink::mojom::blink::CodeCacheHost::FetchCachedCodeCallback callback) override
    {
        std::move(callback).Run(base::Time(), mojo_base::BigBuffer());
    }

    void ClearCodeCacheEntry(::blink::mojom::blink::CodeCacheType cache_type, const ::blink::KURL& url) override
    {
        // There is no stored metadata to clear.
    }

    void DidGenerateCacheableMetadataInCacheStorage(
        const ::blink::KURL& url, ::base::Time expected_response_time, ::mojo_base::BigBuffer data, const WTF::String& cache_storage_cache_name) override
    {
        // This embedder has no CacheStorage service. Reject this connection
        // rather than claiming to update a CacheStorage response's side data.
        m_receiver.ResetWithReason(1, "CacheStorage is not available");
        delete this;
    }
private:
    mojo::Receiver<blink::mojom::blink::CodeCacheHost> m_receiver;
};

void bindCodeCacheHost(mojo::ScopedMessagePipeHandle pipe)
{
    new CodeCacheHostImpl(std::move(pipe));
}

class ReportingServiceProxyImpl : public ::blink::mojom::blink::ReportingServiceProxy {
public:
    void QueueInterventionReport(const ::blink::KURL& url, const WTF::String& id, const WTF::String& message, const WTF::String& source_file,
        int32_t line_number, int32_t column_number) override
    {
    }

    void QueueDeprecationReport(const ::blink::KURL& url, const WTF::String& id, absl::optional<::base::Time> anticipatedRemoval, const WTF::String& message,
        const WTF::String& source_file, int32_t line_number, int32_t column_number) override
    {
    }

    void QueueCspViolationReport(const ::blink::KURL& url, const WTF::String& group, const WTF::String& document_url, const WTF::String& referrer,
        const WTF::String& blocked_url, const WTF::String& effective_directive, const WTF::String& original_policy, const WTF::String& source_file,
        const WTF::String& script_sample, const WTF::String& disposition, uint16_t status_code, int32_t line_number, int32_t column_number) override
    {
    }

    void QueuePermissionsPolicyViolationReport(const ::blink::KURL& url, const WTF::String& endpoint, const WTF::String& policy_id,
        const WTF::String& disposition, const WTF::String& message, const WTF::String& source_file, int32_t line_number, int32_t column_number) override
    {
    }

    void QueueDocumentPolicyViolationReport(const ::blink::KURL& url, const WTF::String& group, const WTF::String& policy_id, const WTF::String& disposition,
        const WTF::String& message, const WTF::String& source_file, int32_t line_number, int32_t column_number) override
    {
    }
};

class DedicatedWorkerHostBrowserInterfaceBroker : public ::blink::mojom::BrowserInterfaceBroker {
public:
    DedicatedWorkerHostBrowserInterfaceBroker()
    {
    }

    ~DedicatedWorkerHostBrowserInterfaceBroker()
    {
    }

    void setOrgin(const std::string& origin)
    {
        m_origin = origin;
    }

private:
    void GetInterface(::mojo::GenericPendingReceiver receiver) override // 参考WebLocalFrameClientImpl::GetInterface
    {
        std::string output("DedicatedWorkerHostBrowserInterfaceBroker::GetInterface: ");
        std::string name = receiver.interface_name().has_value() ? receiver.interface_name().value() : "no-name";
        output += name;
        output += "\n";
        OutputDebugStringA(output.c_str());

        if ("blink.mojom.CodeCacheHost" == name) {
            bindCodeCacheHost(receiver.PassPipe());
        } else if ("blink.mojom.ReportingServiceProxy" == name) {
            createAndBindBrokerProxy<::blink::mojom::blink::ReportingServiceProxy, ReportingServiceProxyImpl>(receiver.PassPipe());
        } else if ("blink.mojom.BlobURLStore" == name) {
            createAndBindInterface<::blink::mojom::blink::BlobURLStore, BlobURLStoreImpl>(receiver.PassPipe(), (m_origin));
        }
        // Unimplemented interfaces close their pending pipe on return.
    }

    std::string m_origin;
};

class DedicatedWorkerHostImpl : public blink::mojom::blink::DedicatedWorkerHost {
    ~DedicatedWorkerHostImpl() override
    {
    }
};

WebDedicatedWorkerHostFactoryClientImpl::WebDedicatedWorkerHostFactoryClientImpl(blink::WebDedicatedWorker* worker)
    //: m_browserInterfaceBrokerImpl(new DedicatedWorkerHostBrowserInterfaceBroker())
    //, m_browserInterfaceBroker(m_browserInterfaceBrokerImpl)
    //, m_dedicatedWorkerHost(new DedicatedWorkerHostImpl())
    : m_weakFactory(this)
{
    m_worker = worker;
}

WebDedicatedWorkerHostFactoryClientImpl::~WebDedicatedWorkerHostFactoryClientImpl()
{
}

void WebDedicatedWorkerHostFactoryClientImpl::CreateWorkerHostDeprecated(
    const blink::DedicatedWorkerToken& dedicated_worker_token,
    const blink::WebURL& script_url, 
    const blink::WebSecurityOrigin& origin, 
    blink::WebDedicatedWorkerHostFactoryClient::CreateWorkerHostCallback callback)
{
    mojo::PendingRemote<blink::mojom::blink::DedicatedWorkerHost> dedicatedWorkerHost;
    mojo::PendingRemote<blink::mojom::BrowserInterfaceBroker> browserInterfaceBroker;

    CHECK(!m_browserInterfaceBrokerImpl);
    mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> pendingBroker = browserInterfaceBroker.InitWithNewPipeAndPassReceiver();
    m_browserInterfaceBrokerImpl = createAndBindBrokerProxy<::blink::mojom::BrowserInterfaceBroker, DedicatedWorkerHostBrowserInterfaceBroker>(pendingBroker.PassPipe());
    m_browserInterfaceBrokerImpl->setOrgin(origin.Get()->ToRawString().Utf8());
    //m_browserInterfaceBroker.Bind(std::move(pendingBroker));

    CHECK(!m_dedicatedWorkerHost);
    mojo::PendingReceiver<blink::mojom::blink::DedicatedWorkerHost> pendingWorkerHost = dedicatedWorkerHost.InitWithNewPipeAndPassReceiver();
    m_dedicatedWorkerHost = createAndBindBrokerProxy<::blink::mojom::blink::DedicatedWorkerHost, DedicatedWorkerHostImpl>(pendingWorkerHost.PassPipe());
    //m_dedicatedWorkerHost.Bind(std::move(pendingWorkerHost));

    m_worker->OnWorkerHostCreated(std::move(browserInterfaceBroker), std::move(dedicatedWorkerHost), origin);

    base::WeakPtr<WebDedicatedWorkerHostFactoryClientImpl> weak = m_weakFactory.GetWeakPtr();

    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(
            [](blink::WebDedicatedWorkerHostFactoryClient::CreateWorkerHostCallback callback, base::WeakPtr<WebDedicatedWorkerHostFactoryClientImpl> self) {
                if (self.get()) {
                    std::move(callback).Run(
                        network::CrossOriginEmbedderPolicy(), blink::CrossVariantMojoRemote<blink::mojom::BackForwardCacheControllerHostInterfaceBase>());
                }
            }, std::move(callback), std::move(weak)));
}

void WebDedicatedWorkerHostFactoryClientImpl::CreateWorkerHost(
    const blink::DedicatedWorkerToken& dedicatedWorkerToken, 
    const blink::WebURL& scriptUrl,
    network::mojom::CredentialsMode credentialsMode,
    const blink::WebFetchClientSettingsObject& fetchClientSettingsObject,
    blink::CrossVariantMojoRemote<blink::mojom::BlobURLTokenInterfaceBase> blobUrlToken, 
    net::StorageAccessApiStatus storageAccessApiStatus)
{
    mojo::PendingRemote<blink::mojom::blink::DedicatedWorkerHost> dedicatedWorkerHost;
    mojo::PendingRemote<blink::mojom::BrowserInterfaceBroker> browserInterfaceBroker;

    blink::WebSecurityOrigin origin = blink::WebSecurityOrigin::Create(scriptUrl);
    //CHECK(!m_browserInterfaceBroker.is_bound());
    //m_browserInterfaceBrokerImpl->setOrgin(origin);
    //mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> pendingBroker = browserInterfaceBroker.InitWithNewPipeAndPassReceiver();
    //m_browserInterfaceBroker.Bind(std::move(pendingBroker));
    CHECK(!m_browserInterfaceBrokerImpl);
    mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> pendingBroker = browserInterfaceBroker.InitWithNewPipeAndPassReceiver();
    m_browserInterfaceBrokerImpl = createAndBindBrokerProxy<::blink::mojom::BrowserInterfaceBroker, DedicatedWorkerHostBrowserInterfaceBroker>(pendingBroker.PassPipe());
    m_browserInterfaceBrokerImpl->setOrgin(origin.Get()->ToRawString().Utf8());

    //CHECK(!m_dedicatedWorkerHost.is_bound());
    //mojo::PendingReceiver<blink::mojom::blink::DedicatedWorkerHost> pendingWorkerHost = dedicatedWorkerHost.InitWithNewPipeAndPassReceiver();
    //m_dedicatedWorkerHost.Bind(std::move(pendingWorkerHost));
    CHECK(!m_dedicatedWorkerHost);
    mojo::PendingReceiver<blink::mojom::blink::DedicatedWorkerHost> pendingWorkerHost = dedicatedWorkerHost.InitWithNewPipeAndPassReceiver();
    m_dedicatedWorkerHost = createAndBindBrokerProxy<::blink::mojom::blink::DedicatedWorkerHost, DedicatedWorkerHostImpl>(pendingWorkerHost.PassPipe());

    m_worker->OnWorkerHostCreated(std::move(browserInterfaceBroker), std::move(dedicatedWorkerHost), origin);

    base::WeakPtr<WebDedicatedWorkerHostFactoryClientImpl> weak = m_weakFactory.GetWeakPtr();
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<WebDedicatedWorkerHostFactoryClientImpl> self) {
                if (self.get()) {
                    self->m_worker->OnScriptLoadStarted(nullptr, blink::CrossVariantMojoRemote<blink::mojom::BackForwardCacheControllerHostInterfaceBase>());
                }
            }, std::move(weak)));
}

scoped_refptr<blink::WebWorkerFetchContext> WebDedicatedWorkerHostFactoryClientImpl::CloneWorkerFetchContext(
    blink::WebWorkerFetchContext* webWorkerFetchContext, 
    scoped_refptr<base::SingleThreadTaskRunner> taskRunner)
{
    return static_cast<WebWorkerFetchContextImpl*>(webWorkerFetchContext)->Clone();
}

}