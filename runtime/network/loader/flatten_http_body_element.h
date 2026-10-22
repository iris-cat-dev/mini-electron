#ifndef MINI_ELECTRON_NETWORK_LOADER_FLATTEN_HTTP_BODY_ELEMENT_H_
#define MINI_ELECTRON_NETWORK_LOADER_FLATTEN_HTTP_BODY_ELEMENT_H_

#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "services/network/public/mojom/data_pipe_getter.mojom.h"
#include "services/network/public/mojom/chunked_data_pipe_getter.mojom.h"
#include "third_party/libcurl/include/curl/curl.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace mini_electron {

struct FlattenHTTPBodyElement {
    enum Type { TypeData, TypeFile, TypeDataPipe, TypeChunkedDataPipe } type = TypeData;
    std::vector<char> data;
    base::File file;
    uint64_t fileStart = 0;
    uint64_t fileLength = 0;
    base::Time expectedModificationTime;
    mojo::PendingRemote<network::mojom::DataPipeGetter> pendingGetter;
    mojo::PendingRemote<network::mojom::ChunkedDataPipeGetter> pendingChunkedGetter;
    mojo::Remote<network::mojom::DataPipeGetter> getter;
    mojo::Remote<network::mojom::ChunkedDataPipeGetter> chunkedGetter;
    bool readOnlyOnce = false;
    bool started = false;
};

// Construct, read, rewind and destroy on the curl IO sequence. A pipe remains
// owned until all its bytes and its completion status have been consumed.
class FlattenHTTPBodyElementStream {
public:
    FlattenHTTPBodyElementStream(std::vector<std::unique_ptr<FlattenHTTPBodyElement>> elements,
        bool failed, base::RepeatingClosure resume)
        : m_elements(std::move(elements)), m_failed(failed), m_resume(std::move(resume)),
          m_watcher(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL)
    {
        m_failed = !reset();
    }

    bool isWaitingForData() const { return m_waitingForData; }

    bool reset()
    {
        for (const auto& element : m_elements) {
            if (element->readOnlyOnce && element->started)
                return false;
            if (element->type == FlattenHTTPBodyElement::TypeFile) {
                base::File::Info info;
                if (!element->file.GetInfo(&info) || info.size < 0
                    || element->fileStart > static_cast<uint64_t>(info.size)
                    || element->fileLength > static_cast<uint64_t>(info.size) - element->fileStart
                    || (!element->expectedModificationTime.is_null()
                        && (info.last_modified - element->expectedModificationTime).magnitude().InSeconds() != 0))
                    return false;
            }
        }
        finishPipe();
        m_index = 0;
        m_offset = 0;
        m_waitingForData = false;
        return !m_failed;
    }

