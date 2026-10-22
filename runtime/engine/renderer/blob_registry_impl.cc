
#include "runtime/engine/renderer/blob_registry_impl.h"

#include "runtime/engine/renderer/blob_url_store_impl.h"
#include "runtime/engine/common/common.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/common/create_and_bind_templ.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom-forward.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom.h"
#include "services/network/public/mojom/data_pipe_getter.mojom-blink.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "net/base/net_errors.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_util.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "base/run_loop.h"
#include "url/url_util.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include <algorithm>
#include <limits>
#include <windows.h>

namespace mini_electron {
void pushBackToBuffer(std::vector<char>* buf, const char* data, size_t len);
}

bool ::blink::mojom::BlobRegistry::Register(::mojo::PendingReceiver<::blink::mojom::Blob> blob, const std::string& uuid, const std::string& content_type,
    const std::string& content_disposition, std::vector<::blink::mojom::DataElementPtr> elements)
{
    // No implementation is bound to the generated base interface.
    return false;
}

bool ::blink::mojom::blink::BlobRegistry::Register(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const WTF::String& uuid,
    const WTF::String& content_type, const WTF::String& content_disposition, WTF::Vector<::blink::mojom::blink::DataElementPtr> elements)
{
    // No implementation is bound to the generated base interface.
    return false;
}

// bool ::blink::mojom::blink::BlobRegistry::GetBlobFromUUID(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const WTF::String& uuid)
// {
//     content::printFuncName(__FUNCTION__, true, true);
//     return false;
// }
// 
// bool ::blink::mojom::BlobRegistry::GetBlobFromUUID(::mojo::PendingReceiver<::blink::mojom::Blob> blob, const std::string& uuid)
// {
//     content::printFuncName(__FUNCTION__, true, true);
//     return false;
// }

bool blink::mojom::blink::Blob::CaptureSnapshot(uint64_t* out_length, absl::optional<::base::Time>* out_modification_time)
{
    // No implementation is bound to the generated base interface.
    return false;
}

namespace content {

class BlobReceiver;
class DataElementImpl;

static ::base::FilePath standardizationFilePath(const ::base::FilePath& path)
{
    std::string result = path.AsUTF8Unsafe();
    if (base::StartsWith(result, "file:///", base::CompareCase::INSENSITIVE_ASCII))
        result = result.substr(sizeof("file:///") - 1);

    url::RawCanonOutputT<char16_t> unescaped;
    url::DecodeURLEscapeSequences(std::string_view(result.data(), result.size()), url::DecodeURLMode::kUTF8OrIsomorphic, &unescaped);
    return base::FilePath::FromUTF16Unsafe(std::u16string_view(unescaped.data(), base::checked_cast<wtf_size_t>(unescaped.length())));
}

std::map<std::string, BlobEntry*>* s_blobEntrys = nullptr;

static void writeBlobToDataPipe(const std::string& uuid, mojo::ScopedDataPipeProducerHandle pipe,
    network::mojom::blink::DataPipeGetter::ReadCallback callback);

class BlobReceiver : public ::blink::mojom::blink::Blob {
public:
    BlobReceiver(mojo::ScopedMessagePipeHandle pipe, const std::string& uuid)
    {
        //m_blobEntry = blobEntry;
        m_uuid = uuid;
        m_receiver.Bind(mojo::PendingReceiver<blink::mojom::blink::Blob>(std::move(pipe)));
        m_receiver.set_disconnect_handler(base::BindOnce(
            [](BlobReceiver* self) { delete self; }, base::Unretained(this)));

        //char output[100] = { 0 };
        //sprintf(output, "BlobReceiver: %p, %d\n", this, s_blobEntrys->size());
        //OutputDebugStringA(output);
    }

    ~BlobReceiver()
    {
        std::map<std::string, BlobEntry*>::const_iterator it = s_blobEntrys->find(m_uuid);
        CHECK(it != s_blobEntrys->end());
        BlobEntry* blobEntry = it->second;
        blobEntry->m_blobReceiverRefCount--;
        if (0 == blobEntry->m_blobReceiverRefCount) {
            s_blobEntrys->erase(it);
            delete blobEntry;
        }

        //char output[100] = { 0 };
        //sprintf(output, "~~BlobReceiver: %p, %d\n", this, s_blobEntrys->size());
        //OutputDebugStringA(output);
    }

