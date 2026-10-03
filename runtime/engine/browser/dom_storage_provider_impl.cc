
#include "runtime/engine/browser/dom_storage_provider_impl.h"

#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/create_and_bind_templ.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/common/string_conversions.h"
#include "runtime/engine/renderer/web_view_client_impl.h"
#include "runtime/engine/renderer/renderer_storage_broker.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/modules/storage/storage_area_map.h"
#include "third_party/blink/renderer/modules/storage/cached_storage_area.h"
#include "third_party/blink/public/mojom/dom_storage/dom_storage.mojom-blink.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom-blink.h"
#include "third_party/blink/public/mojom/dom_storage/session_storage_namespace.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/platform/wtf/text/string_buffer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "base/files/file_path.h"
#include "base/strings/utf_string_conversions.h"

bool ::blink::mojom::blink::StorageArea::GetAll(
    mojo::PendingRemote<::blink::mojom::blink::StorageAreaObserver>, WTF::Vector<::blink::mojom::blink::KeyValuePtr>*)
{
    return false;
}

namespace content {

class StorageAreaImpl;

class StorageAreaImplMgr {
public:
    static WTF::String buildFileNameStringByStorageKey(bool isLocal, const ::blink::BlinkStorageKey& storageKey)
    {
        WTF::String name = buildFileName(storageKey);
        WTF::String key = storageKey.ToDebugString();
        unsigned hash = WTF::HashTraits<String>::GetHash(key);

        return WTF::String::Format(isLocal ? "local_%s_%d.localsto" : "session_%s_%d.localsto", name.Utf8().c_str(), hash);
    }

    static StorageAreaImplMgr* get();

    StorageAreaImpl* findOrCreateByStorageKey(bool isLocal,
        const ::blink::BlinkStorageKey& storageKey,
        const base::FilePath& localStorageDirectory,
        const WTF::String& namespaceId);
    void associateNamespaceFrame(const WTF::String& namespaceId,
        uint64_t frameId)
    {
        if (!namespaceId.empty() && frameId)
            m_namespaceFrames.Set(namespaceId, frameId);
    }

    uint64_t frameForNamespace(const WTF::String& namespaceId) const
    {
        auto found = m_namespaceFrames.find(namespaceId);
        return found == m_namespaceFrames.end() ? 0 : found->value;
    }

private:
    static String buildFileName(const ::blink::BlinkStorageKey& storageKey)
    {
        const scoped_refptr<const blink::SecurityOrigin>& orig = storageKey.GetSecurityOrigin();

        WTF::StringBuilder builder;
        String origin = orig->Protocol();
        if (orig->Protocol() == "file") {
            return "file";
        }

        builder.Append(orig->Protocol());
        builder.Append("_");
        builder.Append(orig->Host());

        if (true /*orig->HasPort()*/) {
            builder.Append('_');
            builder.Append(String::Number(orig->Port()));
        }
        return builder.ToString();
    }

    WTF::HashMap<String, StorageAreaImpl*> m_areas;
    WTF::HashMap<String, uint64_t> m_namespaceFrames;
};

class StorageAreaImpl {
public:
    StorageAreaImpl(bool isLocal, const ::blink::BlinkStorageKey& storageKey,
        const WTF::String& namespaceId)
        : m_isLocal(isLocal)
        , m_storageKey(storageKey)
        , m_namespaceId(namespaceId)
    {
    }

    bool isLocal() const { return m_isLocal; }

    void setStorageAreaMap(blink::StorageAreaMap* areaMap)
    {
        m_areaMap = areaMap;
    }

    String getItem(const String& key, bool* found) const
    {
        *found = false;
        if (!m_areaMap)
            return g_empty_string;

        *found = true;
        return m_areaMap->GetItem(key);
    }

    unsigned getLength()
    {
        if (!m_areaMap)
            return 0;
        return m_areaMap->GetLength();
    }

    void destroy()
    {
        if (m_isDestroying)
            return;
        m_isDestroying = true;
        StorageAreaImpl* self = this;
        ThreadCall::callBlinkThreadDelayed(FROM_HERE, [self] { delete self; }, 5000);
    }

    bool clear(uint64_t frameId)
    {
        if (m_areaMap) {
            while (m_areaMap->GetLength()) {
                String ignored;
                m_areaMap->RemoveItem(m_areaMap->GetKey(0), &ignored);
            }
        }
        base::Value::Dict result;
        std::string error;
        return RequestRendererStorage(
            frameId, StorageRequest("clear"), &result, &error);
    }

