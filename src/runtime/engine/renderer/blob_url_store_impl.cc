
#include "src/runtime/engine/renderer/blob_url_store_impl.h"

#include "src/runtime/engine/common/thread_call.h"
#include "src/runtime/engine/common/create_and_bind_templ.h"
#include "src/runtime/engine/renderer/blob_registry_impl.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/renderer/platform/network/http_names.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/resource_request.h"
#include "gen/services/network/public/mojom/url_loader_factory.mojom-blink.h"
#include "gen/services/network/public/mojom/url_response_head.mojom-blink-forward.h"
#include "gen/services/network/public/mojom/url_response_head.mojom-blink.h"
#include "gen/services/network/public/mojom/url_response_head.mojom.h"
#include "gen/services/network/public/mojom/url_loader.mojom.h"
#include "gen/services/network/public/mojom/url_loader.mojom-blink.h"
#include "base/functional/bind.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/url_loader_completion_status.h"

bool blink::mojom::blink::BlobURLStore::Register(::mojo::PendingRemote<::blink::mojom::blink::Blob> blob, const ::blink::KURL& url,
    const ::base::UnguessableToken& unsafe_agent_cluster_id, const absl::optional<::blink::BlinkSchemefulSite>& unsafe_top_level_site)
{
    return false;
}