    void Clone(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob) override
    {
        //content::printFuncName(__FUNCTION__, true, false);

        BlobEntry* blobEntry = BlobEntry::findByUuid(m_uuid);
        CHECK(blobEntry);
        ++blobEntry->m_blobReceiverRefCount;

        new BlobReceiver(blob.PassPipe(), m_uuid);
    }

    class DataPipeGetterImpl : public ::network::mojom::blink::DataPipeGetter {
    public:
        DataPipeGetterImpl(std::string uuid)
        {
            m_uuid = uuid;
            BlobEntry* entry = BlobEntry::findByUuid(m_uuid);
            if (entry) {
                ++entry->m_blobReceiverRefCount;
                new BlobReceiver(m_blob.BindNewPipeAndPassReceiver().PassPipe(), m_uuid);
            }
        }

        void Read(::mojo::ScopedDataPipeProducerHandle pipe, ReadCallback callback) override
        {
            writeBlobToDataPipe(m_uuid, std::move(pipe), std::move(callback));
        }

        void Clone(::mojo::PendingReceiver<DataPipeGetter> receiver) override
        {
            createAndBindInterface<::network::mojom::blink::DataPipeGetter, DataPipeGetterImpl>(receiver.PassPipe(), m_uuid);
        }

    private:
        std::string m_uuid;
        mojo::Remote<blink::mojom::blink::Blob> m_blob;
    };

    void AsDataPipeGetter(::mojo::PendingReceiver<::network::mojom::blink::DataPipeGetter> dataPipeGetter) override
    {
        content::printFuncName(__FUNCTION__, true, false); // !!!
        CHECK(!m_uuid.empty());
        createAndBindInterface<::network::mojom::blink::DataPipeGetter, DataPipeGetterImpl>(dataPipeGetter.PassPipe(), m_uuid);
    }

    void ReadAll(::mojo::ScopedDataPipeProducerHandle pipe, ::mojo::PendingRemote<::blink::mojom::blink::BlobReaderClient> client/*, bool isSync*/) override;

    void ReadRange(uint64_t offset, uint64_t length, ::mojo::ScopedDataPipeProducerHandle pipe,
        ::mojo::PendingRemote<::blink::mojom::blink::BlobReaderClient> client) override;

    void Load(::mojo::PendingReceiver<::network::mojom::blink::URLLoader> loader, const WTF::String& request_method, const ::net::HttpRequestHeaders& headers,
        ::mojo::PendingRemote<::network::mojom::blink::URLLoaderClient> client) override;

    //using ReadSideDataCallback = base::OnceCallback<void(absl::optional<::mojo_base::BigBuffer>)>;
    void ReadSideData(ReadSideDataCallback callback) override;

    bool CaptureSnapshot(uint64_t* out_length, absl::optional<::base::Time>* out_modification_time) override
    {
        BlobEntry* entry = BlobEntry::findByUuid(m_uuid);
        std::vector<char> unused;
        int64_t size = 0;
        if (!entry || entry->read(&unused, 0, 0, &size) < 0)
            return false;
        *out_length = size;
        *out_modification_time = absl::nullopt;
        return true;
    }

    //using CaptureSnapshotCallback = base::OnceCallback<void(uint64_t, absl::optional<::base::Time>)>;
    void CaptureSnapshot(CaptureSnapshotCallback callback) override
    {
        uint64_t length = 0;
        absl::optional<base::Time> modificationTime;
        if (!CaptureSnapshot(&length, &modificationTime)) {
            m_receiver.ResetWithReason(1, "Blob snapshot is unavailable");
            delete this;
            return;
        }
        std::move(callback).Run(length, modificationTime);
    }

    //using GetInternalUUIDCallback = base::OnceCallback<void(const WTF::String&)>;
    void GetInternalUUID(GetInternalUUIDCallback callback) override
    {
        content::printFuncName(__FUNCTION__, false, false);
        std::move(callback).Run(WTF::String::FromUTF8(m_uuid));
    }