    bool put(uint64_t frameId, const WTF::Vector<uint8_t>& encoded_key,
        const WTF::Vector<uint8_t>& encoded_value)
    {
        base::Value::Dict request = StorageRequest("set");
        std::string key = Uint8VectorToString(encoded_key,
            blink::CachedStorageArea::FormatOption::kLocalStorageDetectFormat).Utf8();
        std::string value = Uint8VectorToString(encoded_value,
            blink::CachedStorageArea::FormatOption::kLocalStorageDetectFormat).Utf8();
        request.Set("key", std::move(key));
        request.Set("value", std::move(value));
        base::Value::Dict result;
        std::string error;
        return RequestRendererStorage(
            frameId, std::move(request), &result, &error);
    }

    bool remove(uint64_t frameId,
        const WTF::Vector<uint8_t>& encoded_key)
    {
        base::Value::Dict request = StorageRequest("remove");
        std::string key = Uint8VectorToString(encoded_key,
            blink::CachedStorageArea::FormatOption::kLocalStorageDetectFormat).Utf8();
        request.Set("key", std::move(key));
        base::Value::Dict result;
        std::string error;
        return RequestRendererStorage(
            frameId, std::move(request), &result, &error);
    }

    void loadFromBroker(uint64_t frameId, const base::FilePath&,
        WTF::Vector<::blink::mojom::blink::KeyValuePtr>* outData)
    {
        base::Value::Dict result;
        std::string error;
        if (!RequestRendererStorage(
                frameId, StorageRequest("keys"), &result, &error))
            return;
        const base::Value::List* keys = result.FindList("keys");
        if (!keys)
            return;
        for (const base::Value& entry : *keys) {
            const std::string* key_string = entry.GetIfString();
            if (!key_string)
                continue;
            base::Value::Dict request = StorageRequest("get");
            request.Set("key", *key_string);
            base::Value::Dict value_result;
            if (!RequestRendererStorage(frameId, std::move(request),
                    &value_result, &error)
                || !value_result.FindBool("found").value_or(false))
                continue;
            const std::string* value_string = value_result.FindString("value");
            if (!value_string)
                continue;
            WTF::Vector<uint8_t> key(key_string->size());
            WTF::Vector<uint8_t> value(value_string->size());
            std::memcpy(key.data(), key_string->data(), key_string->size());
            std::memcpy(value.data(), value_string->data(),
                value_string->size());
            StringToUint8Vector(&key);
            StringToUint8Vector(&value);
            outData->push_back(
                ::blink::mojom::blink::KeyValue::New(key, value));
        }
    }

    // look: CachedStorageArea::Uint8VectorToString
    static void StringToUint8Vector(WTF::Vector<uint8_t>* buf)
    {
        //const char* src, size_t src_len, std::u16string* output
        std::u16string output;
        if (base::UTF8ToUTF16((const char*)buf->data(), buf->size(), &output)) {
            buf->clear();
            buf->resize(1 + output.size() * sizeof(char16_t));
            (*buf)[0] = 0; // StorageFormat::UTF16
            memcpy(buf->data() + 1, output.data(), output.size() * sizeof(char16_t));
        }
    }

    static String Uint8VectorToString(const Vector<uint8_t>& input, blink::CachedStorageArea::FormatOption format_option)
    {
        if (input.empty())
            return g_empty_string;
        const wtf_size_t input_size = input.size();
        String result;
        bool corrupt = false;

        /*StorageFormat*/uint8_t format = static_cast<uint8_t>(input[0]);
        const wtf_size_t payload_size = input_size - 1;
        switch (format) {
            case /*StorageFormat::UTF16*/0:
            {
                if (payload_size % sizeof(UChar) != 0) {
                    corrupt = true;
                    break;
                }
                WTF::StringBuffer<UChar> buffer(payload_size / sizeof(UChar));
                std::memcpy(buffer.Characters(), input.data() + 1, payload_size);
                result = String::Adopt(buffer);
                break;
            }
            case /*StorageFormat::Latin1*/1:
                result = String(base::span(input.data(), input.size()).subspan(1));
                break;
            default:
                corrupt = true;
        }
        if (corrupt) {
            // TODO(mek): Better error recovery when corrupt (or otherwise invalid) data
            // is detected.
            return g_empty_string;
        }
        return result;
    }

private:
    base::Value::Dict StorageRequest(const char* operation) const
    {
        base::Value::Dict request;
        request.Set("operation", operation);
        std::string origin =
            m_storageKey.GetSecurityOrigin()->ToString().Utf8();
        request.Set("origin", std::move(origin));
        request.Set("storageType", m_isLocal ? "local" : "session");
        if (!m_isLocal) {
            std::string namespace_id = m_namespaceId.Utf8();
            request.Set("namespaceId", std::move(namespace_id));
        }
        return request;
    }

