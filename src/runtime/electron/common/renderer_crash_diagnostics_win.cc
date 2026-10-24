// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "src/runtime/electron/common/renderer_crash_diagnostics.h"

#if BUILDFLAG(IS_WIN)
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_handle.h"
#include "v8/include/v8.h"

namespace atom {
namespace {
constexpr DWORD kOomException = 0xe0000008;
constexpr DWORD kCrashMagic = 0x4d454352;
constexpr DWORD kCrashVersion = 1;
constexpr DWORD kCrashWaitMilliseconds = 10000;
constexpr DWORD kCrashExitWaitMilliseconds = 2000;

struct alignas(8) V8MemorySample {
    volatile LONG64 used = 0;
    volatile LONG64 total = 0;
    volatile LONG64 limit = 0;
    volatile LONG64 external = 0;
    volatile LONG64 tick = 0;
};

void SampleV8(v8::Isolate* isolate, V8MemorySample* sample) noexcept
{
    if (!isolate)
        return;
    v8::HeapStatistics statistics;
    isolate->GetHeapStatistics(&statistics);
    ::InterlockedExchange64(&sample->used, statistics.used_heap_size());
    ::InterlockedExchange64(&sample->total, statistics.total_heap_size());
    ::InterlockedExchange64(&sample->limit, statistics.heap_size_limit());
    ::InterlockedExchange64(&sample->external, statistics.external_memory());
    ::InterlockedExchange64(&sample->tick, ::GetTickCount64());
}

// Constructed before launch. The failing renderer only copies into this mapping
// and signals an event; all file I/O and DbgHelp work happen in its healthy host.
struct alignas(64) CrashRecord {
    DWORD magic = kCrashMagic;
    DWORD version = kCrashVersion;
    volatile LONG phase = 0;
    volatile LONG oom_recorded = 0;
    volatile LONG oom_kind = 0;
    DWORD thread_id = 0;
    DWORD oom_thread_id = 0;
    V8MemorySample main_sample;
    char location[128] {};
    char detail[256] {};
    EXCEPTION_RECORD exception {};
    CONTEXT context {};
};

CrashRecord* g_crash_record = nullptr;
HANDLE g_crash_requested = nullptr;
HANDLE g_crash_completed = nullptr;

LONG CaptureException(EXCEPTION_POINTERS* pointers)
{
    CrashRecord* record = g_crash_record;
    if (!record || !pointers || !pointers->ExceptionRecord || !pointers->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (::InterlockedCompareExchange(&record->phase, 1, 0) != 0) {
        ::WaitForSingleObject(g_crash_completed, kCrashWaitMilliseconds);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    record->thread_id = ::GetCurrentThreadId();
    record->exception = *pointers->ExceptionRecord;
    record->exception.ExceptionRecord = nullptr;
    record->context = *pointers->ContextRecord;
    record->context.ContextFlags &= CONTEXT_ALL;
    ::InterlockedExchange(&record->phase, 2);
    ::SetEvent(g_crash_requested);
    ::WaitForSingleObject(g_crash_completed, kCrashWaitMilliseconds);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK ObserveOom(EXCEPTION_POINTERS* pointers)
{
    // V8/PartitionAlloc use this noncontinuable exception. Other first-chance
    // exceptions (including V8 guard-page faults) must not generate dumps.
    if (pointers && pointers->ExceptionRecord
        && pointers->ExceptionRecord->ExceptionCode == kOomException
        && (pointers->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE))
        return CaptureException(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI ObserveUnhandled(EXCEPTION_POINTERS* pointers)
{
    return CaptureException(pointers);
}

template <size_t N>
void CopyCrashText(char (&destination)[N], const char* source) noexcept
{
    size_t index = 0;
    if (source) {
        for (; index + 1 < N && source[index]; ++index)
            destination[index] = source[index];
    }
    destination[index] = '\0';
}

template <size_t N>
std::string CrashText(const char (&text)[N])
{
    return std::string(text, strnlen(text, N));
}

uint64_t ReadCounter(volatile LONG64* counter)
{
    return static_cast<uint64_t>(::InterlockedCompareExchange64(counter, 0, 0));
}

void SetBytes(base::Value::Dict& dict, const char* key, uint64_t value)
{
    dict.Set(key, static_cast<double>(value));
}

DWORD LastWindowsError()
{
    const DWORD error = ::GetLastError();
    return error ? error : ERROR_GEN_FAILURE;
}

base::Value::Dict ImageIdentity()
{
    base::Value::Dict identity;
    // The child is created from this exact executable. RSDS GUID + age is the
    // symbol-server identity; a filename or a runtime version alone is not.
    auto* image = reinterpret_cast<const unsigned char*>(::GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (!image || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        return identity;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return identity;
    identity.Set("timestamp", static_cast<double>(nt->FileHeader.TimeDateStamp));
    identity.Set("imageSize", static_cast<double>(nt->OptionalHeader.SizeOfImage));
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const size_t image_size = nt->OptionalHeader.SizeOfImage;
    if (directory.VirtualAddress > image_size
        || directory.Size > image_size - directory.VirtualAddress)
        return identity;
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
        image + directory.VirtualAddress);
    for (size_t index = 0; index < directory.Size / sizeof(*entries); ++index) {
        const auto& entry = entries[index];
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < 25
            || entry.AddressOfRawData > image_size
            || entry.SizeOfData > image_size - entry.AddressOfRawData)
            continue;
        const auto* codeview = image + entry.AddressOfRawData;
        if (std::memcmp(codeview, "RSDS", 4))
            continue;
        GUID guid;
        DWORD age;
        std::memcpy(&guid, codeview + 4, sizeof(guid));
        std::memcpy(&age, codeview + 20, sizeof(age));
        char text[33];
        std::snprintf(text, sizeof(text), "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
            guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
            guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        identity.Set("pdbGuid", text);
        identity.Set("pdbAge", static_cast<double>(age));
        std::string path(reinterpret_cast<const char*>(codeview + 24),
            strnlen(reinterpret_cast<const char*>(codeview + 24), entry.SizeOfData - 24));
        const size_t separator = path.find_last_of("/\\");
        identity.Set("pdbName", path.substr(separator == std::string::npos ? 0 : separator + 1));
        break;
    }
    return identity;
}

std::mutex& DbgHelpMutex()
{
    static std::mutex mutex;
    return mutex;
}

BOOL CALLBACK DumpCallback(PVOID parameter, MINIDUMP_CALLBACK_INPUT* input,
    MINIDUMP_CALLBACK_OUTPUT* output)
{
    if (input->CallbackType == CancelCallback) {
        output->CheckCancel = TRUE;
        output->Cancel = ::GetTickCount64() >= *static_cast<const ULONGLONG*>(parameter);
    }
    return TRUE;
}
} // namespace

class RendererCrashMonitor : public std::enable_shared_from_this<RendererCrashMonitor> {
public:
    ~RendererCrashMonitor()
    {
        Stop();
        if (record_)
            ::UnmapViewOfFile(record_);
    }

    bool Initialize(std::string* error)
    {
        SECURITY_ATTRIBUTES inherited { sizeof(inherited), nullptr, TRUE };
        mapping_.Set(::CreateFileMappingW(INVALID_HANDLE_VALUE, &inherited,
            PAGE_READWRITE, 0, sizeof(CrashRecord), nullptr));
        requested_.Set(::CreateEventW(&inherited, TRUE, FALSE, nullptr));
        completed_.Set(::CreateEventW(&inherited, TRUE, FALSE, nullptr));
        stop_.Set(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!mapping_.is_valid() || !requested_.is_valid() || !completed_.is_valid()
            || !stop_.is_valid()) {
            *error = "failed to allocate renderer crash channel";
            return false;
        }
        record_ = static_cast<CrashRecord*>(::MapViewOfFile(mapping_.get(),
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(CrashRecord)));
        if (!record_) {
            *error = "failed to map renderer crash channel";
            return false;
        }
        new (record_) CrashRecord();
        return true;
    }

    RendererCrashHandles handles() const
    {
        return { mapping_.get(), requested_.get(), completed_.get() };
    }

    bool Start(HANDLE process, HANDLE job, const std::string& directory, std::string* error)
    {
        HANDLE owned_process = nullptr;
        HANDLE owned_job = nullptr;
        if (!::DuplicateHandle(::GetCurrentProcess(), process, ::GetCurrentProcess(),
                &owned_process, 0, FALSE, DUPLICATE_SAME_ACCESS)
            || !::DuplicateHandle(::GetCurrentProcess(), job, ::GetCurrentProcess(),
                &owned_job, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            if (owned_process)
                ::CloseHandle(owned_process);
            *error = "failed to retain renderer crash handles";
            return false;
        }
        process_.Set(owned_process);
        job_.Set(owned_job);
        pid_ = ::GetProcessId(process_.get());
        directory_ = base::FilePath::FromUTF8Unsafe(directory);
        image_ = ImageIdentity();
        port_.Set(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT association {};
        association.CompletionPort = port_.get();
        association.CompletionKey = this;
        if (!port_.is_valid() || !::SetInformationJobObject(job_.get(),
                JobObjectAssociateCompletionPortInformation, &association, sizeof(association)))
            notification_error_ = ::GetLastError();
        SampleMemory();
        auto* owner = new (std::nothrow)
            std::shared_ptr<RendererCrashMonitor>(shared_from_this());
        if (!owner) {
            *error = "failed to retain renderer crash monitor";
            return false;
        }
        thread_.Set(::CreateThread(nullptr, 0, [](void* argument) -> DWORD {
            std::unique_ptr<std::shared_ptr<RendererCrashMonitor>> owner(
                static_cast<std::shared_ptr<RendererCrashMonitor>*>(argument));
            (*owner)->Run();
            return 0;
        }, owner, 0, nullptr));
        if (!thread_.is_valid()) {
            delete owner;
            *error = "failed to start renderer crash monitor";
            return false;
        }
        return true;
    }

    void Stop()
    {
        // The worker owns its lifetime. In particular, dump filesystem I/O must
        // never make closing a renderer join indefinitely on the browser loop.
        ::SetEvent(stop_.get());
    }

    bool WaitForExit()
    {
        if (::InterlockedCompareExchange(&record_->phase, 0, 0) != 2
            || record_->magic != kCrashMagic || record_->version != kCrashVersion)
            return false;

        // IPC can close while the fatal thread is still waiting for its dump,
        // or immediately after capture unblocks it but before Windows signals
        // the process handle. Neither interval is an ordinary transport error.
        const HANDLE handles[] = { completed_.get(), process_.get() };
        const DWORD result = ::WaitForMultipleObjects(2, handles, FALSE,
            kCrashWaitMilliseconds);
        if (result == WAIT_OBJECT_0 + 1)
            return true;
        if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT)
            return false;
        return ::WaitForSingleObject(process_.get(), kCrashExitWaitMilliseconds)
            == WAIT_OBJECT_0;
    }

    std::optional<bool> MemoryLimitExceeded() const
    {
        // Hard-limit completion-port notifications are best effort. Absence is
        // unknown, not proof that the Job did not refuse a commit.
        return memory_limit_hit_.load(std::memory_order_acquire)
            ? std::optional<bool>(true) : std::nullopt;
    }

    void Append(base::Value::Dict* details)
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (result_.empty()) {
            auto report = Diagnostics();
            if (::InterlockedCompareExchange(&record_->phase, 0, 0) == 2)
                report.Set("crashDumpError", static_cast<double>(ERROR_IO_PENDING));
            details->Merge(std::move(report));
        } else {
            details->Merge(result_.Clone());
        }
    }

private:
    void DrainJobNotifications()
    {
        if (!port_.is_valid() || notification_error_)
            return;
        DWORD message = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* process = nullptr;
        while (::GetQueuedCompletionStatus(port_.get(), &message, &key, &process, 0)) {
            if (key == reinterpret_cast<ULONG_PTR>(this)
                && message == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT
                && reinterpret_cast<ULONG_PTR>(process) == pid_) {
                memory_limit_tick_.store(::GetTickCount64(), std::memory_order_release);
                memory_limit_hit_.store(true, std::memory_order_release);
            }
        }
    }

    void SampleMemory()
    {
        PROCESS_MEMORY_COUNTERS_EX counters {};
        counters.cb = sizeof(counters);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        PERFORMANCE_INFORMATION system {};
        system.cb = sizeof(system);
        const bool have_process = ::GetProcessMemoryInfo(process_.get(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
        const bool have_job = ::QueryInformationJobObject(job_.get(),
            JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr);
        const bool have_system = ::GetPerformanceInfo(&system, sizeof(system));
        std::lock_guard<std::mutex> guard(lock_);
        if (have_process) {
            private_bytes_ = counters.PrivateUsage;
            peak_commit_ = std::max<uint64_t>(peak_commit_, counters.PeakPagefileUsage);
        }
        if (have_job) {
            process_limit_ = limits.ProcessMemoryLimit;
            peak_job_commit_ = std::max<uint64_t>(peak_job_commit_, limits.PeakProcessMemoryUsed);
        }
        if (have_system) {
            system_commit_limit_ = system.CommitLimit * system.PageSize;
            system_commit_available_ = (system.CommitLimit - system.CommitTotal) * system.PageSize;
            physical_available_ = system.PhysicalAvailable * system.PageSize;
        }
    }

    base::Value::Dict Diagnostics()
    {
        base::Value::Dict result;
        result.Set("crashCaptureStatus", captured_ ? "captured" : "not-captured");
        result.Set("sandboxMemoryLimitExceeded", memory_limit_hit_.load()
            ? base::Value(true) : base::Value());
        base::Value::Dict memory;
        SetBytes(memory, "processMemoryLimit", process_limit_);
        SetBytes(memory, "privateBytes", private_bytes_);
        SetBytes(memory, "peakCommitBytes", peak_commit_);
        SetBytes(memory, "jobPeakProcessCommitBytes", peak_job_commit_);
        SetBytes(memory, "systemCommitLimitBytes", system_commit_limit_);
        SetBytes(memory, "systemCommitAvailableBytes", system_commit_available_);
        SetBytes(memory, "physicalAvailableBytes", physical_available_);
        result.Set("memory", std::move(memory));
        result.Set("image", image_.Clone());
        result.Set("rendererPid", static_cast<double>(pid_));
        if (notification_error_)
            result.Set("jobNotificationError", static_cast<double>(notification_error_));
        const LONG oom_kind = ::InterlockedCompareExchange(&record_->oom_kind, 0, 0);
        const std::string location = oom_kind ? CrashText(record_->location) : std::string();
        const bool explicit_heap_limit = location == "Reached heap limit"
            || location == "Ineffective mark-compacts near heap limit";
        const bool matching_v8_failure = oom_kind
            && record_->oom_thread_id == record_->thread_id;
        // The V8 failure path and Job evidence are independent. A Job rejection
        // earlier in this renderer's life must not override a later heap-limit OOM.
        // A concurrent worker/native failure must not inherit another thread's marker.
        result.Set("oomSource", !matching_v8_failure ? "unknown"
            : explicit_heap_limit ? "v8-heap-limit" : "v8-allocation-failure");
        result.Set("memoryLimitEvidence", memory_limit_hit_.load() ? "job-notification" : "unknown");
        const uint64_t limit_tick = memory_limit_tick_.load(std::memory_order_acquire);
        if (limit_tick)
            SetBytes(result, "jobMemoryLimitEventAgeMilliseconds", ::GetTickCount64() - limit_tick);
        auto& sample = record_->main_sample;
        const uint64_t tick = ReadCounter(&sample.tick);
        if (oom_kind || tick) {
            base::Value::Dict v8;
            if (tick) {
                SetBytes(v8, "usedHeapBytes", ReadCounter(&sample.used));
                SetBytes(v8, "totalHeapBytes", ReadCounter(&sample.total));
                SetBytes(v8, "heapLimitBytes", ReadCounter(&sample.limit));
                SetBytes(v8, "externalBytes", ReadCounter(&sample.external));
                SetBytes(v8, "sampleAgeMilliseconds", ::GetTickCount64() >= tick
                    ? ::GetTickCount64() - tick : 0);
            }
            v8.Set("sampleKind", tick ? "last-main-isolate" : "unavailable");
            if (oom_kind) {
                v8.Set("location", location);
                v8.Set("detail", CrashText(record_->detail));
                v8.Set("isHeapOOM", oom_kind == 1);
                v8.Set("failureThreadId", static_cast<double>(record_->oom_thread_id));
            }
            result.Set("v8", std::move(v8));
        }
        return result;
    }

    void Capture()
    {
        DrainJobNotifications();
        SampleMemory();
        captured_ = ::InterlockedCompareExchange(&record_->phase, 0, 0) == 2
            && record_->magic == kCrashMagic && record_->version == kCrashVersion;
        base::Value::Dict report;
        {
            std::lock_guard<std::mutex> guard(lock_);
            report = Diagnostics();
        }
        DWORD dump_error = ERROR_INVALID_DATA;
        // Bound stalled capture I/O to one worker for the host. Other crashes
        // retain their diagnostics and fail dumping explicitly rather than
        // queueing more blocked writers or delaying renderer recovery.
        std::unique_lock<std::mutex> capture_lock(DbgHelpMutex(), std::try_to_lock);
        base::FilePath dump_path;
        base::FilePath metadata_path;
        if (captured_) {
            report.Set("exceptionCode", static_cast<double>(record_->exception.ExceptionCode));
            report.Set("exceptionThreadId", static_cast<double>(record_->thread_id));
            const ULONG_PTR allocation_bytes = record_->exception.ExceptionCode == kOomException
                && record_->exception.NumberParameters ? record_->exception.ExceptionInformation[0] : 0;
            report.Set("failedAllocationBytes", allocation_bytes
                ? base::Value(static_cast<double>(allocation_bytes)) : base::Value());
            dump_error = capture_lock.owns_lock()
                ? WriteDump(report, &dump_path, &metadata_path) : ERROR_BUSY;
        }
        if (!dump_path.empty())
            report.Set("crashDumpPath", dump_path.AsUTF8Unsafe());
        if (dump_error)
            report.Set("crashDumpError", static_cast<double>(dump_error));
        if (!metadata_path.empty()) {
            report.Set("crashDiagnosticsPath", metadata_path.AsUTF8Unsafe());
            const auto json = base::WriteJsonWithOptions(report, base::OPTIONS_PRETTY_PRINT);
            if (!json || !base::WriteFile(metadata_path, *json)) {
                const DWORD error = json ? ::GetLastError() : ERROR_INVALID_DATA;
                report.Set("crashDiagnosticsError", static_cast<double>(error ? error : ERROR_WRITE_FAULT));
                report.Remove("crashDiagnosticsPath");
            }
        }
        {
            std::lock_guard<std::mutex> guard(lock_);
            result_ = std::move(report);
        }
        ::SetEvent(completed_.get());
    }

    DWORD WriteDump(const base::Value::Dict& report, base::FilePath* dump_path,
        base::FilePath* metadata_path)
    {
        if (directory_.empty())
            return ERROR_PATH_NOT_FOUND;
        if (!base::CreateDirectory(directory_))
            return LastWindowsError();
        FILETIME time;
        ::GetSystemTimeAsFileTime(&time);
        const uint64_t timestamp = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        const std::string name = "renderer-" + base::NumberToString(timestamp)
            + "-" + base::NumberToString(pid_);
        const auto destination = directory_.AppendASCII(name + ".dmp");
        const auto temporary = directory_.AppendASCII(name + ".dmp.part");
        *metadata_path = directory_.AppendASCII(name + ".json");
        base::win::ScopedHandle file(::CreateFileW(temporary.value().c_str(), GENERIC_WRITE,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.is_valid())
            return LastWindowsError();
        EXCEPTION_RECORD exception = record_->exception;
        exception.ExceptionRecord = nullptr;
        exception.NumberParameters = std::min<DWORD>(exception.NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS);
        CONTEXT context = record_->context;
        EXCEPTION_POINTERS pointers { &exception, &context };
        MINIDUMP_EXCEPTION_INFORMATION exception_info { record_->thread_id, &pointers, FALSE };
        base::win::ScopedHandle thread(::OpenThread(THREAD_QUERY_LIMITED_INFORMATION,
            FALSE, record_->thread_id));
        DWORD thread_error = ERROR_SUCCESS;
        if (!thread.is_valid()) {
            thread_error = LastWindowsError();
        } else {
            const DWORD owner_pid = ::GetProcessIdOfThread(thread.get());
            if (!owner_pid)
                thread_error = LastWindowsError();
            else if (owner_pid != pid_)
                thread_error = ERROR_INVALID_THREAD_ID;
        }
        if (thread_error) {
            file.Close();
            ::DeleteFileW(temporary.value().c_str());
            return thread_error;
        }
        auto json = base::WriteJson(report);
        MINIDUMP_USER_STREAM stream {};
        MINIDUMP_USER_STREAM_INFORMATION streams {};
        if (json) {
            stream.Type = 0x4d450001;
            stream.BufferSize = static_cast<ULONG>(json->size());
            stream.Buffer = json->data();
            streams.UserStreamCount = 1;
            streams.UserStreamArray = &stream;
        }
        ULONGLONG deadline = ::GetTickCount64() + 8000;
        MINIDUMP_CALLBACK_INFORMATION callback { &DumpCallback, &deadline };
        // The nonblocking host-wide capture gate serializes DbgHelp calls.
        const BOOL success = ::MiniDumpWriteDump(process_.get(), pid_, file.get(),
            static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo
                | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData),
            &exception_info, &streams, &callback);
        DWORD error = success ? ERROR_SUCCESS : LastWindowsError();
        file.Close();
        if (success && !::MoveFileExW(temporary.value().c_str(), destination.value().c_str(), MOVEFILE_WRITE_THROUGH))
            error = LastWindowsError();
        if (error)
            ::DeleteFileW(temporary.value().c_str());
        else
            *dump_path = destination;
        return error;
    }

    void Run()
    {
        const HANDLE handles[] = { stop_.get(), requested_.get(), process_.get() };
        for (;;) {
            DWORD result = ::WaitForMultipleObjects(3, handles, FALSE, 250);
            DrainJobNotifications();
            if (result == WAIT_OBJECT_0)
                break;
            if (result == WAIT_OBJECT_0 + 1) {
                Capture();
                break;
            }
            SampleMemory();
            if (result == WAIT_OBJECT_0 + 2 || result == WAIT_FAILED)
                break;
        }
    }

    base::win::ScopedHandle mapping_, requested_, completed_, stop_, thread_;
    base::win::ScopedHandle process_, job_, port_;
    CrashRecord* record_ = nullptr;
    DWORD pid_ = 0;
    DWORD notification_error_ = 0;
    base::FilePath directory_;
    base::Value::Dict image_;
    std::mutex lock_;
    base::Value::Dict result_;
    std::atomic<bool> memory_limit_hit_ { false };
    std::atomic<uint64_t> memory_limit_tick_ { 0 };
    std::atomic<bool> captured_ { false };
    uint64_t private_bytes_ = 0, peak_commit_ = 0, process_limit_ = 0, peak_job_commit_ = 0;
    uint64_t system_commit_limit_ = 0, system_commit_available_ = 0, physical_available_ = 0;
};

std::shared_ptr<RendererCrashMonitor> CreateRendererCrashMonitor(std::string* error)
{
    auto monitor = std::make_shared<RendererCrashMonitor>();
    return monitor->Initialize(error) ? monitor : nullptr;
}

RendererCrashHandles GetRendererCrashHandles(const std::shared_ptr<RendererCrashMonitor>& monitor)
{
    return monitor->handles();
}

bool StartRendererCrashMonitor(const std::shared_ptr<RendererCrashMonitor>& monitor,
    HANDLE process, HANDLE job, const std::string& directory, std::string* error)
{
    return monitor->Start(process, job, directory, error);
}

void StopRendererCrashMonitor(const std::shared_ptr<RendererCrashMonitor>& monitor)
{
    if (monitor)
        monitor->Stop();
}

bool WaitForRendererCrashExit(const std::shared_ptr<RendererCrashMonitor>& monitor)
{
    return monitor && monitor->WaitForExit();
}


std::optional<bool> RendererMemoryLimitExceeded(const std::shared_ptr<RendererCrashMonitor>& monitor)
{
    return monitor ? monitor->MemoryLimitExceeded() : std::nullopt;
}

void AppendRendererCrashDiagnostics(const std::shared_ptr<RendererCrashMonitor>& monitor,
    base::Value::Dict* details)
{
    if (monitor)
        monitor->Append(details);
}

bool InitializeRendererCrashDiagnostics(RendererCrashHandles handles)
{
    auto* record = static_cast<CrashRecord*>(::MapViewOfFile(handles.mapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(CrashRecord)));
    if (!record || record->magic != kCrashMagic || record->version != kCrashVersion)
        return false;
    g_crash_record = record;
    g_crash_requested = handles.requested;
    g_crash_completed = handles.completed;
    if (!::AddVectoredExceptionHandler(1, &ObserveOom))
        return false;
    ::SetUnhandledExceptionFilter(&ObserveUnhandled);
    ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    return true;
}

void RecordRendererV8Memory(v8::Isolate* isolate) noexcept
{
    if (g_crash_record)
        SampleV8(isolate, &g_crash_record->main_sample);
}

void RecordRendererV8OOM(const char* location, bool is_heap_oom, const char* detail) noexcept
{
    if (!g_crash_record || ::InterlockedCompareExchange(&g_crash_record->oom_recorded, 1, 0))
        return;
    g_crash_record->oom_thread_id = ::GetCurrentThreadId();
    CopyCrashText(g_crash_record->location, location);
    CopyCrashText(g_crash_record->detail, detail);
    ::InterlockedExchange(&g_crash_record->oom_kind, is_heap_oom ? 1 : 2);
}
} // namespace atom
#endif // BUILDFLAG(IS_WIN)