    //BlobEntry* m_blobEntry = nullptr;
    std::string m_uuid;
    //BlobReceiver* m_cloneBlob = nullptr;


    mojo::Receiver<::blink::mojom::blink::Blob> m_receiver { this };
    base::WeakPtrFactory<BlobReceiver> m_weakFactory { this };
};

enum class DataElementType : uint32_t {
    kBytes,
    kFile,
    kBlob,
};

class DataElementBytesImpl {
public:
    uint64_t length; // 按理说这个长度要么=0，要么等于embeddedData的长度。不知道有没反例出现。不确定
    std::vector<uint8_t> embeddedData;
};

class DataElementFileImpl {
public:
    ::base::FilePath path;
    uint64_t offset;
    uint64_t length;
    absl::optional<::base::Time> expectedModificationTime;
};

class DataElementBlobImpl {
public:
    // Retain the actual child, not only its UUID, for sliced/nested blobs.
    ::mojo::Remote<::blink::mojom::blink::Blob> blob;
    std::string uuid;
    uint64_t offset;
    uint64_t length;
};

class DataElementImpl {
public:
    ~DataElementImpl()
    {
        switch (tag) {
        case DataElementType::kBytes: delete u.bytes; break;
        case DataElementType::kFile: delete u.file; break;
        case DataElementType::kBlob: delete u.blob; break;
        }
    }
    DataElementType tag;
    union {
        DataElementBytesImpl* bytes;
        DataElementFileImpl* file;
        DataElementBlobImpl* blob;
    } u;
};

namespace {

class BlobBufferSource final : public mojo::DataPipeProducer::DataSource {
public:
    explicit BlobBufferSource(std::vector<char> data) : m_data(std::move(data)) {}
    uint64_t GetLength() const override { return m_data.size(); }
    ReadResult Read(uint64_t offset, base::span<char> destination) override
    {
        ReadResult result;
        if (offset > m_data.size()) {
            result.result = MOJO_RESULT_OUT_OF_RANGE;
            return result;
        }
        result.bytes_read = (std::min)(destination.size(), m_data.size() - static_cast<size_t>(offset));
        if (result.bytes_read)
            memcpy(destination.data(), m_data.data() + offset, result.bytes_read);
        return result;
    }
private:
    const std::vector<char> m_data;
};

class BlobResponseLoader final : public network::mojom::URLLoader {
public:
    BlobResponseLoader(mojo::PendingReceiver<network::mojom::URLLoader> receiver,
        mojo::PendingRemote<network::mojom::URLLoaderClient> client)
        : m_receiver(this, std::move(receiver)), m_client(std::move(client))
    {
        m_receiver.set_disconnect_handler(base::BindOnce(
            [](BlobResponseLoader* self) { delete self; }, base::Unretained(this)));
    }