    bool m_isLocal;
    bool m_isDestroying = false;
    blink::StorageAreaMap* m_areaMap = nullptr;
    blink::BlinkStorageKey m_storageKey;
    WTF::String m_namespaceId;
};

StorageAreaImplMgr* StorageAreaImplMgr::get()
{
    static StorageAreaImplMgr* s_inst = nullptr;
    if (!s_inst)
        s_inst = new StorageAreaImplMgr();
    return s_inst;
}

StorageAreaImpl* StorageAreaImplMgr::findOrCreateByStorageKey(
    bool isLocal, const ::blink::BlinkStorageKey& storageKey,
    const base::FilePath& localStorageDirectory,
    const WTF::String& namespaceId)
{
    WTF::String fileName =
        buildFileNameStringByStorageKey(isLocal, storageKey);
    std::string storageKeyString = localStorageDirectory.AsUTF8Unsafe() +
        "|" + fileName.Utf8().c_str() + "|" + namespaceId.Utf8().c_str();
    WTF::String key = WTF::String::FromUTF8(storageKeyString);
    auto it = m_areas.find(key);
    if (it != m_areas.end())
        return it->value;

    StorageAreaImpl* result =
        new StorageAreaImpl(isLocal, storageKey, namespaceId);
    m_areas.insert(key, result);
    return result;
}

static base::FilePath getLocalStorageDirByLocalFrameToken(const ::blink::LocalFrameToken& localFrameToken)
{
    base::FilePath result;

    blink::LocalFrame* frame = blink::LocalFrame::FromFrameToken(localFrameToken);
    if (!frame)
        return result;
    blink::LocalFrameView* view = frame->View();
    if (!view)
        return result;
    blink::ChromeClient* chromeClient = view->GetChromeClient();
    if (!chromeClient)
        return result;
    blink::WebViewImpl* webviewimpl = chromeClient->GetWebView();
    if (!webviewimpl)
        return result;
    WebViewClientImpl* client = (WebViewClientImpl*)webviewimpl->Client();
    if (!client)
        return result;
    WebViewHost* mbwebview = client->getEngineViewHost();
    if (!mbwebview)
        return result;

    result = mbwebview->getLocalStorageDir();
    return result;
}

int s_StorageAreaStub = 0;

class StorageAreaStub : public ::blink::mojom::blink::StorageArea {
public:
    StorageAreaStub(bool isLocal,
        const ::blink::BlinkStorageKey& storageKey,
        const ::blink::LocalFrameToken& localFrameToken,
        const WTF::String& namespaceId = WTF::String())
    {
        m_frameId = static_cast<uint64_t>(
            ::blink::LocalFrameToken::Hasher()(localFrameToken));
        s_StorageAreaStub++;
        m_localStorageDir =
            getLocalStorageDirByLocalFrameToken(localFrameToken);
        m_impl = StorageAreaImplMgr::get()->findOrCreateByStorageKey(
            isLocal, storageKey, m_localStorageDir, namespaceId);

        m_impl->loadFromBroker(m_frameId, m_localStorageDir, &m_outData);
    }

    ~StorageAreaStub()
    {
        s_StorageAreaStub--;
    }

    void addObserverImpl(::mojo::PendingRemote<::blink::mojom::blink::StorageAreaObserver> observer)
    {
        CHECK(ThreadCall::isBlinkThread());
        mojo::Remote<::blink::mojom::blink::StorageAreaObserver> observerRemote(std::move(observer));

        // ��������Щ�Ѿ�ʧЧ��
        mojo::RemoteSet<::blink::mojom::blink::StorageAreaObserver> newObservers;
        for (mojo::RemoteSet<::blink::mojom::blink::StorageAreaObserver>::Iterator it = (m_observers.begin()); 
            it != m_observers.end();) {
            ::mojo::Remote<::blink::mojom::blink::StorageAreaObserver>& observer = (::mojo::Remote<::blink::mojom::blink::StorageAreaObserver>&)(*it);
            if (observer.TryGet()) {
                CHECK(observer.is_bound());
                newObservers.Add(std::move(observer));
            } else
                ++((mojo::RemoteSet<::blink::mojom::blink::StorageAreaObserver>::Iterator&)it);
        }
        m_observers.Clear();

        for (mojo::RemoteSet<::blink::mojom::blink::StorageAreaObserver>::Iterator it = (newObservers.begin());
            it != newObservers.end(); ++it) {
            ::mojo::Remote<::blink::mojom::blink::StorageAreaObserver>& observer = (::mojo::Remote<::blink::mojom::blink::StorageAreaObserver>&)(*it);
            m_observers.Add(std::move(observer));
        }

        if (observerRemote.is_bound())
            m_observers.Add(std::move(observerRemote));
    }