    size_t read(void* destination, size_t blockSize, size_t blocks)
    {
        if (m_failed || (blocks && blockSize > std::numeric_limits<size_t>::max() / blocks))
            return CURL_READFUNC_ABORT;
        const size_t capacity = blockSize * blocks;
        if (!capacity)
            return 0;
        while (m_index < m_elements.size()) {
            auto& element = *m_elements[m_index];
            if (element.type == FlattenHTTPBodyElement::TypeData) {
                size_t count = std::min<uint64_t>(capacity, element.data.size() - m_offset);
                if (count) {
                    memcpy(destination, element.data.data() + m_offset, count);
                    m_offset += count;
                    return count;
                }
            } else if (element.type == FlattenHTTPBodyElement::TypeFile) {
                size_t count = std::min<uint64_t>(capacity, element.fileLength - m_offset);
                if (count) {
                    count = std::min<size_t>(count, std::numeric_limits<int>::max());
                    int actual = element.file.Read(element.fileStart + m_offset,
                        static_cast<char*>(destination), static_cast<int>(count));
                    if (actual <= 0) {
                        m_failed = true;
                        return CURL_READFUNC_ABORT;
                    }
                    m_offset += actual;
                    return actual;
                }
            } else {
                if (!m_consumer.is_valid() && !startPipe(element))
                    return CURL_READFUNC_ABORT;
                if (m_failed)
                    return CURL_READFUNC_ABORT;
                if (m_sizeKnown && m_offset > m_pipeSize) {
                    m_failed = true;
                    return CURL_READFUNC_ABORT;
                }
                if (!(m_sizeKnown && m_offset == m_pipeSize)) {
                    uint32_t count = static_cast<uint32_t>(std::min<size_t>(capacity, std::numeric_limits<uint32_t>::max()));
                    if (m_sizeKnown)
                        count = static_cast<uint32_t>(std::min<uint64_t>(count, m_pipeSize - m_offset));
                    MojoResult result = m_consumer->ReadData(destination, &count, MOJO_READ_DATA_FLAG_NONE);
                    if (result == MOJO_RESULT_OK) {
                        m_offset += count;
                        return count;
                    }
                    if (result == MOJO_RESULT_SHOULD_WAIT) {
                        m_waitingForData = true;
                        m_watcher.ArmOrNotify();
                        return CURL_READFUNC_PAUSE;
                    }
                    if (result != MOJO_RESULT_FAILED_PRECONDITION || m_sizeKnown) {
                        m_failed = true;
                        return CURL_READFUNC_ABORT;
                    }
                    // EOF can precede GetSize's reply for a chunked producer.
                    // The reply (or disconnect) resumes curl; do not spin on EOF.
                    m_waitingForData = true;
                    return CURL_READFUNC_PAUSE;
                }
                finishPipe();
            }
            ++m_index;
            m_offset = 0;
        }
        return 0;
    }

private:
    bool startPipe(FlattenHTTPBodyElement& element)
    {
        mojo::ScopedDataPipeProducerHandle producer;
        if (mojo::CreateDataPipe(nullptr, producer, m_consumer) != MOJO_RESULT_OK) {
            m_failed = true;
            return false;
        }
        auto weak = m_weakFactory.GetWeakPtr();
        auto disconnected = base::BindOnce([](base::WeakPtr<FlattenHTTPBodyElementStream> self) {
            if (!self)
                return;
            // A getter may disconnect after reporting a successful size. The
            // data pipe still carries the remaining bytes or a detectable EOF.
            if (!self->m_sizeKnown)
                self->m_failed = true;
            self->m_waitingForData = false;
            self->m_resume.Run();
        }, weak);
        auto complete = base::BindOnce([](base::WeakPtr<FlattenHTTPBodyElementStream> self, int32_t status, uint64_t size) {
            if (!self)
                return;
            self->m_failed = self->m_failed || status != 0 || self->m_offset > size;
            self->m_sizeKnown = true;
            self->m_pipeSize = size;
            self->m_waitingForData = false;
            self->m_resume.Run();
        }, weak);
        m_watcher.Watch(m_consumer.get(), MOJO_HANDLE_SIGNAL_READABLE,
            base::BindRepeating([](base::WeakPtr<FlattenHTTPBodyElementStream> self, MojoResult) {
                if (self) {
                    self->m_waitingForData = false;
                    self->m_resume.Run();
                }
            }, weak));
        element.started = true;
        if (element.type == FlattenHTTPBodyElement::TypeDataPipe) {
            if (!element.getter.is_bound() && element.pendingGetter.is_valid())
                element.getter.Bind(std::move(element.pendingGetter));
            if (!element.getter.is_bound()) {
                m_failed = true;
                return false;
            }
            // The lite getter proxy owns these arguments while dispatching on
            // the Blink receiver sequence; its reply returns to this IO runner.
            element.getter.set_disconnect_handler(std::move(disconnected));
            element.getter->Read(std::move(producer), std::move(complete));
        } else {
            if (!element.chunkedGetter.is_bound() && element.pendingChunkedGetter.is_valid())
                element.chunkedGetter.Bind(std::move(element.pendingChunkedGetter));
            if (!element.chunkedGetter.is_bound()) {
                m_failed = true;
                return false;
            }
            element.chunkedGetter.set_disconnect_handler(std::move(disconnected));
            element.chunkedGetter->GetSize(std::move(complete));
            element.chunkedGetter->StartReading(std::move(producer));
        }
        return true;
    }

    void finishPipe()
    {
        m_weakFactory.InvalidateWeakPtrs();
        m_watcher.Cancel();
        m_consumer.reset();
        m_sizeKnown = false;
        m_pipeSize = 0;
    }

    std::vector<std::unique_ptr<FlattenHTTPBodyElement>> m_elements;
    size_t m_index = 0;
    uint64_t m_offset = 0;
    bool m_failed;
    bool m_waitingForData = false;
    bool m_sizeKnown = false;
    uint64_t m_pipeSize = 0;
    base::RepeatingClosure m_resume;
    mojo::ScopedDataPipeConsumerHandle m_consumer;
    mojo::SimpleWatcher m_watcher;
    base::WeakPtrFactory<FlattenHTTPBodyElementStream> m_weakFactory { this };
};

} // namespace mini_electron

#endif // MINI_ELECTRON_NETWORK_LOADER_FLATTEN_HTTP_BODY_ELEMENT_H_