    void Start(BlobEntry* entry, const WTF::String& method, const net::HttpRequestHeaders& headers)
    {
        if (method != "GET" && method != "HEAD") {
            Complete(net::ERR_METHOD_NOT_SUPPORTED);
            return;
        }
        std::vector<char> buffer;
        int64_t totalSize = 0;
        if (!entry || entry->read(&buffer, 0, 0, &totalSize) < 0) {
            Complete(net::ERR_FILE_NOT_FOUND);
            return;
        }
        int64_t offset = 0;
        int64_t length = totalSize;
        auto rangeHeader = headers.GetHeader("Range");
        if (rangeHeader) {
            std::vector<net::HttpByteRange> ranges;
            if (!net::HttpUtil::ParseRangeHeader(*rangeHeader, &ranges)
                || ranges.size() != 1 || !ranges[0].ComputeBounds(totalSize)) {
                Complete(net::ERR_REQUEST_RANGE_NOT_SATISFIABLE);
                return;
            }
            offset = ranges[0].first_byte_position();
            length = ranges[0].last_byte_position() - offset + 1;
        }
        if (method == "GET" && entry->read(&buffer, offset, length, &totalSize) != length) {
            Complete(net::ERR_FAILED);
            return;
        }
        auto head = network::mojom::URLResponseHead::New();
        const std::string contentLength = std::to_string(length);
        const std::string contentRange = rangeHeader
            ? "bytes " + std::to_string(offset) + "-"
                + std::to_string(offset + length - 1) + "/" + std::to_string(totalSize)
            : std::string();
        net::HttpResponseHeaders::Builder builder(net::HttpVersion(1, 1),
            rangeHeader ? "206 Partial Content" : "200 OK");
        builder.AddHeader("Content-Type", entry->m_contentType);
        builder.AddHeader("Content-Length", contentLength);
        builder.AddHeader("Accept-Ranges", "bytes");
        if (rangeHeader) {
            builder.AddHeader("Content-Range", contentRange);
        }
        head->headers = builder.Build();
        head->mime_type = entry->m_contentType;
        head->content_length = length;
        mojo::ScopedDataPipeProducerHandle producer;
        mojo::ScopedDataPipeConsumerHandle consumer;
        if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
            Complete(net::ERR_INSUFFICIENT_RESOURCES);
            return;
        }
        m_client->OnReceiveResponse(std::move(head), std::move(consumer), absl::nullopt);
        m_bytes = buffer.size();
        m_producer = std::make_unique<mojo::DataPipeProducer>(std::move(producer));
        m_producer->Write(std::make_unique<BlobBufferSource>(std::move(buffer)),
            base::BindOnce([](BlobResponseLoader* self, MojoResult result) {
                self->Complete(result == MOJO_RESULT_OK ? net::OK : net::ERR_FAILED);
            }, base::Unretained(this)));
    }

    void FollowRedirect(const std::vector<std::string>&, const net::HttpRequestHeaders&,
        const net::HttpRequestHeaders&, const std::optional<GURL>&) override {}
    void SetPriority(net::RequestPriority, int32_t) override {}
    // The response comes from local blob storage, never a network connection.
    void PauseReadingBodyFromNet() override {}
    void ResumeReadingBodyFromNet() override {}

private:
    void Complete(int error)
    {
        m_producer.reset();
        network::URLLoaderCompletionStatus status(error);
        if (error == net::OK) {
            status.encoded_data_length = m_bytes;
            status.encoded_body_length = m_bytes;
            status.decoded_body_length = m_bytes;
        }
        m_client->OnComplete(status);
        delete this;
    }
    mojo::Receiver<network::mojom::URLLoader> m_receiver;
    mojo::Remote<network::mojom::URLLoaderClient> m_client;
    std::unique_ptr<mojo::DataPipeProducer> m_producer;
    uint64_t m_bytes = 0;
};

} // namespace

static void writeBlobToDataPipe(const std::string& uuid, mojo::ScopedDataPipeProducerHandle pipe,
    network::mojom::blink::DataPipeGetter::ReadCallback callback)
{
    BlobEntry* entry = BlobEntry::findByUuid(uuid);
    std::vector<char> buffer;
    int64_t totalSize = 0;
    if (!entry || !pipe.is_valid() || entry->read(&buffer, 0, -1, &totalSize) < 0) {
        std::move(callback).Run(net::ERR_FAILED, 0);
        return;
    }
    std::move(callback).Run(net::OK, buffer.size());
    auto producer = std::make_unique<mojo::DataPipeProducer>(std::move(pipe));
    auto* producerPtr = producer.get();
    producerPtr->Write(std::make_unique<BlobBufferSource>(std::move(buffer)),
        base::BindOnce([](std::unique_ptr<mojo::DataPipeProducer> producer, MojoResult) { producer.reset(); },
            std::move(producer)));
}