    // components/services/storage/dom_storage/storage_area_impl.cc
    void AddObserver(::mojo::PendingRemote<::blink::mojom::blink::StorageAreaObserver> observer) override
    {
        addObserverImpl(std::move(observer));
    }

    void SetStorageAreaMap(void* areaMap) override
    {
        m_impl->setStorageAreaMap((blink::StorageAreaMap*)areaMap);
    }

    enum DelayDispatchObserversType {
        kPut, kDelete, kDeleteAll
    };

    void delayDispatchObservers(DelayDispatchObserversType type,
        const WTF::Vector<uint8_t>& key,
        const WTF::Vector<uint8_t>& value, 
        const absl::optional<WTF::Vector<uint8_t>>& clientOldValue,
        const WTF::String& source, 
        base::OnceCallback<void(bool)> callback)
    {
        if (m_impl->isLocal()) {
            for (const auto& observer : m_observers) {
                if (!observer.TryGet())
                    continue;

                if (type == kPut) {
                    observer->KeyChanged(key, value, clientOldValue, source);
                } else if (type == kDelete) {
                    observer->KeyDeleted(key, std::nullopt, source);
                } else if (type == kDeleteAll) {
                    bool wasNonempty = (m_impl->getLength() == 0);
                    observer->AllDeleted(/*was_nonempty=*/wasNonempty, source);
                }
            }
        }
        std::move(callback).Run(true); // Key already has this value.
    }

    void Put(const WTF::Vector<uint8_t>& key,
        const WTF::Vector<uint8_t>& value,
        const absl::optional<WTF::Vector<uint8_t>>& clientOldValue,
        const WTF::String& source, PutCallback callback) override
    {
        CHECK(ThreadCall::isBlinkThread());
        if (!m_impl->put(m_frameId, key, value)) {
            std::move(callback).Run(false);
            return;
        }
        if (m_impl->isLocal()) {
            delayDispatchObservers(DelayDispatchObserversType::kPut,
                key, value, clientOldValue, source, std::move(callback));
        } else {
            std::move(callback).Run(true);
        }
    }

    void Delete(const WTF::Vector<uint8_t>& key,
        const absl::optional<WTF::Vector<uint8_t>>& clientOldValue,
        const WTF::String& source,
        ::blink::mojom::blink::StorageArea::DeleteCallback callback) override
    {
        CHECK(ThreadCall::isBlinkThread());
        if (!m_impl->remove(m_frameId, key)) {
            std::move(callback).Run(false);
            return;
        }
        if (m_impl->isLocal()) {
            delayDispatchObservers(DelayDispatchObserversType::kDelete,
                key, WTF::Vector<uint8_t>(), clientOldValue, source,
                std::move(callback));
        } else {
            std::move(callback).Run(true);
        }
    }

    void DeleteAll(const WTF::String& source,
        ::mojo::PendingRemote<::blink::mojom::blink::StorageAreaObserver> newObserver,
        ::blink::mojom::blink::StorageArea::DeleteAllCallback callback) override
    {
        CHECK(ThreadCall::isBlinkThread());
        addObserverImpl(std::move(newObserver));
        if (!m_impl->clear(m_frameId)) {
            std::move(callback).Run(false);
            return;
        }
        if (m_impl->isLocal()) {
            delayDispatchObservers(DelayDispatchObserversType::kDeleteAll,
                WTF::Vector<uint8_t>(), WTF::Vector<uint8_t>(), std::nullopt,
                source, std::move(callback));
        } else {
            std::move(callback).Run(true);
        }
    }

    void Get(const WTF::Vector<uint8_t>& key, ::blink::mojom::blink::StorageArea::GetCallback callback) override
    {
        CHECK(ThreadCall::isBlinkThread());
        for (const auto& item : m_outData) {
            if (item->key == key) {
                std::move(callback).Run(true, item->value);
                return;
            }
        }
        const WTF::Vector<uint8_t> empty;
        std::move(callback).Run(false, empty);
    }

    bool GetAll(
        ::mojo::PendingRemote<::blink::mojom::blink::StorageAreaObserver> newObserver, WTF::Vector<::blink::mojom::blink::KeyValuePtr>* outData) override
    {
        CHECK(ThreadCall::isBlinkThread());
        addObserverImpl(std::move(newObserver));


        outData->swap(m_outData);
        return true;
    }

