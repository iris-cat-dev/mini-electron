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

#include "third_party/blink/renderer/platform/blob/blob_data.h"

#include <memory>
#include <utility>
#include <algorithm>
#include <limits>
#include <variant>
#include <vector>

#include "base/check_is_test.h"
#include "base/functional/callback_helpers.h"
#include "base/files/file.h"
#include "base/containers/span.h"
#include "base/memory/ptr_util.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#if defined(MINI_UI_QUICKJS)
#include "runtime/ui/local_resources.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "base/task/sequenced_task_runner.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/net_errors.h"
#include "services/network/public/mojom/url_loader.mojom-blink.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/platform/cross_variant_mojo_util.h"
#endif
#include "services/network/public/mojom/url_response_head.mojom-blink.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/data_pipe_getter.mojom-blink.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#endif
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/blob_registry.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/file_backed_blob_factory.mojom-blink.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/public/platform/platform.h"
#endif
#include "third_party/blink/public/platform/file_path_conversion.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/renderer/platform/blob/blob_bytes_provider.h"
#include "third_party/blink/renderer/platform/instrumentation/tracing/trace_event.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/text/line_ending.h"
#include "third_party/blink/renderer/platform/wtf/text/text_encoding.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/platform/wtf/thread_specific.h"
#endif
#include "third_party/blink/renderer/platform/wtf/uuid.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

void base::internal::check_is_test_impl(base::NotFatalUntil)
{
    *(int*)1 = 1;
}

