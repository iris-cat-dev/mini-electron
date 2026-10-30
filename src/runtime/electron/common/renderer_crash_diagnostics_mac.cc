// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "src/runtime/electron/common/renderer_crash_diagnostics.h"
#if BUILDFLAG(IS_MAC)
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <libproc.h>
#include <mutex>
#include <new>
#include <poll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "src/runtime/electron/common/renderer_memory_mac.h"
#include "v8/include/v8.h"
#include "v8/include/v8-profiler.h"
namespace atom {
namespace {
constexpr uint64_t kReserve = 256ull << 20;
constexpr uint64_t kMaximum = 2ull << 30;
constexpr uint64_t kTimeout = 30000;
constexpr size_t kPayload = 1024;
constexpr uint32_t kMagic = 0x4d454853;
enum class PacketType : uint32_t { Ready, Data, Complete, Failure };
struct Header {
    uint32_t magic = kMagic;
    PacketType type = PacketType::Data;
    uint64_t sequence = 0;
    uint32_t size = 0;
    int32_t error = 0;
    uint64_t capture_used = 0;
    uint64_t capture_limit = 0;
};
enum class Status { Armed, Capturing, Succeeded, Skipped, Failed, Disabled, Unavailable };
enum class Reason { None, Headroom, Budget, Deadline, Size, Incomplete, File, Stopped, Configuration, Storage };
// Every field in this mapping is an untrusted renderer report or advisory hint.
// No terminal result, file capability, or host deadline lives here.
struct Record {
    std::atomic<uint64_t> used{0}, limit{0}, sample{0};
    std::atomic<int> cancel{0}, oom{0};
};
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
Record* g_record = nullptr;
int g_socket = -1;
v8::Isolate* g_owner = nullptr;
bool g_attempted = false;
std::mutex storage_lock;
const char* ReasonName(Reason reason) {
    switch (reason) {
    case Reason::Headroom: return "insufficient-headroom";
    case Reason::Budget: return "memory-budget-unavailable";
    case Reason::Deadline: return "deadline-exceeded";
    case Reason::Size: return "serialized-size-limit";
    case Reason::Incomplete: return "snapshot-incomplete";
    case Reason::File: return "file-io";
    case Reason::Stopped: return "renderer-stopped";
    case Reason::Configuration: return "invalid-configuration";
    case Reason::Storage: return "storage-budget";
    default: return "none";
    }
}
// Both endpoints reject ancillary capabilities, including on truncated packets.
ssize_t Receive(int socket, Header* header, char* payload) {
    iovec vectors[] = {{header, sizeof(*header)}, {payload, kPayload}};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 64)]{};
    msghdr message{};
    message.msg_iov = vectors;
    message.msg_iovlen = 2;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t size = ::recvmsg(socket, &message, MSG_DONTWAIT);
    if (size < 0) return size;
    bool ancillary = false;
    for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        ancillary = true;
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(0)) {
            size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) {
                int fd;
                std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(fd));
                ::close(fd);
            }
        }
    }
    if (ancillary || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        size < static_cast<ssize_t>(sizeof(Header)) || header->magic != kMagic ||
        header->size > kPayload || size != static_cast<ssize_t>(sizeof(Header) + header->size)) {
        errno = EPROTO;
        return -1;
    }
    return size;
}
class Writer final : public v8::ActivityControl, public v8::OutputStream {
public:
    explicit Writer(v8::Isolate* isolate) : isolate_(isolate), deadline_(Now() + kTimeout) {
        v8::HeapStatistics heap;
        isolate->GetHeapStatistics(&heap);
        capture_used_ = heap.used_heap_size();
        capture_limit_ = heap.heap_size_limit();
    }
    ControlOption ReportProgressValue(uint32_t, uint32_t) override {
        return Check() ? v8::ActivityControl::kContinue : v8::ActivityControl::kAbort;
    }
    int GetChunkSize() override { return 64 * 1024; }
    WriteResult WriteAsciiChunk(char* data, int size) override {
        if (size < 0) return v8::OutputStream::kAbort;
        for (int offset = 0; offset < size;) {
            uint32_t length = std::min<int>(kPayload, size - offset);
            if (!Send(PacketType::Data, data + offset, length)) return v8::OutputStream::kAbort;
            offset += length;
        }
        return v8::OutputStream::kContinue;
    }
    void EndOfStream() override { complete = Send(PacketType::Complete, nullptr, 0); }
    bool Check() {
        if (error) return false;
        if (g_record->cancel.load() || Now() >= deadline_) {
            error = g_record->cancel.load() ? ECANCELED : ETIMEDOUT;
            return false;
        }
        if (Now() < next_budget_check_) return true;
        next_budget_check_ = Now() + 100;
        v8::HeapStatistics heap;
        isolate_->GetHeapStatistics(&heap);
        MacRendererMemoryStatus memory;
        if (!ReadMacRendererMemoryStatus(&memory) || memory.pressure == MacMemoryPressureLevel::kUnknown) {
            error = ENODATA;
            return false;
        }
        if (!heap.heap_size_limit() || heap.used_heap_size() >= heap.heap_size_limit() - heap.heap_size_limit() * 15 / 100 ||
            heap.used_heap_size() > UINT64_MAX - kReserve || memory.available_bytes < heap.used_heap_size() + kReserve ||
            memory.pressure == MacMemoryPressureLevel::kCritical) {
            error = ENOMEM;
            return false;
        }
        return true;
    }
    bool Send(PacketType type, const char* data, uint32_t length) {
        Header header;
        header.type = type;
        header.sequence = sequence_;
        header.size = length;
        header.capture_used = capture_used_;
        header.capture_limit = capture_limit_;
        iovec vectors[] = {{&header, sizeof(header)}, {const_cast<char*>(data), length}};
        msghdr message{};
        message.msg_iov = vectors;
        message.msg_iovlen = length ? 2 : 1;
        const uint64_t wait_deadline = std::min(deadline_, Now() + 1000);
        while (Check()) {
            const ssize_t sent = ::sendmsg(g_socket, &message, MSG_DONTWAIT);
            if (sent == static_cast<ssize_t>(sizeof(header) + length)) {
                ++sequence_;
                return true;
            }
            const int send_error = sent < 0 ? errno : EIO;
            if (send_error != EAGAIN && send_error != EWOULDBLOCK && send_error != EINTR && send_error != ENOBUFS) {
                error = send_error;
                return false;
            }
            if (Now() >= wait_deadline) {
                error = ETIMEDOUT;
                return false;
            }
            pollfd p{g_socket, POLLOUT, 0};
            // Darwin reports peer datagram-queue pressure as ENOBUFS while
            // POLLOUT may remain ready; wait without spinning in that case.
            ::poll(send_error == ENOBUFS ? nullptr : &p, send_error == ENOBUFS ? 0 : 1, 10);
        }
        return false;
    }
    void Failure() {
        Header header;
        header.type = PacketType::Failure;
        header.sequence = sequence_;
        header.error = error ? error : EIO;
        ::send(g_socket, &header, sizeof(header), MSG_DONTWAIT);
    }
    int error = 0;
    bool complete = false;
private:
    v8::Isolate* isolate_;
    uint64_t deadline_;
    uint64_t sequence_ = 0;
    uint64_t next_budget_check_ = 0;
    uint64_t capture_used_ = 0;
    uint64_t capture_limit_ = 0;
};
} // namespace
class RendererCrashMonitor {
public:
    ~RendererCrashMonitor() {
        Stop();
        if (record) ::munmap(record, sizeof(Record));
        for (int fd : {mapping, sockets[0], sockets[1]}) if (fd >= 0) ::close(fd);
    }
    bool Init() {
        std::string name = "/me-d-" + base::NumberToString(base::RandUint64());
        mapping = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (mapping < 0) return false;
        ::shm_unlink(name.c_str());
        if (::ftruncate(mapping, sizeof(Record)) || ::socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets)) return false;
        for (int fd : {mapping, sockets[0], sockets[1]}) if (::fcntl(fd, F_SETFD, FD_CLOEXEC)) return false;
        int buffer = 64 * 1024;
        for (int fd : sockets) {
            if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) ||
                ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer))) return false;
        }
        void* memory = ::mmap(nullptr, sizeof(Record), PROT_READ | PROT_WRITE, MAP_SHARED, mapping, 0);
        if (memory == MAP_FAILED) return false;
        record = new (memory) Record;
        const char* setting = ::getenv("MINI_ELECTRON_PRE_OOM_SNAPSHOT_PERCENT");
        if (setting) {
            if (!base::StringToInt(setting, &percent) || percent < 0 || percent > 65) {
                Finish(Status::Unavailable, EINVAL, Reason::Configuration);
            } else if (!percent) {
                Finish(Status::Disabled, 0, Reason::None);
            }
        }
        return true;
    }
    base::Value::Dict Details() {
        base::Value::Dict d;
        const char* names[] = {"armed", "capturing", "succeeded", "skipped", "failed", "disabled", "unavailable"};
        d.Set("status", names[static_cast<int>(status)]);
        d.Set("reason", ReasonName(reason));
        d.Set("error", error);
        d.Set("thresholdPercent", percent);
        d.Set("triggerUsedHeapBytes", static_cast<double>(used));
        d.Set("triggerHeapLimitBytes", static_cast<double>(limit));
        d.Set("captureUsedHeapBytes", static_cast<double>(capture_used));
        d.Set("captureHeapLimitBytes", static_cast<double>(capture_limit));
        d.Set("serializedBytes", static_cast<double>(bytes));
        d.Set("mainSampleAgeMilliseconds", sampled ? base::Value(static_cast<double>(sample_age)) : base::Value());
        d.Set("footprintBytes", static_cast<double>(footprint));
        d.Set("systemAvailableBytes", static_cast<double>(available));
        d.Set("physicalBytes", static_cast<double>(physical));
        d.Set("memoryPressure", pressure);
        d.Set("v8OomRecorded", oom);
        d.Set("heapGraphSource", "renderer-reported");
        if (status == Status::Succeeded) d.Set("path", path);
        return d;
    }
    void Finish(Status next, int code, Reason why) {
        if (terminal) return;
        status = next;
        error = code;
        reason = why;
        oom = record && record->oom.load() != 0;
        terminal = true;
        if (record) record->cancel.store(1);
        if (sockets[0] >= 0) {
            ::close(sockets[0]);
            sockets[0] = -1;
        }
        if (file >= 0) {
            ::close(file);
            file = -1;
        }
        if (next != Status::Succeeded && owns_temporary) ::unlink(temporary.c_str());
    }
    void Persist() {
        if (persisted || !terminal) return;
        persisted = true;
        base::Value::Dict report;
        report.Set("rendererPid", pid);
        report.Set("preOomHeapSnapshot", Details());
        auto json = base::WriteJson(report);
        std::string temp = metadata + ".part";
        int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            metadata_error = errno;
            return;
        }
        size_t offset = 0;
        bool ok = json.has_value();
        while (ok && offset < json->size()) {
            ssize_t n = ::write(fd, json->data() + offset, json->size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                metadata_error = n < 0 ? errno : EIO;
                ok = false;
            } else {
                offset += n;
            }
        }
        if (ok && ::fsync(fd)) {
            metadata_error = errno;
            ok = false;
        }
        ::close(fd);
        if (ok && ::rename(temp.c_str(), metadata.c_str())) {
            metadata_error = errno;
            ok = false;
        }
        if (!ok) ::unlink(temp.c_str());
    }
    Reason Budget() {
        MacRendererMemoryStatus memory;
        rusage_info_v2 usage{};
        if (!ReadMacRendererMemoryStatus(&memory) || memory.pressure == MacMemoryPressureLevel::kUnknown ||
            ::proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) || !usage.ri_phys_footprint) return Reason::Budget;
        footprint = usage.ri_phys_footprint;
        available = memory.available_bytes;
        physical = memory.physical_bytes;
        pressure = static_cast<int>(memory.pressure);
        if (used > physical || footprint > physical || used > UINT64_MAX - kReserve ||
            available < used + kReserve || memory.pressure == MacMemoryPressureLevel::kCritical) return Reason::Headroom;
        return Reason::None;
    }
    void Prepare() {
        attempted = true;
        deadline = Now() + kTimeout;
        Reason budget = Budget();
        if (budget != Reason::None) {
            Finish(Status::Skipped, ENOMEM, budget);
            return;
        }
        if (used >= limit - limit * 15 / 100) {
            Finish(Status::Skipped, ENOMEM, Reason::Headroom);
            return;
        }
        std::lock_guard<std::mutex> guard(storage_lock);
        DIR* dir = ::opendir(directory.c_str());
        if (!dir) {
            Finish(Status::Failed, errno, Reason::File);
            return;
        }
        int count = 0;
        while (dirent* entry = ::readdir(dir)) {
            std::string name = entry->d_name;
            auto suffix = [&](const char* text) {
                size_t length = std::strlen(text);
                return name.size() >= length && name.compare(name.size() - length, length, text) == 0;
            };
            if (name.rfind("renderer-pre-oom-", 0) == 0 && (suffix(".heapsnapshot") || suffix(".heapsnapshot.part"))) ++count;
        }
        ::closedir(dir);
        struct statvfs storage{};
        if (::statvfs(directory.c_str(), &storage)) {
            Finish(Status::Failed, errno, Reason::File);
            return;
        }
        if (count >= 4 || !storage.f_frsize || storage.f_bavail < (kMaximum + storage.f_frsize - 1) / storage.f_frsize) {
            Finish(Status::Skipped, ENOSPC, Reason::Storage);
            return;
        }
        file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (file < 0) {
            Finish(Status::Failed, errno, Reason::File);
            return;
        }
        owns_temporary = true;
        Header ready;
        ready.type = PacketType::Ready;
        if (::send(sockets[0], &ready, sizeof(ready), MSG_DONTWAIT) != sizeof(ready)) {
            Finish(Status::Failed, errno, Reason::File);
            return;
        }
        capture_used = used;
        capture_limit = limit;
        status = Status::Capturing;
    }
    void Consume() {
        Header header;
        char payload[kPayload];
        ssize_t size = Receive(sockets[0], &header, payload);
        if (size < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) Finish(Status::Failed, errno, Reason::Incomplete);
            return;
        }
        if (header.sequence != sequence || (header.error && header.type != PacketType::Failure) ||
            (header.type != PacketType::Data && header.type != PacketType::Complete && header.type != PacketType::Failure)) {
            Finish(Status::Failed, EPROTO, Reason::Incomplete);
            return;
        }
        if (!sequence && header.type != PacketType::Failure) {
            if (!header.capture_limit || header.capture_limit > (1ull << 40) ||
                header.capture_used > header.capture_limit) {
                Finish(Status::Failed, EPROTO, Reason::Incomplete);
                return;
            }
            capture_used = header.capture_used;
            capture_limit = header.capture_limit;
        }
        ++sequence;
        if (header.type == PacketType::Data) {
            if (!header.size || header.size > kMaximum - bytes) {
                Finish(Status::Failed, EFBIG, Reason::Size);
                return;
            }
            size_t offset = 0;
            while (offset < header.size) {
                if (Now() >= deadline) {
                    Finish(Status::Failed, ETIMEDOUT, Reason::Deadline);
                    return;
                }
                ssize_t n = ::write(file, payload + offset, header.size - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    Finish(Status::Failed, n < 0 ? errno : EIO, Reason::File);
                    return;
                }
                offset += n;
                bytes += n;
            }
            return;
        }
        if (header.size || (header.type == PacketType::Complete && !bytes) ||
            (header.type == PacketType::Failure && header.error <= 0)) {
            Finish(Status::Failed, EPROTO, Reason::Incomplete);
            return;
        }
        if (header.type == PacketType::Failure) {
            Finish(Status::Failed, header.error, Reason::Incomplete);
            return;
        }
        if (::fsync(file)) {
            Finish(Status::Failed, errno, Reason::File);
            return;
        }
        if (Now() >= deadline) {
            Finish(Status::Failed, ETIMEDOUT, Reason::Deadline);
            return;
        }
        // The same mutex held by Stop linearizes cancellation with commit.
        if (::rename(temporary.c_str(), path.c_str())) {
            Finish(Status::Failed, errno, Reason::File);
            return;
        }
        Finish(Status::Succeeded, 0, Reason::None);
    }
    void Run() {
        for (;;) {
            int endpoint;
            int wait_ms = 100;
            {
                std::lock_guard<std::mutex> guard(finalize_lock);
                if (stopped) break;
                endpoint = sockets[0];
                wait_ms = attempted ? 5 : 100;
                if (!terminal && !attempted) {
                    uint64_t timestamp = record->sample.load();
                    uint64_t sample_used = record->used.load();
                    uint64_t sample_limit = record->limit.load();
                    uint64_t now = Now();
                    if (timestamp && timestamp <= now && now - timestamp <= 2000 && sample_limit &&
                        sample_limit <= (1ull << 40) && sample_used <= sample_limit &&
                        sample_used >= (sample_limit / 100) * percent) {
                        used = sample_used;
                        limit = sample_limit;
                        sampled = true;
                        sample_age = now - timestamp;
                        Prepare();
                    }
                }
                if (!terminal && attempted) {
                    if (Now() >= deadline) Finish(Status::Failed, ETIMEDOUT, Reason::Deadline);
                    else Consume();
                }
                if (terminal) {
                    Persist();
                    return;
                }
            }
            pollfd p{endpoint, POLLIN, 0};
            ::poll(&p, 1, wait_ms);
        }
    }
    void Stop() {
        std::lock_guard<std::mutex> stop_guard(stop_lock);
        {
            std::lock_guard<std::mutex> guard(finalize_lock);
            if (!stopped) {
                stopped = true;
                if (record) record->cancel.store(1);
                if (sockets[0] >= 0) {
                    ::close(sockets[0]);
                    sockets[0] = -1;
                }
                if (attempted && !terminal) Finish(Status::Failed, ECANCELED, Reason::Stopped);
                if (terminal && !metadata.empty()) Persist();
            }
        }
        if (worker.joinable()) worker.join();
    }
    Record* record = nullptr;
    int mapping = -1, sockets[2]{-1, -1}, file = -1, pid = 0, metadata_error = 0;
    int percent = 65, error = 0, pressure = 0;
    Status status = Status::Armed;
    Reason reason = Reason::None;
    bool attempted = false, terminal = false, persisted = false, stopped = false, sampled = false, oom = false;
    bool owns_temporary = false;
    uint64_t used = 0, limit = 0, capture_used = 0, capture_limit = 0, sample_age = 0;
    uint64_t footprint = 0, available = 0, physical = 0, bytes = 0, deadline = 0, sequence = 0;
    std::string directory, path, temporary, metadata;
    std::thread worker;
    std::mutex stop_lock, finalize_lock;
};
std::shared_ptr<RendererCrashMonitor> CreateRendererCrashMonitor(std::string* error) {
    auto monitor = std::make_shared<RendererCrashMonitor>();
    if (!monitor->Init()) {
        *error = std::string("failed to create private renderer diagnostics: ") + std::strerror(errno);
        return nullptr;
    }
    return monitor;
}
int MacRendererDiagnosticsMapping(const std::shared_ptr<RendererCrashMonitor>& m) { return m->mapping; }
int MacRendererDiagnosticsSocket(const std::shared_ptr<RendererCrashMonitor>& m) { return m->sockets[1]; }
bool StartRendererCrashMonitor(const std::shared_ptr<RendererCrashMonitor>& m, int pid, const std::string& directory, std::string*) {
    m->pid = pid;
    m->directory = directory;
    if (!directory.empty()) base::CreateDirectory(base::FilePath(directory));
    if (m->sockets[1] >= 0) {
        ::close(m->sockets[1]);
        m->sockets[1] = -1;
    }
    std::string stem = directory + "/renderer-pre-oom-" + base::NumberToString(pid) + "-" + base::NumberToString(base::RandUint64());
    m->path = stem + ".heapsnapshot";
    m->temporary = m->path + ".part";
    m->metadata = stem + ".json";
    m->worker = std::thread([m] { m->Run(); });
    return true;
}
void StopRendererCrashMonitor(const std::shared_ptr<RendererCrashMonitor>& m) { if (m) m->Stop(); }
void AppendRendererCrashDiagnostics(const std::shared_ptr<RendererCrashMonitor>& m, base::Value::Dict* d) {
    if (m) {
        m->Stop();
        d->Set("rendererPid", m->pid);
        // An exit-time marker is independent of the frozen snapshot outcome.
        d->Set("rendererReportedV8Oom", m->record && m->record->oom.load() != 0);
        d->Set("preOomHeapSnapshot", m->Details());
        if (m->metadata_error) d->Set("diagnosticsError", m->metadata_error);
    } else {
        base::Value::Dict details;
        details.Set("status", "unsupported-single-process");
        d->Set("preOomHeapSnapshot", std::move(details));
    }
}
bool InitializeMacRendererCrashDiagnostics(int mapping, int socket) {
    struct stat st{};
    const size_t page_size = static_cast<size_t>(::getpagesize());
    const size_t mapping_size = ((sizeof(Record) + page_size - 1) / page_size) * page_size;
    const int stat_result = ::fstat(mapping, &st);
    if (stat_result || st.st_size != static_cast<off_t>(mapping_size)) {
        const int error = stat_result ? errno : EINVAL;
        ::close(mapping);
        ::close(socket);
        errno = error;
        return false;
    }
    void* memory = ::mmap(nullptr, sizeof(Record), PROT_READ | PROT_WRITE, MAP_SHARED, mapping, 0);
    ::close(mapping);
    if (memory == MAP_FAILED) {
        ::close(socket);
        return false;
    }
    if (::fcntl(socket, F_SETFD, FD_CLOEXEC)) {
        ::munmap(memory, sizeof(Record));
        ::close(socket);
        return false;
    }
    g_record = static_cast<Record*>(memory);
    g_socket = socket;
    return true;
}
void RecordRendererV8Memory(v8::Isolate* isolate) noexcept {
    if (!g_record || !isolate || v8::Isolate::GetCurrent() != isolate) return;
    if (!g_owner) {
        g_owner = isolate;
        isolate->SetOOMErrorHandler([](const char* location, const v8::OOMDetails& details) {
            RecordRendererV8OOM(location, details.is_heap_oom, details.detail);
        });
    }
    if (g_owner != isolate) return;
    v8::HeapStatistics heap;
    isolate->GetHeapStatistics(&heap);
    g_record->used.store(heap.used_heap_size());
    g_record->limit.store(heap.heap_size_limit());
    g_record->sample.store(Now());
    if (g_attempted || g_record->cancel.load()) return;
    Header header;
    char payload[kPayload];
    if (Receive(g_socket, &header, payload) < 0) return;
    if (header.type != PacketType::Ready || header.size || header.sequence || header.error) return;
    g_attempted = true;
    Writer writer(isolate);
    const v8::HeapSnapshot* snapshot = nullptr;
    if (writer.Check()) {
        v8::HeapProfiler::HeapSnapshotOptions options;
        options.control = &writer;
        snapshot = isolate->GetHeapProfiler()->TakeHeapSnapshot(options);
    }
    if (snapshot) {
        snapshot->Serialize(&writer, v8::HeapSnapshot::kJSON);
        const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
    }
    if (!writer.complete) writer.Failure();
}
void RecordRendererV8OOM(const char*, bool, const char*) noexcept {
    if (g_record) g_record->oom.store(1);
}
} // namespace atom
#endif