namespace content {

BlobURLStoreSet* BlobURLStoreSet::get()
{
    static BlobURLStoreSet* s_inst = nullptr;
    if (s_inst)
        return s_inst;
    s_inst = new BlobURLStoreSet();
    return s_inst;
}

void BlobURLStoreSet::addUrlBlob(const std::string& url, const std::string& securityOriginToken, ::mojo::PendingRemote<::blink::mojom::blink::Blob> blob)
{
    UrlToBlobs::iterator it = m_urlToBlobs.find(url);
    CHECK(it == m_urlToBlobs.end());

    SecurityOriginAndBlob* securityOriginAndBlob = new SecurityOriginAndBlob();
    securityOriginAndBlob->securityOriginToken = securityOriginToken;

    //::mojo::Remote<::blink::mojom::blink::Blob>* blobRemote = new ::mojo::Remote<::blink::mojom::blink::Blob>();
    securityOriginAndBlob->blob.Bind(std::move(blob));
    securityOriginAndBlob->blob.set_disconnect_handler(base::BindOnce([](const std::string& url) { BlobURLStoreSet::get()->removeUrlBlob(url); }, url));
    m_urlToBlobs[url] = securityOriginAndBlob;
}

void BlobURLStoreSet::removeUrlBlob(const std::string& url)
{
    auto found = m_urlToBlobs.find(url);
    if (found == m_urlToBlobs.end() || found->second->revoked)
        return;
    // Keep deferred destruction, but revoke authorization immediately.
    found->second->revoked = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(FROM_HERE, base::BindOnce([](BlobURLStoreSet* self, const std::string& url) {
        UrlToBlobs::iterator it = self->m_urlToBlobs.find(url);
        if (it == self->m_urlToBlobs.end())
            return;
        SecurityOriginAndBlob* securityOriginAndBlob = it->second;
        delete securityOriginAndBlob;

        self->m_urlToBlobs.erase(it);
    }, base::Unretained(this), url), base::Seconds(6));
}

void BlobURLStoreSet::removeBySecurityOriginToken(const std::string& securityOriginToken)
{
    for (UrlToBlobs::iterator it = m_urlToBlobs.begin(); it != m_urlToBlobs.end();) {
        SecurityOriginAndBlob* securityOriginAndBlob = it->second;
        if (securityOriginAndBlob->securityOriginToken == securityOriginToken) {
            SecurityOriginAndBlob* securityOriginAndBlob = it->second;
            m_urlToBlobs.erase(it++);
            delete securityOriginAndBlob;
        } else {
            ++it;
        }
    }
}

::mojo::Remote<::blink::mojom::blink::Blob>* BlobURLStoreSet::getBlobByUrl(const std::string& url)
{
    UrlToBlobs::iterator it = m_urlToBlobs.find(url);
    if (it == m_urlToBlobs.end() || it->second->revoked)
        return nullptr;
    //::mojo::Remote<::blink::mojom::blink::Blob>* ret = it->second;
    SecurityOriginAndBlob* securityOriginAndBlob = it->second;
    return &securityOriginAndBlob->blob;
}

//-----

BlobURLStoreImpl::BlobURLStoreImpl(const std::string& origin)
    : m_origin(origin)
{
}

BlobURLStoreImpl::~BlobURLStoreImpl()
{
    BlobURLStoreSet::get()->removeBySecurityOriginToken(m_origin);
}

bool BlobURLStoreImpl::Register(::mojo::PendingRemote<::blink::mojom::blink::Blob> blob, const ::blink::KURL& url,
    const ::base::UnguessableToken& unsafe_agent_cluster_id, const absl::optional<::blink::BlinkSchemefulSite>& unsafe_top_level_site)
{
//     std::string urlStr = "BlobURLStoreImpl::Register, url: ";
//     urlStr += url.GetString().Utf8();
//     urlStr += ",,,,,";
//     urlStr += m_origin->ToString().Utf8();
//     urlStr += "\n";
//     OutputDebugStringA(urlStr.c_str());

    BlobURLStoreSet::get()->addUrlBlob(url.GetString().Utf8(), m_origin, std::move(blob));

    //     BlobURLStoreImpl* self = this;
    //     ThreadCall::callBlinkThreadAsync(FROM_HERE, [self] {
    //         std::string* uuid = nullptr;
    //         self->m_blobRemote->GetInternalUUID(base::BindOnce([](std::string** uuid, const WTF::String& uid) {
    //             *uuid = new std::string(uid.Ascii());
    //         }, &uuid));
    //
    //         std::string output = "BlobURLStoreImpl::Register, uuid: ";
    //         output += *uuid;
    //         output += "\n";
    //         delete uuid;
    //         OutputDebugStringA(output.c_str());
    //     });
    //
    //     m_blobRemote.Bind(std::move(blob));
    return true;
}

//using RegisterCallback = base::OnceCallback<void()>;

void BlobURLStoreImpl::Register(::mojo::PendingRemote<::blink::mojom::blink::Blob> blob, const ::blink::KURL& url,
    const ::base::UnguessableToken& unsafe_agent_cluster_id, const ::absl::optional<::blink::BlinkSchemefulSite>& unsafe_top_level_site,
    ::blink::mojom::blink::BlobURLStore::RegisterCallback callback)
{
    BlobURLStoreSet::get()->addUrlBlob(url.GetString().Utf8(), m_origin, std::move(blob));
    std::move(callback).Run();
}

void BlobURLStoreImpl::Revoke(const ::blink::KURL& url)
{
    BlobURLStoreSet::get()->removeUrlBlob(url.GetString().Utf8());
}


class BlobURLStoreLoaderFactoryImpl : public ::network::mojom::URLLoaderFactory {
public:
    BlobURLStoreLoaderFactoryImpl(const ::blink::KURL& url)
    {
        m_url = url;
    }

    void CreateLoaderAndStart(::mojo::PendingReceiver<::network::mojom::URLLoader> loader, int32_t requestId, uint32_t options,
        const ::network::ResourceRequest& request, ::mojo::PendingRemote<::network::mojom::URLLoaderClient> client,
        const ::net::MutableNetworkTrafficAnnotationTag& trafficAnnotation) override
    {
        auto* blob = BlobURLStoreSet::get()->getBlobByUrl(request.url.possibly_invalid_spec());
        auto* blobEndpoint = blob ? blob->TryGet() : nullptr;
        if (!blobEndpoint) {
            mojo::Remote<network::mojom::URLLoaderClient> remote(std::move(client));
            remote->OnComplete(network::URLLoaderCompletionStatus(net::ERR_FILE_NOT_FOUND));
            return;
        }
        blobEndpoint->Load(
            mojo::PendingReceiver<network::mojom::blink::URLLoader>(loader.PassPipe()),
            WTF::String::FromUTF8(request.method), request.headers,
            mojo::PendingRemote<network::mojom::blink::URLLoaderClient>(client.PassPipe(), 0));
    }

