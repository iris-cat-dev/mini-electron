
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
    content::printFuncName(__FUNCTION__, true, true);
    return false;
}

bool ::blink::mojom::blink::BlobRegistry::Register(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const WTF::String& uuid,
    const WTF::String& content_type, const WTF::String& content_disposition, WTF::Vector<::blink::mojom::blink::DataElementPtr> elements)
{
    content::printFuncName(__FUNCTION__, true, true);
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
    content::printFuncName(__FUNCTION__, true, true);
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

class BlobReceiver : public ::blink::mojom::blink::Blob {
public:
    BlobReceiver(/*BlobEntry* blobEntry*/ const std::string& uuid)
    {
        //m_blobEntry = blobEntry;
        m_uuid = uuid;

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

        createAndBindInterface<::blink::mojom::blink::Blob, BlobReceiver>(blob.PassPipe(), m_uuid);
    }

    class DataPipeGetterImpl : public ::network::mojom::blink::DataPipeGetter {
    public:
        DataPipeGetterImpl(std::string uuid)
        {
            m_uuid = uuid;
        }

        void Read(::mojo::ScopedDataPipeProducerHandle pipe, ReadCallback callback) override
        {
            BlobEntry* blobEntry = BlobEntry::findByUuid(m_uuid);
            if (!blobEntry) {
                pipe->WriteData(nullptr, 0, MOJO_WRITE_DATA_FLAG_NONE);
                std::move(callback).Run(0, 0);
            } else {
                std::vector<char> buffer;
                int64_t allSize = 0;
                blobEntry->read(&buffer, 0, -1, &allSize);

                uint32_t numBytes = buffer.size();
                pipe->WriteData(buffer.data(), &numBytes, MOJO_WRITE_DATA_FLAG_NONE);
                std::move(callback).Run(numBytes, numBytes);
            }
        }

        void Clone(::mojo::PendingReceiver<DataPipeGetter> receiver) override
        {
            createAndBindInterface<::network::mojom::blink::DataPipeGetter, DataPipeGetterImpl>(receiver.PassPipe(), m_uuid);
        }

    private:
        std::string m_uuid;
    };

    void AsDataPipeGetter(::mojo::PendingReceiver<::network::mojom::blink::DataPipeGetter> dataPipeGetter) override
    {
        content::printFuncName(__FUNCTION__, true, false); // !!!
        CHECK(!m_uuid.empty());
        createAndBindInterface<::network::mojom::blink::DataPipeGetter, DataPipeGetterImpl>(dataPipeGetter.PassPipe(), m_uuid);
    }

    void ReadAll(::mojo::ScopedDataPipeProducerHandle pipe, ::mojo::PendingRemote<::blink::mojom::blink::BlobReaderClient> client/*, bool isSync*/) override;

    void ReadRange(uint64_t offset, uint64_t length, ::mojo::ScopedDataPipeProducerHandle pipe,
        ::mojo::PendingRemote<::blink::mojom::blink::BlobReaderClient> client) override
    {
        content::printFuncName(__FUNCTION__, true, true);
    }

    void Load(::mojo::PendingReceiver<::network::mojom::blink::URLLoader> loader, const WTF::String& request_method, const ::net::HttpRequestHeaders& headers,
        ::mojo::PendingRemote<::network::mojom::blink::URLLoaderClient> client) override
    {
        content::printFuncName(__FUNCTION__, true, true);
    }

    //using ReadSideDataCallback = base::OnceCallback<void(absl::optional<::mojo_base::BigBuffer>)>;
    void ReadSideData(ReadSideDataCallback callback) override;

    bool CaptureSnapshot(uint64_t* out_length, absl::optional<::base::Time>* out_modification_time) override
    {
        content::printFuncName(__FUNCTION__, true, true);
        return false;
    }

    //using CaptureSnapshotCallback = base::OnceCallback<void(uint64_t, absl::optional<::base::Time>)>;
    void CaptureSnapshot(CaptureSnapshotCallback callback) override
    {
        content::printFuncName(__FUNCTION__, true, true);
    }

    //using GetInternalUUIDCallback = base::OnceCallback<void(const WTF::String&)>;
    void GetInternalUUID(GetInternalUUIDCallback callback) override
    {
        content::printFuncName(__FUNCTION__, false, false);
        std::move(callback).Run(WTF::String::FromUTF8(m_uuid));
    }

    void onReadAllFinish(uint64_t totalSize);

    //BlobEntry* m_blobEntry = nullptr;
    std::string m_uuid;
    //BlobReceiver* m_cloneBlob = nullptr;

    ::mojo::ScopedDataPipeProducerHandle m_pipe;
    ::mojo::Remote<::blink::mojom::blink::BlobReaderClient> m_client;

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

void BlobReceiver::onReadAllFinish(uint64_t totalSize)
{
    if (m_client.is_bound() && m_client.TryGet())
        m_client->OnCalculatedSize(totalSize, totalSize);

    if (m_client.is_bound() && m_client.TryGet())
        m_client->OnComplete(0, totalSize);

    if (m_client.is_bound())
        m_client.Unbind();
}

void BlobReceiver::ReadAll(::mojo::ScopedDataPipeProducerHandle pipe, ::mojo::PendingRemote<::blink::mojom::blink::BlobReaderClient> client/*, bool isSync*/)
{
    bool isSync = false;
    m_pipe = std::move(pipe);

    bool needBind = false;
    if (m_client.is_bound()) {
        if (client.is_valid() && (client.Pipe().get().value() != m_client.internal_state()->handle().value())) {
            m_client.Unbind();
            needBind = true;
        }
    } else if (client.is_valid()) {
        needBind = true;
    }

    if (needBind)
        m_client.Bind(std::move(client));

    uint64_t totalSize = 0;

    BlobEntry* blobEntry = BlobEntry::findByUuid(m_uuid);

    for (size_t i = 0; blobEntry && i < blobEntry->m_elements.size(); ++i) {
        DataElementImpl* ele = blobEntry->m_elements[i];
        if (ele->tag == DataElementType::kBytes) {
            totalSize += ele->u.bytes->length;

            uint32_t numBytes = ele->u.bytes->embeddedData.size();
            m_pipe->WriteData(ele->u.bytes->embeddedData.data(), &numBytes, 0);
        } else if (ele->tag == DataElementType::kFile) {
            std::string contents;
            if (!base::ReadFileToString(standardizationFilePath(ele->u.file->path), &contents))
                continue;

            uint32_t numBytes = ele->u.file->length;
            if (ele->u.file->offset >= contents.size())
                continue;
            if (ele->u.file->offset + numBytes >= contents.size())
                numBytes = contents.size() - ele->u.file->offset;
            totalSize += numBytes;

            m_pipe->WriteData(contents.c_str(), &numBytes, 0);
        } else if (ele->tag == DataElementType::kBlob) {
            DataElementBlobImpl* blob = ele->u.blob;
            BlobEntry* childBlobEntry = BlobEntry::findByUuid(blob->uuid);
            if (!childBlobEntry)
                continue;
            std::vector<char> buffer;
            int64_t allSize = 0;
            childBlobEntry->read(&buffer, blob->offset, blob->length, &allSize);

            uint32_t numBytes = buffer.size();
            m_pipe->WriteData(buffer.data(), &numBytes, 0);
            totalSize += numBytes;
        } else
            DebugBreak();
    }

    if (isSync)
        onReadAllFinish(totalSize);
    else
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(&BlobReceiver::onReadAllFinish, m_weakFactory.GetWeakPtr(), totalSize));
}

void BlobReceiver::ReadSideData(ReadSideDataCallback callback)
{
    // 目前只在LocalFrameHostImpl::DownloadURL里用到，所以本函数简化了实现
    uint64_t totalSize = 0;

    BlobEntry* blobEntry = BlobEntry::findByUuid(m_uuid);
    CHECK(blobEntry->m_elements.size() == 1);

    for (size_t i = 0; blobEntry && i < blobEntry->m_elements.size(); ++i) {
        DataElementImpl* ele = blobEntry->m_elements[i];
        if (ele->tag == DataElementType::kBytes) {
            totalSize += ele->u.bytes->length;

            uint32_t numBytes = ele->u.bytes->embeddedData.size();
            ::mojo_base::BigBuffer buf(base::span<const uint8_t>(ele->u.bytes->embeddedData.data(), numBytes));
            std::optional<::mojo_base::BigBuffer> bufOpt(std::move(buf));
            std::move(callback).Run(std::move(bufOpt));
        } else {
            CHECK(false);
        }
    }
}

struct WaitForDataInfo {
    ::mojo::Remote<blink::mojom::blink::BytesProvider> bytesProvider;
    DataElementImpl* dataEle = nullptr;
    bool waitCount = 0;

    void waitForData()
    {
        scoped_refptr<base::SequencedTaskRunner> runner = bytesProvider.GetReceiverTaskRunner();
        runner->PostTask(FROM_HERE, base::BindOnce([](WaitForDataInfo* self) { self->waitForDataImpl(); }, this));

        while (waitCount == 0) {
            ::Sleep(1);
        }
    }

private:
    void waitForDataImpl()
    {
        bytesProvider->RequestAsReply(base::BindOnce(
            [](WaitForDataInfo* self, const WTF::Vector<uint8_t>& bytes) {
                self->dataEle->u.bytes->embeddedData.resize(bytes.size());
                memcpy(self->dataEle->u.bytes->embeddedData.data(), bytes.data(), bytes.size());
                self->waitCount = 1;
            },
            this));
    }
};

BlobEntry::BlobEntry(::mojo::PendingReceiver<::blink::mojom::blink::Blob> blob, const std::string& uuid, const std::string& contentType,
    const std::string& contentDisposition, WTF::Vector<::blink::mojom::blink::DataElementPtr> elements)
{
    createAndBindInterface<::blink::mojom::blink::Blob, BlobReceiver>(blob.PassPipe(), uuid);
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
                WaitForDataInfo* info = new WaitForDataInfo();
                info->dataEle = dataEle;
                info->bytesProvider.Bind(std::move(ele->get_bytes()->data));
                info->waitForData();
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
    content::printFuncName(__FUNCTION__, true, true);
}

struct RegisterFromStreamAsyncCall {
    ::blink::mojom::blink::BlobRegistry::RegisterFromStreamCallback callback;
    ::mojo::ScopedDataPipeConsumerHandle data;
    WTF::String contentType;
    mojo::SimpleWatcher dataPipeCloseWatcher;

    RegisterFromStreamAsyncCall()
        : dataPipeCloseWatcher(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC)
    {
    }

    void start()
    {
        dataPipeCloseWatcher.Watch(data.get(), MOJO_HANDLE_SIGNAL_PEER_CLOSED, MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
            base::BindRepeating(&RegisterFromStreamAsyncCall::OnPeerClosed, base::Unretained(this)));
    }

    void OnPeerClosed(MojoResult result, const mojo::HandleSignalsState& state)
    {
        const char* buffer = nullptr;
        uint32_t bufferNumBytes = 0;
        data->BeginReadData((const void**)(&buffer), &bufferNumBytes, MOJO_READ_DATA_FLAG_NONE); // 不需要end，因为这里把数据都读完了

        std::unique_ptr<blink::BlobData> blobData = base::WrapUnique(new blink::BlobData(blink::BlobData::FileCompositionStatus::kNoUnknownSizeFiles));
        blobData->SetContentType(contentType);
        scoped_refptr<blink::RawData> rawData = blink::RawData::Create();
        rawData->MutableData()->Append(buffer, bufferNumBytes);
        blobData->AppendData(rawData);
        scoped_refptr<blink::BlobDataHandle> blobDataHandle = blink::BlobDataHandle::Create(std::move(blobData), bufferNumBytes);

        RegisterFromStreamAsyncCall* self = this;
        std::move(self->callback).Run(blobDataHandle);

        ThreadCall::callBlinkThreadAsync(FROM_HERE, [self] { // 感觉异步删除比较好一点
            delete self;
        });
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