void BlobReceiver::ReadRange(uint64_t offset, uint64_t length,
    mojo::ScopedDataPipeProducerHandle pipe,
    mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> pendingClient)
{
    mojo::Remote<blink::mojom::blink::BlobReaderClient> client(std::move(pendingClient));
    BlobEntry* entry = BlobEntry::findByUuid(m_uuid);
    std::vector<char> buffer;
    int64_t totalSize = 0;
    const bool validRange = offset <= std::numeric_limits<int64_t>::max()
        && (length == std::numeric_limits<uint64_t>::max() || length <= std::numeric_limits<int64_t>::max());
    const int64_t bytesRead = entry && validRange
        ? entry->read(&buffer, offset,
            length == std::numeric_limits<uint64_t>::max() ? -1 : static_cast<int64_t>(length), &totalSize)
        : -1;
    if (bytesRead < 0 || !pipe.is_valid()) {
        if (client.is_bound())
            client->OnComplete(net::ERR_FAILED, 0);
        return;
    }
    if (client.is_bound())
        client->OnCalculatedSize(totalSize, bytesRead);
    auto producer = std::make_unique<mojo::DataPipeProducer>(std::move(pipe));
    auto* producerPtr = producer.get();
    producerPtr->Write(std::make_unique<BlobBufferSource>(std::move(buffer)),
        base::BindOnce([](std::unique_ptr<mojo::DataPipeProducer> producer,
            mojo::Remote<blink::mojom::blink::BlobReaderClient> client, uint64_t bytesRead, MojoResult result) {
            producer.reset();
            if (client.is_bound())
                client->OnComplete(result == MOJO_RESULT_OK ? net::OK : net::ERR_FAILED,
                    result == MOJO_RESULT_OK ? bytesRead : 0);
        }, std::move(producer), std::move(client), static_cast<uint64_t>(bytesRead)));
}

void BlobReceiver::ReadAll(mojo::ScopedDataPipeProducerHandle pipe,
    mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client)
{
    ReadRange(0, std::numeric_limits<uint64_t>::max(), std::move(pipe), std::move(client));
}

void BlobReceiver::Load(mojo::PendingReceiver<network::mojom::blink::URLLoader> loader,
    const WTF::String& method, const net::HttpRequestHeaders& headers,
    mojo::PendingRemote<network::mojom::blink::URLLoaderClient> client)
{
    auto* response = new BlobResponseLoader(
        mojo::PendingReceiver<network::mojom::URLLoader>(loader.PassPipe()),
        mojo::PendingRemote<network::mojom::URLLoaderClient>(client.PassPipe(), 0));
    response->Start(BlobEntry::findByUuid(m_uuid), method, headers);
}

void BlobReceiver::ReadSideData(ReadSideDataCallback callback)
{
    // Blob contents are not code-cache side data. This registry does not
    // attach side data; callers retrieve the body with ReadAll/ReadRange.
    std::move(callback).Run(absl::nullopt);
}