    void GetAll(::mojo::PendingRemote<::blink::mojom::blink::StorageAreaObserver> newObserver, GetAllCallback callback) override
    {
        CHECK(ThreadCall::isBlinkThread());
        addObserverImpl(std::move(newObserver));
        WTF::Vector<::blink::mojom::blink::KeyValuePtr> result;
        result.ReserveInitialCapacity(m_outData.size());
        for (const auto& item : m_outData)
            result.push_back(item->Clone());
        std::move(callback).Run(std::move(result));
    }

    void Checkpoint() override
    {

    }

private:
    uint64_t m_frameId = 0;
    base::FilePath m_localStorageDir;
    WTF::Vector<::blink::mojom::blink::KeyValuePtr> m_outData;
    StorageAreaImpl* m_impl = nullptr;
    mojo::RemoteSet<::blink::mojom::blink::StorageAreaObserver> m_observers;
};

class SessionStorageNamespaceImpl : public ::blink::mojom::blink::SessionStorageNamespace {
public:
    SessionStorageNamespaceImpl(const WTF::String& namespaceId)
    {
        m_namespaceId = namespaceId;
    }

    void Clone(const WTF::String& cloneToNamespace) override
    {
        base::Value::Dict request;
        request.Set("operation", "clone-session");
        request.Set("storageType", "session");
        std::string source_namespace = m_namespaceId.Utf8();
        std::string target_namespace = cloneToNamespace.Utf8();
        request.Set("namespaceId", std::move(source_namespace));
        request.Set("targetNamespaceId", std::move(target_namespace));
        base::Value::Dict result;
        std::string error;
        uint64_t frame_id =
            StorageAreaImplMgr::get()->frameForNamespace(m_namespaceId);
        content::RequestRendererStorage(
            frame_id, std::move(request), &result, &error);
    }

private:
    WTF::String m_namespaceId;
};

class DomStorageImpl : public ::blink::mojom::blink::DomStorage {
public:
    DomStorageImpl()
    {
    }

    ~DomStorageImpl()
    {
    }

    void OpenLocalStorage(const ::blink::BlinkStorageKey& storageKey, const ::blink::LocalFrameToken& localFrameToken,
        ::mojo::PendingReceiver<::blink::mojom::blink::StorageArea> area) override
    {
        createAndBindBrokerProxy<::blink::mojom::blink::StorageArea, StorageAreaStub>(area.PassPipe(), true, storageKey, localFrameToken);
    }

    // namespaceId��ͬһ��ҳ��͹��ã������Ƿ���frame��Ҳ�����Ƿ�ͬԴ����blink::StorageAreaMap��sub frameͬԴ�͹���
    // window.open��ʱ�򣬻���Ǹ�frame��session storage���Ƶ��µı�open��frame��
    void BindSessionStorageNamespace(const WTF::String& namespaceId, ::mojo::PendingReceiver<::blink::mojom::blink::SessionStorageNamespace> receiver) override
    {
        //char* output = (char*)malloc(400);
        //sprintf(output, "BindSessionStorageNamespace: %s,\n", namespaceId.Utf8().c_str());
        //OutputDebugStringA(output);
        //free(output);
        createAndBindBrokerProxy<::blink::mojom::blink::SessionStorageNamespace, SessionStorageNamespaceImpl>(receiver.PassPipe(), namespaceId);
    }

    // ����󶨵�ʱ��������Ҫ����Ƿ�Ҫ������Դ��frame��session
    void BindSessionStorageArea(const ::blink::BlinkStorageKey& storageKey,
        const ::blink::LocalFrameToken& localFrameToken,
        const WTF::String& namespaceId,
        ::mojo::PendingReceiver<::blink::mojom::blink::StorageArea>
            sessionNamespace) override
    {
        StorageAreaImplMgr::get()->associateNamespaceFrame(namespaceId,
            static_cast<uint64_t>(
                ::blink::LocalFrameToken::Hasher()(localFrameToken)));
        createAndBindBrokerProxy<::blink::mojom::blink::StorageArea,
            StorageAreaStub>(sessionNamespace.PassPipe(), false, storageKey,
            localFrameToken, namespaceId);
    }
};

DomStorageProviderImpl::DomStorageProviderImpl()
{
}

void DomStorageProviderImpl::BindDomStorage(
    ::mojo::PendingReceiver<::blink::mojom::blink::DomStorage> receiver, ::mojo::PendingRemote<::blink::mojom::blink::DomStorageClient> client)
{
    createAndBindBrokerProxy<::blink::mojom::blink::DomStorage, DomStorageImpl>(receiver.PassPipe());
}

}