    // LoaderFactoryForWorker::CreateURLLoader会走到这个函数
    void Clone(::mojo::PendingReceiver<network::mojom::URLLoaderFactory> factory) override
    {
        createAndBindInterface<::network::mojom::URLLoaderFactory, BlobURLStoreLoaderFactoryImpl>(factory.PassPipe(), m_url);
    }

    ::blink::KURL m_url;
};

//using ResolveAsURLLoaderFactoryCallback = base::OnceCallback<void(const absl::optional<::base::UnguessableToken>&, const absl::optional<::blink::BlinkSchemefulSite>&)>;
void BlobURLStoreImpl::ResolveAsURLLoaderFactory(const ::blink::KURL& url, ::mojo::PendingReceiver<::network::mojom::blink::URLLoaderFactory> factory,
    ::blink::mojom::blink::BlobURLStore::ResolveAsURLLoaderFactoryCallback callback)
{
    // 这里要注意了，从blink的GetBlobURLStore().ResolveAsURLLoaderFactory传过来的居然是::network::mojom::blink::URLLoaderFactory，
    // 但在真正使用时（LoaderFactoryForFrame::CreateURLLoader里），又是::network::mojom::URLLoaderFactory了
    createAndBindInterface<::network::mojom::URLLoaderFactory, BlobURLStoreLoaderFactoryImpl>(factory.PassPipe(), url);
    absl::optional<::base::UnguessableToken> token;
    absl::optional<::blink::BlinkSchemefulSite> site;
    std::move(callback).Run(token, site);
}

#ifdef _DEBUG
static int s_BlobURLTokenImpl = 0;
#endif

class BlobURLTokenImpl : public ::blink::mojom::blink::BlobURLToken {
public:
    explicit BlobURLTokenImpl(base::UnguessableToken token = base::UnguessableToken::Create())
        : m_token(token)
    {
#ifdef _DEBUG
        s_BlobURLTokenImpl++;
        char output[100] = { 0 };
        sprintf_s(output, 99, "BlobURLTokenImpl: %d\n", s_BlobURLTokenImpl);
        OutputDebugStringA(output);
#endif
    }
    ~BlobURLTokenImpl()
    {
#ifdef _DEBUG
        --s_BlobURLTokenImpl;
        char output[100] = { 0 };
        sprintf_s(output, 99, "~BlobURLTokenImpl: %d\n", s_BlobURLTokenImpl);
        OutputDebugStringA(output);
#endif
    }

    /*virtual*/ void Clone(::mojo::PendingReceiver<::blink::mojom::blink::BlobURLToken> token) override
    {
        createAndBindInterface<::blink::mojom::blink::BlobURLToken, BlobURLTokenImpl>(token.PassPipe(), m_token);
    }

    //using GetTokenCallback = base::OnceCallback<void(const ::base::UnguessableToken&)>;
    /*virtual*/ void GetToken(GetTokenCallback callback) override
    {
        std::move(callback).Run(m_token);
    }
private:
    const base::UnguessableToken m_token;

};

//using ResolveForNavigationCallback = base::OnceCallback<void(const absl::optional<::base::UnguessableToken>&)>;
void BlobURLStoreImpl::ResolveForNavigation(const ::blink::KURL& url, ::mojo::PendingReceiver<::blink::mojom::blink::BlobURLToken> token,
    ::blink::mojom::blink::BlobURLStore::ResolveForNavigationCallback callback)
{
    createAndBindInterface<::blink::mojom::blink::BlobURLToken, BlobURLTokenImpl>(token.PassPipe());
    std::move(callback).Run(absl::nullopt);
}

void BlobURLStoreImpl::ResolveForWorkerScriptFetch(const ::blink::KURL& url, ::mojo::PendingReceiver< blink::mojom::blink::BlobURLToken> token, ResolveForWorkerScriptFetchCallback callback)
{
    createAndBindInterface<::blink::mojom::blink::BlobURLToken, BlobURLTokenImpl>(token.PassPipe());
    //absl::optional<::base::UnguessableToken> token;
    std::move(callback).Run(absl::nullopt);
}

}