BlobEntry::BlobEntry(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const std::string& uuid, const std::string& contentType,
    const std::string& contentDisposition, WTF::Vector<::blink::mojom::blink::DataElementPtr> elements)
{
    m_blobReceiverRefCount++;
    m_uuid = uuid;
    m_contentType = contentType;
    m_contentDisposition = contentDisposition;

    for (size_t i = 0; i < elements.size(); ++i) {
        blink::mojom::blink::DataElementPtr ele(std::move(elements[i]));
        if (ele->is_bytes()) {
            DataElementImpl* dataEle = new DataElementImpl();
            dataEle->tag = DataElementType::kBytes;
            dataEle->u.bytes = new DataElementBytesImpl();
            dataEle->u.bytes->length = ele->get_bytes()->length;

            if (ele->get_bytes()->embedded_data.has_value() && ele->get_bytes()->embedded_data->size() > 0) {
                CHECK(ele->get_bytes()->embedded_data->size() == dataEle->u.bytes->length);
                dataEle->u.bytes->embeddedData.resize(ele->get_bytes()->embedded_data->size());
                memcpy(dataEle->u.bytes->embeddedData.data(), ele->get_bytes()->embedded_data.value().data(), ele->get_bytes()->embedded_data->size());
            } else if (ele->get_bytes()->data.is_valid()) {
                mojo::Remote<blink::mojom::blink::BytesProvider> provider(std::move(ele->get_bytes()->data));
                base::RunLoop runLoop(base::RunLoop::Type::kNestableTasksAllowed);
                provider.set_disconnect_handler(runLoop.QuitClosure());
                provider->RequestAsReply(base::BindOnce(
                    [](DataElementImpl* element, base::OnceClosure quit, const WTF::Vector<uint8_t>& bytes) {
                        element->u.bytes->embeddedData.assign(bytes.begin(), bytes.end());
                        std::move(quit).Run();
                    }, dataEle, runLoop.QuitClosure()));
                runLoop.Run();
            }
            //CHECK(!(ele->get_bytes()->data.is_valid())); // 这个ele->get_bytes()->data暂时没搞明白有啥用

            m_elements.push_back(dataEle);
        } else if (ele->is_blob()) {
            DataElementImpl* dataEle = new DataElementImpl();
            dataEle->tag = DataElementType::kBlob;
            dataEle->u.blob = new DataElementBlobImpl();

            dataEle->u.blob->blob.Bind(std::move(ele->get_blob()->blob));
            dataEle->u.blob->blob->GetInternalUUID(base::BindOnce(
                [](DataElementImpl* element, const WTF::String& uuid) {
                    element->u.blob->uuid = uuid.Utf8();
                }, dataEle));

            dataEle->u.blob->offset = ele->get_blob()->offset;
            dataEle->u.blob->length = ele->get_blob()->length;
            m_elements.push_back(dataEle);
        } else if (ele->is_file()) {
            DataElementImpl* dataEle = new DataElementImpl();
            dataEle->tag = DataElementType::kFile;
            dataEle->u.file = new DataElementFileImpl();
            dataEle->u.file->path = ele->get_file()->path;
            dataEle->u.file->offset = ele->get_file()->offset;
            dataEle->u.file->length = ele->get_file()->length;
            dataEle->u.file->expectedModificationTime = ele->get_file()->expected_modification_time;
            m_elements.push_back(dataEle);
        }
    }
    new BlobReceiver(blob.PassPipe(), uuid);
}

BlobEntry::~BlobEntry()
{
    for (size_t i = 0; i < m_elements.size(); ++i) {
        DataElementImpl* ele = m_elements[i];
        delete ele;
    }
}


BlobEntry* BlobEntry::findByUuid(const std::string& uuid)
{
    BlobEntry* blobEntry = nullptr;
    std::map<std::string, BlobEntry*>::const_iterator it = s_blobEntrys->find(uuid);
    if (it != s_blobEntrys->end())
        blobEntry = it->second;
    return blobEntry;
}