namespace blink {

using mojom::blink::BytesProvider;
using mojom::blink::DataElement;
using mojom::blink::DataElementBlob;
using mojom::blink::DataElementBytes;
using mojom::blink::DataElementBytesPtr;
using mojom::blink::DataElementFile;
using mojom::blink::DataElementPtr;
#if defined(MINI_UI_QUICKJS)
void BindNativeBlobRegistry(
    mojo::PendingReceiver<mojom::blink::BlobRegistry>);
#endif

namespace {

// http://dev.w3.org/2006/webapi/FileAPI/#constructorBlob
bool IsValidBlobType(const String& type)
{
    for (unsigned i = 0; i < type.length(); ++i) {
        UChar c = type[i];
        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return true;
}

mojom::blink::BlobRegistry* g_blob_registry_for_testing = nullptr;

mojom::blink::BlobRegistry* GetThreadSpecificRegistry()
{
    if (g_blob_registry_for_testing) [[unlikely]]
        return g_blob_registry_for_testing;
#if defined(MINI_UI_QUICKJS)
    static mojo::Remote<mojom::blink::BlobRegistry> registry;
    if (!registry.is_bound()) {
        mojo::PendingReceiver<mojom::blink::BlobRegistry> receiver =
            registry.BindNewPipeAndPassReceiver();
        BindNativeBlobRegistry(std::move(receiver));
    }
    return registry.get();
#else
    DEFINE_THREAD_SAFE_STATIC_LOCAL(ThreadSpecific<mojo::Remote<mojom::blink::BlobRegistry>>, registry, ());
    if (!registry.IsSet()) [[unlikely]]
        Platform::Current()->GetBrowserInterfaceBroker()->GetInterface((*registry).BindNewPipeAndPassReceiver());
    return registry->get();
#endif
}

} // namespace

#if defined(MINI_UI_QUICKJS)
static const scoped_refptr<base::SingleThreadTaskRunner>&
NativeBlobServiceTaskRunner()
{
    static const scoped_refptr<base::SingleThreadTaskRunner> runner =
        base::ThreadPool::CreateSingleThreadTaskRunner(
            {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
    return runner;
}


class NativeBlobData {
public:
    struct Element {
        enum class Kind { kBytes, kFile, kBlob };
        Kind kind = Kind::kBytes;
        uint64_t length = 0;
        scoped_refptr<RawData> bytes;
        base::File file;
        int file_status = net::ERR_ACCESS_DENIED;
        uint64_t file_offset = 0;
        std::optional<base::Time> expected_modification_time;
        scoped_refptr<NativeBlobStorage> blob;
        mojo::PendingRemote<mojom::blink::Blob> blob_remote;
        uint64_t blob_offset = 0;
        mojo::PendingRemote<mojom::blink::BytesProvider> bytes_provider;
    };
    Vector<Element> elements;
};

// Metadata and data reads must use the same authorized, owned handle.
static int NativeFileInfo(const NativeBlobData::Element& element,
    base::File::Info* info)
{
    if (element.file_status != net::OK)
        return element.file_status;
    if (!element.file.IsValid() || !element.file.GetInfo(info) ||
        info->is_directory || info->size < 0)
        return net::ERR_FAILED;
    if (element.expected_modification_time &&
        info->last_modified != *element.expected_modification_time)
        return net::ERR_UPLOAD_FILE_CHANGED;
    return net::OK;
}


using NativeReadCallback =
    base::OnceCallback<void(int, uint64_t, Vector<uint8_t>)>;
class NativeBlobReadJob;
void CancelNativeBlobReadJob(NativeBlobReadJob*);

struct NativeBlobStorageTraits {
    static void Destruct(const NativeBlobStorage*);
};

class NativeBlobStorage
    : public ThreadSafeRefCounted<NativeBlobStorage, NativeBlobStorageTraits> {
public:
    NativeBlobStorage(const String& uuid, const String& type, uint64_t size,
        std::unique_ptr<NativeBlobData> data)
        : uuid_(uuid), type_(type), declared_size_(size)
    {
        for (auto& source : data->elements) {
            StoredElement element;
            static_cast<NativeBlobData::Element&>(element) =
                std::move(source);
            elements_.push_back(std::move(element));
        }
    }
    const String& uuid() const { return uuid_; }
    const String& type() const { return type_; }
    uint64_t declared_size() const { return declared_size_; }
    uint64_t GetSize(int* status) const
    {
        return CalculateSize(status);
    }
    void ReadRange(uint64_t, uint64_t, NativeReadCallback,
        NativeBlobReadJob**);
    bool CaptureSnapshot(uint64_t*, std::optional<base::Time>*);

private:
    friend struct NativeBlobStorageTraits;
    friend class NativeBlobReadJob;
    struct StoredElement : NativeBlobData::Element {
        mojo::Remote<mojom::blink::Blob> remote;
        mojo::Remote<mojom::blink::BytesProvider> provider;
        std::optional<Vector<uint8_t>> provider_bytes;
        bool provider_request_pending = false;
        Vector<base::OnceCallback<void(bool)>> provider_waiters;
    };
    ~NativeBlobStorage() = default;
    uint64_t CalculateSize(int*) const;
    void ResolveProvider(wtf_size_t, base::OnceCallback<void(bool)>);
    const String uuid_;
    const String type_;
    const uint64_t declared_size_;
    Vector<StoredElement> elements_;
};

void NativeBlobStorageTraits::Destruct(const NativeBlobStorage* storage)
{
    auto& runner = NativeBlobServiceTaskRunner();
    if (!runner->RunsTasksInCurrentSequence()) {
        CHECK(runner->PostTask(FROM_HERE, base::BindOnce(
            [](const NativeBlobStorage* storage) {
                NativeBlobStorageTraits::Destruct(storage);
            },
            base::Unretained(storage))));
        return;
    }

    // Releasing a local blob element can release its child storage. Drain
    // those zero-refcount children iteratively instead of recursing through a
    // deeply composed blob graph.
    static thread_local bool draining = false;
    static thread_local std::vector<const NativeBlobStorage*> pending;
    pending.push_back(storage);
    if (draining)
        return;
    draining = true;
    while (!pending.empty()) {
        const NativeBlobStorage* next = pending.back();
        pending.pop_back();
        delete next;
    }
    draining = false;
}

// Only one bounded read buffer is outstanding, including for nested file blobs.
class NativeBlobWriter {
public:
    using Callback = base::OnceCallback<void(int, uint64_t)>;
    NativeBlobWriter(scoped_refptr<NativeBlobStorage> storage,
        uint64_t offset, uint64_t length,
        mojo::ScopedDataPipeProducerHandle pipe, Callback callback)
        : storage_(std::move(storage)), offset_(offset), remaining_(length),
          pipe_(std::move(pipe)), callback_(std::move(callback)),
          watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC)
    {
        UpdateWatch();
    }
    void Pause()
    {
        if (paused_)
            return;
        paused_ = true;
        UpdateWatch();
    }
    void Resume()
    {
        if (!paused_)
            return;
        paused_ = false;
        UpdateWatch();
    }
    void Cancel()
    {
        StopWatching();
        callback_.Reset();
        if (read_job_)
            CancelNativeBlobReadJob(read_job_);
        delete this;
    }
private:
    void StopWatching()
    {
        watcher_.Cancel();
    }
    void UpdateWatch()
    {
        StopWatching();
        const MojoHandleSignals signals =
            paused_ || reading_
            ? MOJO_HANDLE_SIGNAL_PEER_CLOSED
            : MOJO_HANDLE_SIGNAL_WRITABLE |
                MOJO_HANDLE_SIGNAL_PEER_CLOSED;
        watcher_.Watch(pipe_.get(), signals,
            MOJO_WATCH_CONDITION_SATISFIED,
            base::BindRepeating(&NativeBlobWriter::OnWritable,
                weak_factory_.GetWeakPtr()));
    }
    void OnWritable(MojoResult result, const mojo::HandleSignalsState&)
    {
        // While paused or reading, the watcher is armed only for peer
        // closure. This keeps cancellation observable without writable-spin.
        if (result != MOJO_RESULT_OK || paused_ || reading_)
            return Finish(net::ERR_ABORTED);
        while (buffer_written_ < bytes_.size()) {
            size_t count = 0;
            MojoResult write_result = pipe_->WriteData(
                base::span(bytes_).subspan(buffer_written_),
                MOJO_WRITE_DATA_FLAG_NONE, count);
            if (write_result == MOJO_RESULT_SHOULD_WAIT)
                return;
            if (write_result != MOJO_RESULT_OK)
                return Finish(net::ERR_ABORTED);
            buffer_written_ += count;
            written_ += count;
        }
        if (!remaining_)
            return Finish(net::OK);
        bytes_.clear();
        buffer_written_ = 0;
        reading_ = true;
        UpdateWatch();
        pending_count_ = std::min<uint64_t>(remaining_, 64 * 1024);
        storage_->ReadRange(offset_, pending_count_,
            base::BindOnce(&NativeBlobWriter::ReadFinished,
                weak_factory_.GetWeakPtr()),
            &read_job_);
    }
    void ReadFinished(int status, uint64_t, Vector<uint8_t> bytes)
    {
        reading_ = false;
        if (status != net::OK)
            return Finish(status);
        if (bytes.size() != pending_count_)
            return Finish(net::ERR_FAILED);
        offset_ += pending_count_;
        remaining_ -= pending_count_;
        bytes_ = std::move(bytes);
        UpdateWatch();
    }
    void Finish(int status)
    {
        StopWatching();
        Callback callback = std::move(callback_);
        uint64_t written = written_;
        if (read_job_)
            CancelNativeBlobReadJob(read_job_);
        delete this;
        if (callback)
            std::move(callback).Run(status, written);
    }
    scoped_refptr<NativeBlobStorage> storage_;
    uint64_t offset_;
    uint64_t remaining_;
    uint64_t pending_count_ = 0;
    Vector<uint8_t> bytes_;
    mojo::ScopedDataPipeProducerHandle pipe_;
    Callback callback_;
    mojo::SimpleWatcher watcher_;
    uint64_t written_ = 0;
    size_t buffer_written_ = 0;
    bool reading_ = false;
    bool paused_ = false;
    NativeBlobReadJob* read_job_ = nullptr;
    base::WeakPtrFactory<NativeBlobWriter> weak_factory_{this};
};

class NativeBlobReadJob final : public mojom::blink::BlobReaderClient,
                                public mojo::DataPipeDrainer::Client {
public:
    NativeBlobReadJob(scoped_refptr<NativeBlobStorage> storage,
        uint64_t offset, uint64_t length, uint64_t total,
        NativeReadCallback callback, NativeBlobReadJob** owner_slot)
        : total_size_(total), callback_(std::move(callback)),
          nested_client_receiver_(this), owner_slot_(owner_slot)
    {
        const uint64_t requested_offset = std::min(offset, total);
        frames_.push_back(Frame(std::move(storage), requested_offset,
            std::min(length, total - requested_offset)));
    }
    void Start() { ProcessNext(); }
    void Cancel()
    {
        callback_.Reset();
        DetachOwner();
        delete this;
    }
    void OnCalculatedSize(uint64_t, uint64_t) override {}
    void OnComplete(int32_t status, uint64_t data_length) override
    {
        nested_status_ =
            status == net::OK &&
                    data_length != nested_expected_count_
                ? net::ERR_FAILED
                : status;
        nested_client_complete_ = true;
        MaybeCompleteNestedRead();
    }
    void OnDataAvailable(base::span<const uint8_t> data) override
    {
        output_.AppendSpan(data);
    }
    void OnDataComplete() override
    {
        nested_data_complete_ = true;
        MaybeCompleteNestedRead();
    }

private:
    struct Frame {
        Frame(scoped_refptr<NativeBlobStorage> storage_in,
            uint64_t offset, uint64_t length)
            : storage(std::move(storage_in)), requested_offset(offset),
              remaining(length) {}
        scoped_refptr<NativeBlobStorage> storage;
        uint64_t requested_offset;
        uint64_t remaining;
        uint64_t logical_position = 0;
        wtf_size_t element_index = 0;
    };

    void ProcessNext()
    {
        for (;;) {
            if (frames_.empty())
                return Finish(net::OK);
            Frame& frame = frames_.back();
            if (!frame.remaining) {
                frames_.pop_back();
                continue;
            }
            if (frame.element_index == frame.storage->elements_.size())
                return Finish(net::ERR_FAILED);
            auto& element =
                frame.storage->elements_[frame.element_index];
            uint64_t element_length = element.length;
            if (element.kind == NativeBlobData::Element::Kind::kFile &&
                element_length == std::numeric_limits<uint64_t>::max()) {
                base::File::Info info;
                int status = NativeFileInfo(element, &info);
                if (status != net::OK)
                    return Finish(status);
                if (element.file_offset >
                    static_cast<uint64_t>(info.size))
                    return Finish(net::ERR_FAILED);
                element_length =
                    static_cast<uint64_t>(info.size) -
                    element.file_offset;
            }
            if (frame.logical_position >
                std::numeric_limits<uint64_t>::max() - element_length)
                return Finish(net::ERR_FAILED);
            const uint64_t element_start = frame.logical_position;
            frame.logical_position += element_length;
            ++frame.element_index;
            if (frame.requested_offset >= frame.logical_position)
                continue;
            const uint64_t offset =
                frame.requested_offset > element_start
                ? frame.requested_offset - element_start : 0;
            const uint64_t count =
                std::min(frame.remaining, element_length - offset);
            if (!count)
                continue;
            frame.requested_offset += count;
            frame.remaining -= count;
            if (element.kind == NativeBlobData::Element::Kind::kBytes) {
                if (!ReadBytes(element, offset, count))
                    return;
                continue;
            }
            if (element.kind == NativeBlobData::Element::Kind::kFile) {
                if (!ReadFile(element, offset, count))
                    return;
                continue;
            }
            if (!ReadBlob(element, offset, count))
                return;
        }
    }

    bool ReadBytes(NativeBlobStorage::StoredElement& element,
        uint64_t offset, uint64_t count)
    {
        if (element.bytes) {
            if (offset > element.bytes->size() ||
                count > element.bytes->size() - offset) {
                Finish(net::ERR_FAILED);
                return false;
            }
            output_.AppendSpan(base::span(
                reinterpret_cast<const uint8_t*>(element.bytes->data()),
                element.bytes->size()).subspan(
                    base::checked_cast<size_t>(offset),
                    base::checked_cast<size_t>(count)));
            return true;
        }
        if (element.provider_bytes) {
            if (offset > element.provider_bytes->size() ||
                count > element.provider_bytes->size() - offset) {
                Finish(net::ERR_FAILED);
                return false;
            }
            output_.AppendSpan(base::span(*element.provider_bytes).subspan(
                base::checked_cast<size_t>(offset),
                base::checked_cast<size_t>(count)));
            return true;
        }
        const wtf_size_t frame_index = frames_.size() - 1;
        const wtf_size_t element_index =
            frames_.back().element_index - 1;
        frames_.back().storage->ResolveProvider(element_index,
            base::BindOnce(&NativeBlobReadJob::ProviderResolved,
                weak_factory_.GetWeakPtr(), frame_index, element_index,
                offset, count));
        return false;
    }
    void ProviderResolved(wtf_size_t frame_index,
        wtf_size_t element_index, uint64_t offset, uint64_t count,
        bool success)
    {
        if (!success || frame_index >= frames_.size()) {
            Finish(net::ERR_FAILED);
            return;
        }
        auto& element =
            frames_[frame_index].storage->elements_[element_index];
        if (!element.provider_bytes ||
            offset > element.provider_bytes->size() ||
            count > element.provider_bytes->size() - offset) {
            Finish(net::ERR_FAILED);
            return;
        }
        output_.AppendSpan(base::span(*element.provider_bytes).subspan(
            base::checked_cast<size_t>(offset),
            base::checked_cast<size_t>(count)));
        ProcessNext();
    }
    bool ReadFile(NativeBlobStorage::StoredElement& element,
        uint64_t offset, uint64_t count)
    {
        base::File::Info info;
        int status = NativeFileInfo(element, &info);
        if (status != net::OK) {
            Finish(status);
            return false;
        }
        const uint64_t max_file_offset =
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (element.file_offset > max_file_offset ||
            offset > max_file_offset - element.file_offset ||
            (count && count - 1 >
                max_file_offset - element.file_offset - offset)) {
            Finish(net::ERR_FAILED);
            return false;
        }
        size_t old_size = output_.size();
        if (count > std::numeric_limits<size_t>::max() - old_size) {
            Finish(net::ERR_INSUFFICIENT_RESOURCES);
            return false;
        }
        output_.Grow(old_size + base::checked_cast<size_t>(count));
        auto destination = base::span(output_).subspan(old_size);
        size_t read = 0;
        while (read < destination.size()) {
            std::optional<size_t> result = element.file.Read(
                base::checked_cast<int64_t>(
                    element.file_offset + offset + read),
                destination.subspan(read));
            if (!result || !*result) {
                output_.Shrink(old_size);
                Finish(net::ERR_FAILED);
                return false;
            }
            read += *result;
        }
        return true;
    }
    bool ReadBlob(NativeBlobStorage::StoredElement& element,
        uint64_t offset, uint64_t count)
    {
        if (offset >
            std::numeric_limits<uint64_t>::max() -
                element.blob_offset) {
            Finish(net::ERR_FAILED);
            return false;
        }
        const uint64_t nested_offset = element.blob_offset + offset;
        nested_expected_count_ = count;
        if (element.blob) {
            int status;
            const uint64_t total = element.blob->CalculateSize(&status);
            if (status != net::OK) {
                Finish(status);
                return false;
            }
            const uint64_t bounded_offset =
                std::min(nested_offset, total);
            const uint64_t bounded_count =
                std::min(count, total - bounded_offset);
            if (bounded_count != count) {
                Finish(net::ERR_FAILED);
                return false;
            }
            frames_.push_back(Frame(
                element.blob, bounded_offset, bounded_count));
            return true;
        }
        if (element.blob_remote)
            element.remote.Bind(std::move(element.blob_remote));
        if (!element.remote.is_bound()) {
            Finish(net::ERR_FAILED);
            return false;
        }
        mojo::ScopedDataPipeProducerHandle producer;
        mojo::ScopedDataPipeConsumerHandle consumer;
        if (mojo::CreateDataPipe(nullptr, producer, consumer) !=
            MOJO_RESULT_OK) {
            Finish(net::ERR_INSUFFICIENT_RESOURCES);
            return false;
        }
        nested_client_complete_ = nested_data_complete_ = false;
        nested_status_ = net::ERR_FAILED;
        nested_client_receiver_.reset();
        auto client =
            nested_client_receiver_.BindNewPipeAndPassRemote();
        nested_client_receiver_.set_disconnect_handler(
            base::BindOnce(
                &NativeBlobReadJob::NestedClientDisconnected,
                weak_factory_.GetWeakPtr()));
        nested_drainer_ = std::make_unique<mojo::DataPipeDrainer>(
            this, std::move(consumer));
        element.remote->ReadRange(nested_offset, count,
            std::move(producer), std::move(client));
        return false;
    }
    void NestedClientDisconnected()
    {
        if (!nested_client_complete_)
            Finish(net::ERR_FAILED);
    }
    void MaybeCompleteNestedRead()
    {
        if (!nested_client_complete_ || !nested_data_complete_)
            return;
        nested_drainer_.reset();
        nested_client_receiver_.reset();
        if (nested_status_ != net::OK)
            return Finish(nested_status_);
        ProcessNext();
    }
    void DetachOwner()
    {
        if (owner_slot_ && *owner_slot_ == this)
            *owner_slot_ = nullptr;
        owner_slot_ = nullptr;
    }
    void Finish(int status)
    {
        NativeReadCallback callback = std::move(callback_);
        Vector<uint8_t> output = std::move(output_);
        uint64_t total = total_size_;
        DetachOwner();
        delete this;
        std::move(callback).Run(status, total, std::move(output));
    }
    Vector<Frame> frames_;
    uint64_t total_size_;
    Vector<uint8_t> output_;
    NativeReadCallback callback_;
    mojo::Receiver<mojom::blink::BlobReaderClient>
        nested_client_receiver_;
    std::unique_ptr<mojo::DataPipeDrainer> nested_drainer_;
    int nested_status_ = net::ERR_FAILED;
    uint64_t nested_expected_count_ = 0;
    bool nested_client_complete_ = false;
    bool nested_data_complete_ = false;
    NativeBlobReadJob** owner_slot_;
    base::WeakPtrFactory<NativeBlobReadJob> weak_factory_{this};
};

void CancelNativeBlobReadJob(NativeBlobReadJob* job)
{
    job->Cancel();
}

uint64_t NativeBlobStorage::CalculateSize(int* status) const
{
    *status = net::OK;
    uint64_t size = declared_size_;
    if (size == std::numeric_limits<uint64_t>::max() &&
        (elements_.size() != 1 ||
            elements_[0].kind != NativeBlobData::Element::Kind::kFile)) {
        *status = net::ERR_FAILED;
        return 0;
    }
    for (const auto& element : elements_) {
        if (element.kind != NativeBlobData::Element::Kind::kFile)
            continue;
        base::File::Info info;
        *status = NativeFileInfo(element, &info);
        if (*status != net::OK)
            return 0;
        if (element.file_offset > static_cast<uint64_t>(info.size) ||
            (element.length != std::numeric_limits<uint64_t>::max() &&
                element.length > static_cast<uint64_t>(info.size) -
                    element.file_offset)) {
            *status = net::ERR_UPLOAD_FILE_CHANGED;
            return 0;
        }
        if (size == std::numeric_limits<uint64_t>::max())
            size = static_cast<uint64_t>(info.size) - element.file_offset;
    }
    return size;
}

void NativeBlobStorage::ReadRange(uint64_t offset, uint64_t length,
    NativeReadCallback callback, NativeBlobReadJob** owner_slot)
{
    int status;
    uint64_t total = CalculateSize(&status);
    if (status != net::OK) {
        std::move(callback).Run(status, 0, {});
        return;
    }
    uint64_t bounded_length =
        length == std::numeric_limits<uint64_t>::max() ? total : length;
    auto* job = new NativeBlobReadJob(
        this, offset, bounded_length, total, std::move(callback),
        owner_slot);
    *owner_slot = job;
    job->Start();
}

void NativeBlobStorage::ResolveProvider(
    wtf_size_t index, base::OnceCallback<void(bool)> callback)
{
    auto& element = elements_[index];
    if (element.provider_bytes) {
        std::move(callback).Run(true);
        return;
    }
    element.provider_waiters.push_back(std::move(callback));
    if (element.provider_request_pending)
        return;
    if (element.bytes_provider)
        element.provider.Bind(std::move(element.bytes_provider));
    if (!element.provider.is_bound()) {
        auto waiters = std::move(element.provider_waiters);
        for (auto& waiter : waiters)
            std::move(waiter).Run(false);
        return;
    }
    element.provider_request_pending = true;
    // The provider Remote is owned by this storage; retaining the storage in
    // its callbacks would form a cycle if a request is cancelled or stalls.
    // Destroying the storage destroys the Remote and its callbacks first.
    element.provider.set_disconnect_handler(base::BindOnce(
        [](NativeBlobStorage* storage, wtf_size_t index) {
            auto& element = storage->elements_[index];
            if (element.provider_bytes)
                return;
            element.provider_request_pending = false;
            auto waiters = std::move(element.provider_waiters);
            for (auto& waiter : waiters)
                std::move(waiter).Run(false);
        }, base::Unretained(this), index));
    element.provider->RequestAsReply(base::BindOnce(
        [](NativeBlobStorage* storage, wtf_size_t index,
            const Vector<uint8_t>& bytes) {
            auto& element = storage->elements_[index];
            bool valid = bytes.size() == element.length;
            if (valid)
                element.provider_bytes = bytes;
            element.provider_request_pending = false;
            element.provider.reset();
            auto waiters = std::move(element.provider_waiters);
            for (auto& waiter : waiters)
                std::move(waiter).Run(valid);
        }, base::Unretained(this), index));
}

bool NativeBlobStorage::CaptureSnapshot(
    uint64_t* size, std::optional<base::Time>* modification_time)
{
    if (elements_.size() != 1 ||
        elements_[0].kind != NativeBlobData::Element::Kind::kFile) {
        int status;
        const uint64_t validated_size = CalculateSize(&status);
        if (status != net::OK)
            return false;
        *size = validated_size;
        *modification_time = std::nullopt;
        return true;
    }
    base::File::Info info;
    const auto& element = elements_[0];
    if (NativeFileInfo(element, &info) != net::OK ||
        element.file_offset > static_cast<uint64_t>(info.size) ||
        (element.length != std::numeric_limits<uint64_t>::max() &&
            element.length > static_cast<uint64_t>(info.size) -
                element.file_offset))
        return false;
    *size = element.length ==
            std::numeric_limits<uint64_t>::max()
        ? static_cast<uint64_t>(info.size) -
            element.file_offset
        : element.length;
    *modification_time = info.last_modified;
    return true;
}

class NativeDataPipeGetter final
    : public network::mojom::blink::DataPipeGetter {
public:
    explicit NativeDataPipeGetter(
        scoped_refptr<NativeBlobStorage> storage)
        : storage_(std::move(storage)) {}
    void Read(mojo::ScopedDataPipeProducerHandle pipe,
        ReadCallback callback) override
    {
        int status;
        uint64_t size = storage_->GetSize(&status);
        std::move(callback).Run(status, status == net::OK ? size : 0);
        if (status == net::OK)
            new NativeBlobWriter(storage_, 0, size, std::move(pipe),
                base::DoNothing());
    }
    void Clone(
        mojo::PendingReceiver<network::mojom::blink::DataPipeGetter>
            receiver) override
    {
        mojo::MakeSelfOwnedReceiver(
            std::make_unique<NativeDataPipeGetter>(storage_),
            std::move(receiver));
    }
private:
    scoped_refptr<NativeBlobStorage> storage_;
};

class NativeBlob final : public mojom::blink::Blob {
public:
    explicit NativeBlob(scoped_refptr<NativeBlobStorage> storage)
        : storage_(std::move(storage)) {}
    static void Bind(scoped_refptr<NativeBlobStorage> storage,
        mojo::PendingReceiver<mojom::blink::Blob> receiver)
    {
        if (!NativeBlobServiceTaskRunner()->RunsTasksInCurrentSequence()) {
            NativeBlobServiceTaskRunner()->PostTask(FROM_HERE, base::BindOnce(
                &NativeBlob::Bind, std::move(storage), std::move(receiver)));
            return;
        }
        auto blob = std::make_unique<NativeBlob>(std::move(storage));
        auto* ptr = blob.get();
        ptr->receiver_ = mojo::MakeSelfOwnedReceiver(
            std::move(blob), std::move(receiver));
    }
    void Clone(
        mojo::PendingReceiver<mojom::blink::Blob> receiver) override
    {
        Bind(storage_, std::move(receiver));
    }
    void AsDataPipeGetter(
        mojo::PendingReceiver<network::mojom::blink::DataPipeGetter>
            receiver) override
    {
        mojo::MakeSelfOwnedReceiver(
            std::make_unique<NativeDataPipeGetter>(storage_),
            std::move(receiver));
    }
    void ReadRange(uint64_t offset, uint64_t length,
        mojo::ScopedDataPipeProducerHandle pipe,
        mojo::PendingRemote<mojom::blink::BlobReaderClient> client)
        override
    {
        mojo::Remote<mojom::blink::BlobReaderClient> remote(std::move(client));
        int status;
        uint64_t total = storage_->GetSize(&status);
        if (status != net::OK) {
            pipe.reset();
            if (remote)
                remote->OnComplete(status, 0);
            return;
        }
        offset = std::min(offset, total);
        uint64_t count = std::min(length, total - offset);
        if (remote)
            remote->OnCalculatedSize(total, count);
        new NativeBlobWriter(storage_, offset, count, std::move(pipe),
            base::BindOnce(
                [](mojo::Remote<mojom::blink::BlobReaderClient> remote,
                    int status, uint64_t written) {
                    if (remote)
                        remote->OnComplete(status, written);
                }, std::move(remote)));
    }
    void ReadAll(mojo::ScopedDataPipeProducerHandle pipe,
        mojo::PendingRemote<mojom::blink::BlobReaderClient> client)
        override
    {
        ReadRange(0, std::numeric_limits<uint64_t>::max(),
            std::move(pipe), std::move(client));
    }
    void Load(
        mojo::PendingReceiver<network::mojom::blink::URLLoader>,
        const String&, const net::HttpRequestHeaders&,
        mojo::PendingRemote<network::mojom::blink::URLLoaderClient>)
        override;
    void ReadSideData(ReadSideDataCallback callback) override
    {
        std::move(callback).Run(std::nullopt);
    }
    void CaptureSnapshot(CaptureSnapshotCallback callback) override
    {
        uint64_t size;
        std::optional<base::Time> modification_time;
        if (!storage_->CaptureSnapshot(&size, &modification_time)) {
            // The snapshot IPC has no error payload. Closing this clone makes
            // the synchronous call fail without destroying other blob clones.
            receiver_->Close();
            return;
        }
        std::move(callback).Run(size, modification_time);
    }
    void GetInternalUUID(GetInternalUUIDCallback callback) override
    {
        std::move(callback).Run(storage_->uuid());
    }
private:
    scoped_refptr<NativeBlobStorage> storage_;
    mojo::SelfOwnedReceiverRef<mojom::blink::Blob> receiver_;
};

class NativeBlobURLLoader final
    : public network::mojom::blink::URLLoader {
public:
    NativeBlobURLLoader(scoped_refptr<NativeBlobStorage> storage,
        const String& method, const net::HttpRequestHeaders& headers,
        mojo::PendingRemote<network::mojom::blink::URLLoaderClient>
            client)
        : storage_(std::move(storage)), method_(method),
          headers_(headers), client_(ToCrossVariantMojoType(std::move(client)))
    {
        if (client_) {
            client_.set_disconnect_handler(base::BindOnce(
                &NativeBlobURLLoader::ClientDisconnected,
                weak_factory_.GetWeakPtr()));
        }
    }
    ~NativeBlobURLLoader() override
    {
        CancelWriter();
    }
    void Start()
    {
        if (method_ != "GET" && method_ != "HEAD") {
            Complete(net::ERR_METHOD_NOT_SUPPORTED);
            return;
        }
        int status;
        total_size_ = storage_->GetSize(&status);
        if (status != net::OK)
            return Complete(status);
        read_length_ = total_size_;
        std::optional<std::string> range_header =
            headers_.GetHeader(net::HttpRequestHeaders::kRange);
        if (range_header) {
            std::vector<net::HttpByteRange> ranges;
            if (!net::HttpUtil::ParseRangeHeader(*range_header, &ranges) ||
                ranges.size() != 1 ||
                !ranges[0].ComputeBounds(
                    base::checked_cast<int64_t>(total_size_))) {
                Complete(net::ERR_REQUEST_RANGE_NOT_SATISFIABLE);
                return;
            }
            partial_ = true;
            read_offset_ = ranges[0].first_byte_position();
            read_length_ =
                ranges[0].last_byte_position() - read_offset_ + 1;
        }
        SendResponse();
    }

    void FollowRedirect(const Vector<String>&,
        const net::HttpRequestHeaders&,
        const net::HttpRequestHeaders&,
        const std::optional<KURL>&) override
    {
        Complete(net::ERR_UNEXPECTED);
    }
    void SetPriority(net::RequestPriority priority, int32_t) override
    {
        priority_ = priority;
    }
    void PauseReadingBodyFromNet() override
    {
        paused_ = true;
        if (writer_)
            writer_->Pause();
    }
    void ResumeReadingBodyFromNet() override
    {
        paused_ = false;
        if (writer_)
            writer_->Resume();
    }

private:
    void CancelWriter()
    {
        if (!writer_)
            return;
        NativeBlobWriter* writer = std::exchange(writer_, nullptr);
        writer->Cancel();
    }
    void ClientDisconnected()
    {
        CancelWriter();
        client_.reset();
        storage_.reset();
    }
    void SendResponse()
    {

        auto head = network::mojom::URLResponseHead::New();
        auto response_headers =
            base::MakeRefCounted<net::HttpResponseHeaders>(
                partial_ ? "HTTP/1.1 206 Partial Content"
                         : "HTTP/1.1 200 OK");
        if (!storage_->type().empty()) {
            response_headers->AddHeader("Content-Type", storage_->type().Utf8());
        }
        response_headers->AddHeader("Content-Length",
            base::NumberToString(read_length_));
        response_headers->AddHeader("Accept-Ranges", "bytes");
        if (partial_) {
            response_headers->AddHeader("Content-Range", base::StrCat(
                {"bytes ",
                    base::NumberToString(read_offset_), "-",
                    base::NumberToString(
                        read_offset_ + read_length_ - 1),
                    "/", base::NumberToString(total_size_)}));
        }
        response_headers->GetMimeTypeAndCharset(&head->mime_type, &head->charset);
        head->headers = std::move(response_headers);
        head->content_length = read_length_;
        head->encoded_data_length = read_length_;
        head->has_range_requested = partial_;

        mojo::ScopedDataPipeProducerHandle producer;
        mojo::ScopedDataPipeConsumerHandle consumer;
        if (mojo::CreateDataPipe(nullptr, producer, consumer) !=
            MOJO_RESULT_OK) {
            Complete(net::ERR_INSUFFICIENT_RESOURCES);
            return;
        }
        client_->OnReceiveResponse(std::move(head), std::move(consumer),
            std::nullopt);
        writer_ = new NativeBlobWriter(
            storage_, read_offset_, method_ == "HEAD" ? 0 : read_length_,
            std::move(producer),
            base::BindOnce(&NativeBlobURLLoader::WriteFinished,
                weak_factory_.GetWeakPtr()));
        if (paused_)
            writer_->Pause();
    }

    void WriteFinished(int status, uint64_t written)
    {
        writer_ = nullptr;
        if (client_) {
            network::URLLoaderCompletionStatus completion(status);
            completion.decoded_body_length = written;
            completion.encoded_body_length = written;
            client_->OnComplete(completion);
            client_.reset();
        }
        storage_.reset();
    }
    void Complete(int status)
    {
        if (client_) {
            client_->OnComplete(
                network::URLLoaderCompletionStatus(status));
            client_.reset();
        }
        storage_.reset();
    }

    scoped_refptr<NativeBlobStorage> storage_;
    const String method_;
    const net::HttpRequestHeaders headers_;
    // Native value defaults serialize completely for the Blink wire variant.
    mojo::Remote<network::mojom::URLLoaderClient> client_;
    net::RequestPriority priority_ = net::IDLE;
    uint64_t total_size_ = 0;
    uint64_t read_offset_ = 0;
    uint64_t read_length_ = 0;
    bool partial_ = false;
    bool paused_ = false;
    NativeBlobWriter* writer_ = nullptr;
    base::WeakPtrFactory<NativeBlobURLLoader> weak_factory_{this};
};

void NativeBlob::Load(
    mojo::PendingReceiver<network::mojom::blink::URLLoader> receiver,
    const String& method, const net::HttpRequestHeaders& headers,
    mojo::PendingRemote<network::mojom::blink::URLLoaderClient> client)
{
    auto loader = std::make_unique<NativeBlobURLLoader>(
        storage_, method, headers, std::move(client));
    NativeBlobURLLoader* loader_ptr = loader.get();
    mojo::MakeSelfOwnedReceiver(std::move(loader), std::move(receiver));
    loader_ptr->Start();
}

void BindNativeBlob(scoped_refptr<NativeBlobStorage> storage,
    mojo::PendingReceiver<mojom::blink::Blob> receiver)
{
    NativeBlob::Bind(std::move(storage), std::move(receiver));
}

class NativeFileBackedBlobFactory final
    : public mojom::blink::FileBackedBlobFactory {
public:
    explicit NativeFileBackedBlobFactory(
        std::shared_ptr<mini_ui::LocalResources> resources)
        : resources_(std::move(resources)) {}

    void RegisterBlob(mojo::PendingReceiver<mojom::blink::Blob> receiver,
        const String& uuid, const String& content_type,
        mojom::blink::DataElementFilePtr file) override
    {
        auto data = std::make_unique<NativeBlobData>();
        NativeBlobData::Element element;
        element.kind = NativeBlobData::Element::Kind::kFile;
        element.file_offset = file->offset;
        element.length = file->length;
        element.expected_modification_time = file->expected_modification_time;
        if (resources_) {
            mini_ui::Error error;
            element.file = resources_->OpenFile(file->path, &error);
            if (element.file.IsValid()) {
                element.file_status = net::OK;
            } else if (error.code == mini_ui::ErrorCode::not_found) {
                element.file_status = net::ERR_FILE_NOT_FOUND;
            } else if (error.code != mini_ui::ErrorCode::resource_denied &&
                error.code != mini_ui::ErrorCode::invalid_argument) {
                element.file_status = net::ERR_FAILED;
            }
        }
        const uint64_t size = element.length;
        data->elements.push_back(std::move(element));
        BindNativeBlob(base::MakeRefCounted<NativeBlobStorage>(
            uuid, content_type, size, std::move(data)), std::move(receiver));
    }

    void RegisterBlobSync(mojo::PendingReceiver<mojom::blink::Blob> receiver,
        const String& uuid, const String& content_type,
        mojom::blink::DataElementFilePtr file,
        RegisterBlobSyncCallback callback) override
    {
        RegisterBlob(std::move(receiver), uuid, content_type, std::move(file));
        std::move(callback).Run();
    }

private:
    const std::shared_ptr<mini_ui::LocalResources> resources_;
};

class NativeBlobRegistry final : public mojom::blink::BlobRegistry {
public:
    void Register(mojo::PendingReceiver<mojom::blink::Blob> receiver,
        const String& uuid, const String& content_type, const String&,
        Vector<mojom::blink::DataElementPtr> elements,
        RegisterCallback callback) override
    {
        auto data = std::make_unique<NativeBlobData>();
        uint64_t size = 0;
        for (auto& source : elements) {
            NativeBlobData::Element target;
            if (source->is_bytes()) {
                auto& bytes = *source->get_bytes();
                target.kind = NativeBlobData::Element::Kind::kBytes;
                target.length = bytes.length;
                if (bytes.embedded_data) {
                    target.bytes = RawData::Create();
                    target.bytes->MutableData()->AppendSpan(
                        base::as_chars(base::span(*bytes.embedded_data)));
                } else {
                    target.bytes_provider = std::move(bytes.data);
                }
            } else if (source->is_file()) {
                auto& file = *source->get_file();
                target.kind = NativeBlobData::Element::Kind::kFile;
                // Registry callers carry no context capability. Preserve a
                // failing file element so reads report a real permission error.
                target.file_offset = file.offset;
                target.length = file.length;
                target.expected_modification_time =
                    file.expected_modification_time;
            } else {
                auto& blob = *source->get_blob();
                target.kind = NativeBlobData::Element::Kind::kBlob;
                target.blob_remote = std::move(blob.blob);
                target.blob_offset = blob.offset;
                target.length = blob.length;
            }
            if (size != std::numeric_limits<uint64_t>::max()) {
                if (target.length ==
                        std::numeric_limits<uint64_t>::max() ||
                    size > std::numeric_limits<uint64_t>::max() -
                        target.length) {
                    size = std::numeric_limits<uint64_t>::max();
                } else {
                    size += target.length;
                }
            }
            data->elements.push_back(std::move(target));
        }
        auto storage = base::MakeRefCounted<NativeBlobStorage>(
            uuid, content_type, size, std::move(data));
        BindNativeBlob(std::move(storage), std::move(receiver));
        std::move(callback).Run();
    }
    void RegisterFromStream(const String&, const String&, uint64_t,
        mojo::ScopedDataPipeConsumerHandle,
        mojo::PendingAssociatedRemote<mojom::blink::ProgressClient>,
        RegisterFromStreamCallback) override;
};

class NativeStreamRegistration final
    : public mojo::DataPipeDrainer::Client {
public:
    NativeStreamRegistration(const String& content_type,
        mojo::ScopedDataPipeConsumerHandle data,
        mojo::PendingAssociatedRemote<mojom::blink::ProgressClient>
            progress,
        mojom::blink::BlobRegistry::RegisterFromStreamCallback callback)
        : content_type_(content_type), progress_(std::move(progress)),
          callback_(std::move(callback)),
          drainer_(this, std::move(data)) {}
    void OnDataAvailable(base::span<const uint8_t> data) override
    {
        bytes_.AppendSpan(data);
        if (progress_)
            progress_->OnProgress(data.size());
    }
    void OnDataComplete() override
    {
        String uuid = WTF::CreateCanonicalUUIDString();
        auto data = std::make_unique<NativeBlobData>();
        if (!bytes_.empty()) {
            NativeBlobData::Element element;
            element.kind = NativeBlobData::Element::Kind::kBytes;
            element.length = bytes_.size();
            element.bytes = RawData::Create();
            element.bytes->MutableData()->AppendSpan(
                base::as_chars(base::span(bytes_)));
            data->elements.push_back(std::move(element));
        }
        uint64_t size = bytes_.size();
        auto storage = base::MakeRefCounted<NativeBlobStorage>(
            uuid, content_type_, size, std::move(data));
        mojo::PendingRemote<mojom::blink::Blob> remote;
        BindNativeBlob(storage,
            remote.InitWithNewPipeAndPassReceiver());
        scoped_refptr<BlobDataHandle> handle =
            BlobDataHandle::Create(uuid, content_type_, size,
                std::move(remote));
        auto callback = std::move(callback_);
        delete this;
        std::move(callback).Run(handle);
    }
private:
    const String content_type_;
    mojo::AssociatedRemote<mojom::blink::ProgressClient> progress_;
    mojom::blink::BlobRegistry::RegisterFromStreamCallback callback_;
    Vector<uint8_t> bytes_;
    mojo::DataPipeDrainer drainer_;
};

void NativeBlobRegistry::RegisterFromStream(
    const String& content_type, const String&, uint64_t,
    mojo::ScopedDataPipeConsumerHandle data,
    mojo::PendingAssociatedRemote<mojom::blink::ProgressClient> progress,
    RegisterFromStreamCallback callback)
{
    if (!data) {
        std::move(callback).Run(nullptr);
        return;
    }
    new NativeStreamRegistration(content_type, std::move(data),
        std::move(progress), std::move(callback));
}

void BindNativeBlobRegistry(
    mojo::PendingReceiver<mojom::blink::BlobRegistry> receiver)
{
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<NativeBlobRegistry>(), std::move(receiver),
        NativeBlobServiceTaskRunner());
}

#endif // defined(MINI_UI_QUICKJS)

constexpr int64_t BlobData::kToEndOfFile;

RawData::RawData() = default;

BlobData::BlobData(FileCompositionStatus composition)
    : file_composition_(composition)
#if defined(MINI_UI_QUICKJS)
    , native_data_(std::make_unique<NativeBlobData>())
#endif
{
}

BlobData::~BlobData() = default;

Vector<mojom::blink::DataElementPtr> BlobData::ReleaseElements()
{
    if (last_bytes_provider_) {
        DCHECK(last_bytes_provider_receiver_);
        BlobBytesProvider::Bind(std::move(last_bytes_provider_), std::move(last_bytes_provider_receiver_));
    }

    return std::move(elements_);
}
#if defined(MINI_UI_QUICKJS)
std::unique_ptr<NativeBlobData> BlobData::ReleaseNativeData()
{
    return std::move(native_data_);
}
#endif

void BlobData::SetContentType(const String& content_type)
{
    if (IsValidBlobType(content_type))
        content_type_ = content_type;
    else
        content_type_ = "";
}

void BlobData::AppendData(scoped_refptr<RawData> data)
{
    AppendDataInternal(base::span(data->data(), data->size()), data);
}

void BlobData::AppendBlob(scoped_refptr<BlobDataHandle> data_handle, int64_t offset, int64_t length)
{
    DCHECK_EQ(file_composition_, FileCompositionStatus::kNoUnknownSizeFiles) << "Blobs with a unknown-size file cannot have other items.";
    DCHECK(!data_handle->IsSingleUnknownSizeFile() || length != BlobData::kToEndOfFile)
        << "It is illegal to append an unknown size file blob without specifying "
           "a size.";
    // Skip zero-byte items, as they don't matter for the contents of the blob.
    if (length == 0)
        return;
#if defined(MINI_UI_QUICKJS)
    NativeBlobData::Element native_element;
    native_element.kind = NativeBlobData::Element::Kind::kBlob;
    native_element.length = length;
    native_element.blob_offset = offset;
    if (data_handle->native_storage_) {
        native_element.blob = data_handle->native_storage_;
    } else {
        native_element.blob_remote = data_handle->CloneBlobRemote();
    }
    native_data_->elements.push_back(std::move(native_element));
#else
    elements_.push_back(DataElement::NewBlob(DataElementBlob::New(data_handle->CloneBlobRemote(), offset, length)));
#endif
}

void BlobData::AppendText(const String& text, bool do_normalize_line_endings_to_native)
{
    DCHECK_EQ(file_composition_, FileCompositionStatus::kNoUnknownSizeFiles) << "Blobs with a unknown-size file cannot have other items.";
    std::string utf8_text = UTF8Encoding().Encode(text, WTF::kNoUnencodables);

    if (do_normalize_line_endings_to_native) {
        if (utf8_text.length() > BlobBytesProvider::kMaxConsolidatedItemSizeInBytes) {
            auto raw_data = RawData::Create();
            NormalizeLineEndingsToNative(utf8_text, *raw_data->MutableData());
            AppendDataInternal(base::span(raw_data->data(), raw_data->size()), raw_data);
        } else {
            Vector<char> buffer;
            NormalizeLineEndingsToNative(utf8_text, buffer);
            AppendDataInternal(base::make_span(buffer));
        }
    } else {
        AppendDataInternal(base::span(utf8_text.c_str(), utf8_text.size()));
    }
}

void BlobData::AppendBytes(base::span<const uint8_t> bytes)
{
    AppendDataInternal(base::as_chars(bytes));
}

uint64_t BlobData::length() const
{
    uint64_t length = 0;
#if defined(MINI_UI_QUICKJS)
    for (const auto& element : native_data_->elements)
        length += element.length;
#else

    for (const auto& element : elements_) {
        switch (element->which()) {
        case DataElement::Tag::kBytes:
            length += element->get_bytes()->length;
            break;
        case DataElement::Tag::kFile:
            length += element->get_file()->length;
            break;
        case DataElement::Tag::kBlob:
            length += element->get_blob()->length;
            break;
        }
    }
#endif
    return length;
}

void BlobData::AppendDataInternal(base::span<const char> data, scoped_refptr<RawData> raw_data)
{
    DCHECK_EQ(file_composition_, FileCompositionStatus::kNoUnknownSizeFiles) << "Blobs with a unknown-size file cannot have other items.";
    // Skip zero-byte items, as they don't matter for the contents of the blob.
    if (data.empty())
        return;
#if defined(MINI_UI_QUICKJS)
    if (!native_data_->elements.empty() &&
        native_data_->elements.back().kind ==
            NativeBlobData::Element::Kind::kBytes) {
        auto& previous = native_data_->elements.back();
        previous.bytes->MutableData()->AppendSpan(data);
        previous.length += data.size();
    } else {
        NativeBlobData::Element element;
        element.kind = NativeBlobData::Element::Kind::kBytes;
        element.length = data.size();
        if (raw_data) {
            element.bytes = std::move(raw_data);
        } else {
            element.bytes = RawData::Create();
            element.bytes->MutableData()->AppendSpan(data);
        }
        native_data_->elements.push_back(std::move(element));
    }
#else
    bool should_embed_bytes = current_memory_population_ + data.size() <= DataElementBytes::kMaximumEmbeddedDataSize;
    if (!elements_.empty() && elements_.back()->is_bytes()) {
        // Append bytes to previous element.
        DCHECK(last_bytes_provider_);
        DCHECK(last_bytes_provider_receiver_);
        const auto& bytes_element = elements_.back()->get_bytes();
        bytes_element->length += data.size();
        if (should_embed_bytes && bytes_element->embedded_data) {
            bytes_element->embedded_data->AppendSpan(data);
            current_memory_population_ += data.size();
        } else if (bytes_element->embedded_data) {
            current_memory_population_ -= bytes_element->embedded_data->size();
            bytes_element->embedded_data = std::nullopt;
        }
    } else {
        if (last_bytes_provider_) {
            // If `last_bytes_provider_` is set, but the previous element is not a
            // bytes element, a new BytesProvider will be created and we need to
            // make sure to bind the previous one first.
            DCHECK(last_bytes_provider_receiver_);
            BlobBytesProvider::Bind(std::move(last_bytes_provider_), std::move(last_bytes_provider_receiver_));
        }
        mojo::PendingRemote<BytesProvider> bytes_provider_remote;
        last_bytes_provider_ = std::make_unique<BlobBytesProvider>();
        last_bytes_provider_receiver_ = bytes_provider_remote.InitWithNewPipeAndPassReceiver();

        auto bytes_element = DataElementBytes::New(data.size(), std::nullopt, std::move(bytes_provider_remote));
        if (should_embed_bytes) {
            bytes_element->embedded_data = Vector<uint8_t>();
            bytes_element->embedded_data->AppendSpan(data);
            current_memory_population_ += data.size();
        }
        elements_.push_back(DataElement::NewBytes(std::move(bytes_element)));
    }
    if (raw_data)
        last_bytes_provider_->AppendData(std::move(raw_data));
    else
        last_bytes_provider_->AppendData(std::move(data));
#endif
}

// static
scoped_refptr<BlobDataHandle> BlobDataHandle::CreateForFile(mojom::blink::FileBackedBlobFactory* file_backed_blob_factory, const String& path, int64_t offset,
    int64_t length, const std::optional<base::Time>& expected_modification_time, const String& content_type)
{
    mojom::blink::DataElementFilePtr element = mojom::blink::DataElementFile::New(WebStringToFilePath(WebString(path)), offset, length, expected_modification_time);
    uint64_t size = length == BlobData::kToEndOfFile ? std::numeric_limits<uint64_t>::max() : length;
    return base::AdoptRef(new BlobDataHandle(file_backed_blob_factory, std::move(element), content_type, size));
}

// static
scoped_refptr<BlobDataHandle> BlobDataHandle::CreateForFileSync(mojom::blink::FileBackedBlobFactory* file_backed_blob_factory, const String& path,
    int64_t offset, int64_t length, const std::optional<base::Time>& expected_modification_time, const String& content_type)
{
    mojom::blink::DataElementFilePtr element = mojom::blink::DataElementFile::New(WebStringToFilePath(WebString(path)), offset, length, expected_modification_time);
    uint64_t size = length == BlobData::kToEndOfFile ? std::numeric_limits<uint64_t>::max() : length;
    return base::AdoptRef(new BlobDataHandle(file_backed_blob_factory, std::move(element), content_type, size, true));
}

// static
scoped_refptr<BlobDataHandle> BlobDataHandle::Create(const String& uuid, const String& type, uint64_t size, mojo::PendingRemote<mojom::blink::Blob> blob_remote)
{
    CHECK(blob_remote.is_valid());
    return base::AdoptRef(new BlobDataHandle(uuid, type, size, std::move(blob_remote)));
}

BlobDataHandle::BlobDataHandle()
    : uuid_(WTF::CreateCanonicalUUIDString())
    , size_(0)
    , is_single_unknown_size_file_(false)
{
#if defined(MINI_UI_QUICKJS)
    auto data = std::make_unique<NativeBlobData>();
    native_storage_ = base::MakeRefCounted<NativeBlobStorage>(
        uuid_, String(), size_, std::move(data));
    BindNativeBlob(native_storage_,
        blob_remote_.InitWithNewPipeAndPassReceiver());
#else
    GetThreadSpecificRegistry()->Register(
        blob_remote_.InitWithNewPipeAndPassReceiver(),
        uuid_, "", "", {});
#endif
}

BlobDataHandle::BlobDataHandle(std::unique_ptr<BlobData> data, uint64_t size)
    : uuid_(WTF::CreateCanonicalUUIDString())
    , type_(data->ContentType())
    , size_(size)
    , is_single_unknown_size_file_(data->IsSingleUnknownSizeFile())
{
#if defined(MINI_UI_QUICKJS)
    native_storage_ = base::MakeRefCounted<NativeBlobStorage>(
        uuid_, type_, size_, data->ReleaseNativeData());
    BindNativeBlob(native_storage_,
        blob_remote_.InitWithNewPipeAndPassReceiver());
#else
    auto elements = data->ReleaseElements();
    TRACE_EVENT0("Blob", "Registry::RegisterBlob");
    GetThreadSpecificRegistry()->Register(
        blob_remote_.InitWithNewPipeAndPassReceiver(), uuid_,
        type_.IsNull() ? "" : type_, "", std::move(elements));
#endif
}

BlobDataHandle::BlobDataHandle(mojom::blink::FileBackedBlobFactory* file_backed_blob_factory, mojom::blink::DataElementFilePtr file_element,
    const String& content_type, uint64_t size, bool synchronous_register)
    : uuid_(WTF::CreateCanonicalUUIDString())
    , type_(content_type)
    , size_(size)
    , is_single_unknown_size_file_(size == std::numeric_limits<uint64_t>::max())
{
    if (file_backed_blob_factory) {
        if (synchronous_register) {
            file_backed_blob_factory->RegisterBlobSync(
                blob_remote_.InitWithNewPipeAndPassReceiver(), uuid_, type_.IsNull() ? "" : type_, std::move(file_element));
        } else {
            file_backed_blob_factory->RegisterBlob(blob_remote_.InitWithNewPipeAndPassReceiver(), uuid_, type_.IsNull() ? "" : type_, std::move(file_element));
        }
    } else {
#if defined(MINI_UI_QUICKJS)
        // No context factory means no grant. The registry retains a denied
        // element and reports ERR_ACCESS_DENIED through the normal read path.
#else
        // TODO(b/287417238): Temporarily fallback to the previous BlobRegistry
        // registration when new interface is disabled by its feature flag or the
        // interface is not bound to a frame.
#endif
        Vector<mojom::blink::DataElementPtr> elements;
        elements.push_back(DataElement::NewFile(std::move(file_element)));
        TRACE_EVENT0("Blob", "Registry::RegisterBlob");
        GetThreadSpecificRegistry()->Register(blob_remote_.InitWithNewPipeAndPassReceiver(), uuid_, type_.IsNull() ? "" : type_, "", std::move(elements));
    }
}

BlobDataHandle::BlobDataHandle(const String& uuid, const String& type, uint64_t size)
    : uuid_(uuid)
    , type_(IsValidBlobType(type) ? type : "")
    , size_(size)
    , is_single_unknown_size_file_(false)
{
    // This is only used by unit tests that won't access `blob_remote_`.
    CHECK_IS_TEST();
}

BlobDataHandle::BlobDataHandle(const String& uuid, const String& type, uint64_t size, mojo::PendingRemote<mojom::blink::Blob> blob_remote)
    : uuid_(uuid)
    , type_(IsValidBlobType(type) ? type : "")
    , size_(size)
    , is_single_unknown_size_file_(false)
    , blob_remote_(std::move(blob_remote))
{
    DCHECK(blob_remote_.is_valid());
}

BlobDataHandle::~BlobDataHandle() = default;

mojo::PendingRemote<mojom::blink::Blob> BlobDataHandle::CloneBlobRemote()
{
#if defined(MINI_UI_QUICKJS)
    if (native_storage_) {
        mojo::PendingRemote<mojom::blink::Blob> result;
        BindNativeBlob(native_storage_,
            result.InitWithNewPipeAndPassReceiver());
        return result;
    }
#endif
    base::AutoLock locker(blob_remote_lock_);
    if (!blob_remote_.is_valid())
        return mojo::NullRemote();
    mojo::Remote<mojom::blink::Blob> blob(std::move(blob_remote_));
    mojo::PendingRemote<mojom::blink::Blob> blob_clone;
    blob->Clone(blob_clone.InitWithNewPipeAndPassReceiver());
    blob_remote_ = blob.Unbind();
    return blob_clone;
}

void BlobDataHandle::CloneBlobRemote(mojo::PendingReceiver<mojom::blink::Blob> receiver)
{
#if defined(MINI_UI_QUICKJS)
    if (native_storage_) {
        BindNativeBlob(native_storage_, std::move(receiver));
        return;
    }
#endif
    base::AutoLock locker(blob_remote_lock_);
    if (!blob_remote_.is_valid())
        return;
    mojo::Remote<mojom::blink::Blob> blob(std::move(blob_remote_));
    blob->Clone(std::move(receiver));
    blob_remote_ = blob.Unbind();
}

mojo::PendingRemote<network::mojom::blink::DataPipeGetter> BlobDataHandle::AsDataPipeGetter()
{
#if defined(MINI_UI_QUICKJS)
    if (native_storage_) {
        mojo::PendingRemote<
            network::mojom::blink::DataPipeGetter> result;
        mojo::MakeSelfOwnedReceiver(
            std::make_unique<NativeDataPipeGetter>(
                native_storage_),
            result.InitWithNewPipeAndPassReceiver(),
            NativeBlobServiceTaskRunner());
        return result;
    }
#endif
    base::AutoLock locker(blob_remote_lock_);
    if (!blob_remote_.is_valid())
        return mojo::NullRemote();
    mojo::PendingRemote<network::mojom::blink::DataPipeGetter> result;
    mojo::Remote<mojom::blink::Blob> blob(std::move(blob_remote_));
    blob->AsDataPipeGetter(result.InitWithNewPipeAndPassReceiver());
    blob_remote_ = blob.Unbind();
    return result;
}

void BlobDataHandle::ReadAll(mojo::ScopedDataPipeProducerHandle pipe, mojo::PendingRemote<mojom::blink::BlobReaderClient> client)
{
    base::AutoLock locker(blob_remote_lock_);
    mojo::Remote<mojom::blink::Blob> blob(std::move(blob_remote_));
    blob->ReadAll(std::move(pipe), std::move(client));
    blob_remote_ = blob.Unbind();
}

bool BlobDataHandle::CaptureSnapshot(uint64_t* snapshot_size, std::optional<base::Time>* snapshot_modification_time)
{
#if defined(MINI_UI_QUICKJS)
    if (native_storage_) {
        return native_storage_->CaptureSnapshot(
            snapshot_size, snapshot_modification_time);
    }
#endif
    // This method operates on a cloned blob remote; this lets us avoid holding
    // the |blob_remote_lock_| locked during the duration of the (synchronous)
    // CaptureSnapshot call.
    mojo::Remote<mojom::blink::Blob> remote(CloneBlobRemote());
    return remote->CaptureSnapshot(snapshot_size, snapshot_modification_time);
}

// static
mojom::blink::BlobRegistry* BlobDataHandle::GetBlobRegistry()
{
    return GetThreadSpecificRegistry();
}
#if defined(MINI_UI_QUICKJS)
// static
mojo::PendingRemote<mojom::blink::BlobRegistry>
BlobDataHandle::GetNativeBlobRegistryRemote()
{
    mojo::PendingRemote<mojom::blink::BlobRegistry> registry;
    BindNativeBlobRegistry(
        registry.InitWithNewPipeAndPassReceiver());
    return registry;
}

// static
mojo::PendingRemote<mojom::blink::FileBackedBlobFactory>
BlobDataHandle::GetNativeFileBackedBlobFactoryRemote(
    std::shared_ptr<mini_ui::LocalResources> resources)
{
    mojo::PendingRemote<mojom::blink::FileBackedBlobFactory> factory;
    auto receiver = factory.InitWithNewPipeAndPassReceiver();
    NativeBlobServiceTaskRunner()->PostTask(FROM_HERE, base::BindOnce(
        [](std::shared_ptr<mini_ui::LocalResources> resources,
            mojo::PendingReceiver<mojom::blink::FileBackedBlobFactory> receiver) {
            mojo::MakeSelfOwnedReceiver(
                std::make_unique<NativeFileBackedBlobFactory>(
                    std::move(resources)), std::move(receiver));
        }, std::move(resources), std::move(receiver)));
    return factory;
}
#endif


// static
void BlobDataHandle::SetBlobRegistryForTesting(mojom::blink::BlobRegistry* registry)
{
    g_blob_registry_for_testing = registry;
}

} // namespace blink