namespace {

// A zero-length read obtains metadata without materializing the blob. Every
// range is relative to the logical blob, including sliced child/file elements.
template<typename Byte>
int64_t ReadBlobRange(BlobEntry* source, std::vector<Byte>* buffer,
    int64_t start, int64_t len, int64_t* allSize)
{
    if (!buffer || !allSize || start < 0 || len < -1)
        return -1;
    const size_t original_size = buffer->size();
    auto visit = [&](auto&& visit, BlobEntry* entry, int64_t offset,
                     int64_t requested, int64_t* total, unsigned depth) -> int64_t {
        if (depth > 128)
            return -1;
        *total = 0;
        int64_t received = 0;
        for (DataElementImpl* element : entry->m_elements) {
            int64_t size = 0;
            uint64_t source_offset = 0;
            BlobEntry* child = nullptr;
            base::File file;
            if (element->tag == DataElementType::kBytes) {
                if (element->u.bytes->embeddedData.size()
                    != element->u.bytes->length)
                    return -1;
                if (element->u.bytes->length > std::numeric_limits<int64_t>::max())
                    return -1;
                size = static_cast<int64_t>(element->u.bytes->length);
            } else if (element->tag == DataElementType::kBlob) {
                child = BlobEntry::findByUuid(element->u.blob->uuid);
                int64_t child_size = 0;
                if (!child || visit(visit, child, 0, 0, &child_size, depth + 1) < 0)
                    return -1;
                source_offset = element->u.blob->offset;
                if (source_offset > static_cast<uint64_t>(child_size))
                    return -1;
                size = static_cast<int64_t>((std::min)(
                    element->u.blob->length,
                    static_cast<uint64_t>(child_size) - source_offset));
            } else if (element->tag == DataElementType::kFile) {
                file = base::File(standardizationFilePath(element->u.file->path),
                    base::File::FLAG_READ | base::File::FLAG_OPEN);
                base::File::Info info;
                if (!file.IsValid() || !file.GetInfo(&info) || info.size < 0
                    || (element->u.file->expectedModificationTime
                        && *element->u.file->expectedModificationTime != info.last_modified))
                    return -1;
                source_offset = element->u.file->offset;
                if (source_offset > static_cast<uint64_t>(info.size))
                    return -1;
                size = static_cast<int64_t>((std::min)(
                    element->u.file->length,
                    static_cast<uint64_t>(info.size) - source_offset));
            } else {
                return -1;
            }
            if (size > std::numeric_limits<int64_t>::max() - *total)
                return -1;
            *total += size;
            if (offset >= size) {
                offset -= size;
                continue;
            }
            const int64_t count = requested == -1
                ? size - offset : (std::min)(size - offset, requested - received);
            if (count > 0) {
                if (child) {
                    int64_t ignored_size = 0;
                    if (visit(visit, child, source_offset + offset, count,
                            &ignored_size, depth + 1) != count)
                        return -1;
                } else {
                    if (static_cast<uint64_t>(count) > buffer->max_size() - buffer->size())
                        return -1;
                    size_t destination = buffer->size();
                    buffer->resize(destination + static_cast<size_t>(count));
                    if (element->tag == DataElementType::kBytes) {
                        memcpy(buffer->data() + destination,
                            element->u.bytes->embeddedData.data() + offset,
                            static_cast<size_t>(count));
                    } else {
                        int64_t remaining = count;
                        int64_t position = source_offset + offset;
                        while (remaining) {
                            int chunk = static_cast<int>((std::min)(
                                remaining, static_cast<int64_t>(std::numeric_limits<int>::max())));
                            int read = file.Read(position,
                                reinterpret_cast<char*>(buffer->data() + destination), chunk);
                            if (read <= 0)
                                return -1;
                            position += read;
                            destination += read;
                            remaining -= read;
                        }
                    }
                }
                received += count;
            }
            offset = 0;
        }
        return received;
    };
    int64_t result = visit(visit, source, start, len, allSize, 0);
    if (result < 0)
        buffer->resize(original_size);
    return result;
}

} // namespace

int64_t BlobEntry::read(std::vector<char>* buffer, int64_t start, int64_t len, int64_t* allSize)
{
    return ReadBlobRange(this, buffer, start, len, allSize);
}

int64_t BlobEntry::read(std::vector<uint8_t>* buffer, int64_t start, int64_t len, int64_t* allSize)
{
    return ReadBlobRange(this, buffer, start, len, allSize);
}

//-----

BlobRegistryImpl::BlobRegistryImpl()
{
    if (!s_blobEntrys)
        s_blobEntrys = new std::map<std::string, BlobEntry*>();
}

BlobRegistryImpl::~BlobRegistryImpl()
{
    //OutputDebugStringA("~BlobRegistryImpl\n");
}

bool BlobRegistryImpl::Register(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const WTF::String& uuid, const WTF::String& contentType,
    const WTF::String& contentDisposition, WTF::Vector<::blink::mojom::blink::DataElementPtr> elements)
{
    //content::printFuncName(__FUNCTION__, true, false);
    //CHECK(elements.size() == 1 || elements.size() == 0);

    std::map<std::string, BlobEntry*>::const_iterator it = s_blobEntrys->find(uuid.Ascii());
    CHECK(it == s_blobEntrys->end());
    ((*s_blobEntrys)[uuid.Ascii()]) = (new BlobEntry(std::move(blob), uuid.Ascii(), contentType.Ascii(), contentDisposition.Ascii(), std::move(elements)));

    return true;
}

void BlobRegistryImpl::Register(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const WTF::String& uuid, const WTF::String& content_type,
    const WTF::String& content_disposition, WTF::Vector<::blink::mojom::blink::DataElementPtr> elements,
    ::blink::mojom::blink::BlobRegistry::RegisterCallback callback)
{
    if (Register(std::move(blob), uuid, content_type, content_disposition, std::move(elements)))
        std::move(callback).Run();
}

struct RegisterFromStreamAsyncCall {
    blink::mojom::blink::BlobRegistry::RegisterFromStreamCallback callback;
    mojo::ScopedDataPipeConsumerHandle data;
    mojo::AssociatedRemote<blink::mojom::blink::ProgressClient> progress;
    WTF::String contentType;
    scoped_refptr<blink::RawData> bytes = blink::RawData::Create();
    mojo::SimpleWatcher watcher { FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC };

    void start()
    {
        if (!data.is_valid()) {
            finish(false);
            return;
        }
        watcher.Watch(data.get(), MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
            base::BindRepeating(&RegisterFromStreamAsyncCall::onReady, base::Unretained(this)));
    }

    void onReady(MojoResult, const mojo::HandleSignalsState&)
    {
        for (;;) {
            const void* buffer = nullptr;
            uint32_t size = 0;
            MojoResult result = data->BeginReadData(&buffer, &size, MOJO_BEGIN_READ_DATA_FLAG_NONE);
            if (result == MOJO_RESULT_SHOULD_WAIT)
                return;
            if (result != MOJO_RESULT_OK) {
                finish(result == MOJO_RESULT_FAILED_PRECONDITION);
                return;
            }
            bytes->MutableData()->Append(static_cast<const char*>(buffer), size);
            data->EndReadData(size);
            if (progress.is_bound())
                progress->OnProgress(size);
        }
    }

    void finish(bool success)
    {
        watcher.Cancel();
        data.reset();
        scoped_refptr<blink::BlobDataHandle> handle;
        if (success) {
            auto blobData = std::make_unique<blink::BlobData>(blink::BlobData::FileCompositionStatus::kNoUnknownSizeFiles);
            blobData->SetContentType(contentType);
            const uint64_t size = bytes->MutableData()->size();
            blobData->AppendData(bytes);
            handle = blink::BlobDataHandle::Create(std::move(blobData), size);
        }
        std::move(callback).Run(handle);
        base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
    }
};

//using RegisterFromStreamCallback = base::OnceCallback<void(const ::scoped_refptr<::blink::BlobDataHandle>&)>;
void BlobRegistryImpl::RegisterFromStream(const WTF::String& contentType, const WTF::String& contentDisposition, uint64_t lengthHint,
    ::mojo::ScopedDataPipeConsumerHandle data, ::mojo::PendingAssociatedRemote<::blink::mojom::blink::ProgressClient> progressClient,
    RegisterFromStreamCallback callback)
{
    RegisterFromStreamAsyncCall* asyncCall = new RegisterFromStreamAsyncCall();
    asyncCall->callback = std::move(callback);
    asyncCall->data = std::move(data);
    asyncCall->contentType = contentType;
    if (progressClient.is_valid())
        asyncCall->progress.Bind(std::move(progressClient));
    asyncCall->start();
}

// bool BlobRegistryImpl::GetBlobFromUUID(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const WTF::String& uuid)
// {
//     content::printFuncName(__FUNCTION__, true, true);
//     return false;
// }
// 
// //using GetBlobFromUUIDCallback = base::OnceCallback<void()>;
// void BlobRegistryImpl::GetBlobFromUUID(
//     ::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const WTF::String& uuid, ::blink::mojom::blink::BlobRegistry::GetBlobFromUUIDCallback callback)
// {
//     content::printFuncName(__FUNCTION__, true, true);
// }
// 
// void BlobRegistryImpl::URLStoreForOrigin(
//     const ::scoped_refptr<const ::blink::SecurityOrigin>& origin, ::mojo::PendingAssociatedReceiver<::blink::mojom::blink::BlobURLStore> urlStore)
// {
//     //content::printFuncName(__FUNCTION__, true, true);
// 
//     //m_blobURLStoreReceiver = new mojo::AssociatedReceiver<::blink::mojom::blink::BlobURLStore>(new BlobURLStoreImpl(origin));
//     //m_blobURLStoreReceiver->Bind(std::move(url_store));
// 
//     createAndBindInterface<::blink::mojom::blink::BlobURLStore, BlobURLStoreImpl>(std::move(urlStore.PassPipe()), origin);
// }

}