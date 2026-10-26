// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "src/runtime/electron/common/renderer_platform.h"

#if BUILDFLAG(IS_WIN)

#include <userenv.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/process/kill.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/win/scoped_handle.h"
#include "src/runtime/electron/common/renderer_crash_diagnostics.h"
#include "src/runtime/electron/common/renderer_protocol.h"
#include "v8/include/v8-isolate.h"

namespace atom {
namespace {

constexpr wchar_t kAppContainerName[] = L"mini-electron.renderer";
constexpr size_t kFrameMappingSize = sizeof(renderer_protocol::FrameBufferHeader)
    + renderer_protocol::kMaxFrameBytes;

constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kMaximumRendererMemoryLimit = 8 * kGiB;

size_t RendererMemoryLimit()
{
#if defined(_M_X64)
    const uint64_t physical_memory = base::SysInfo::AmountOfPhysicalMemory();
    v8::ResourceConstraints constraints;
    constraints.ConfigureDefaults(
        physical_memory, base::SysInfo::AmountOfVirtualMemory());
    const uint64_t default_heap =
        static_cast<uint64_t>(constraints.max_old_generation_size_in_bytes())
        + constraints.max_young_generation_size_in_bytes();
    // Preserve V8's machine-derived defaults, independently of the Job. Budget
    // half of RAM with a floor covering twice the default old+young heap and at
    // least 2 GiB for native allocations. The supported x64 V8 heap fits its
    // 4-GiB pointer-compression cage, so the 8-GiB ceiling preserves that floor.
    const uint64_t with_headroom = std::max(
        default_heap * 2, default_heap + 2 * kGiB);
    return static_cast<size_t>(std::clamp<uint64_t>(
        std::max(physical_memory / 2, with_headroom),
        2 * kGiB, kMaximumRendererMemoryLimit));
#else
    return static_cast<size_t>(kGiB);
#endif
}

struct LocalFreeDeleter {
    void operator()(void* value) const { ::LocalFree(value); }
};
using ScopedLocal = std::unique_ptr<void, LocalFreeDeleter>;

bool CreateInheritedPipe(base::win::ScopedHandle* parent_end,
    base::win::ScopedHandle* child_end, bool child_reads)
{
    SECURITY_ATTRIBUTES attributes { sizeof(attributes), nullptr, TRUE };
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!::CreatePipe(&read_handle, &write_handle, &attributes, 0))
        return false;
    base::win::ScopedHandle read(read_handle);
    base::win::ScopedHandle write(write_handle);
    HANDLE parent = child_reads ? write.get() : read.get();
    if (!::SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0))
        return false;
    if (child_reads) {
        *parent_end = std::move(write);
        *child_end = std::move(read);
    } else {
        *parent_end = std::move(read);
        *child_end = std::move(write);
    }
    return true;
}


bool CreateRendererToken(base::win::ScopedHandle* restricted_token,
    ScopedLocal* appcontainer_sid)
{
    HANDLE process_token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY
            | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            &process_token))
        return false;
    base::win::ScopedHandle token(process_token);
    HANDLE restricted = nullptr;
    if (!::CreateRestrictedToken(token.get(), DISABLE_MAX_PRIVILEGE, 0, nullptr,
            0, nullptr, 0, nullptr, &restricted))
        return false;
    restricted_token->Set(restricted);

    PSID sid = nullptr;
    HRESULT profile = ::CreateAppContainerProfile(kAppContainerName,
        L"mini-electron renderer", L"Sandboxed Blink renderer", nullptr, 0, &sid);
    if (profile == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        profile = ::DeriveAppContainerSidFromAppContainerName(kAppContainerName, &sid);
    if (FAILED(profile) || !sid)
        return false;
    appcontainer_sid->reset(sid);
    return true;
}

std::wstring HandleArgument(const wchar_t* name, HANDLE handle)
{
    return std::wstring(L" --") + name + L"="
        + base::NumberToWString(reinterpret_cast<uintptr_t>(handle));
}

} // namespace

std::vector<std::string> GetPreferredSystemLanguages()
{
    ULONG count = 0;
    ULONG size = 0;
    std::vector<std::string> languages;
    if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size)) {
        std::vector<wchar_t> buffer(size);
        if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)) {
            languages.reserve(count);
            const wchar_t* tag = buffer.data();
            for (ULONG index = 0; index < count && *tag; ++index) {
                const std::wstring_view language(tag);
                languages.push_back(base::WideToUTF8(language));
                tag += language.size() + 1;
            }
        }
    }
    if (languages.empty()) {
        wchar_t locale[LOCALE_NAME_MAX_LENGTH];
        if (::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH))
            languages.push_back(base::WideToUTF8(locale));
    }
    return languages;
}

bool LaunchRendererSandbox(uint64_t auth_high, uint64_t auth_low,
    RendererPlatformProcess* output, std::string* error)
{
    base::win::ScopedHandle parent_read;
    base::win::ScopedHandle child_write;
    base::win::ScopedHandle parent_write;
    base::win::ScopedHandle child_read;
    base::win::ScopedHandle parent_control;
    base::win::ScopedHandle child_control;
    if (!CreateInheritedPipe(&parent_read, &child_write, false)
        || !CreateInheritedPipe(&parent_write, &child_read, true)
        || !CreateInheritedPipe(&parent_control, &child_control, true)) {
        *error = "failed to create private renderer pipes";
        return false;
    }
    auto crash_monitor = CreateRendererCrashMonitor(error);
    if (!crash_monitor)
        return false;
    const auto crash_handles = GetRendererCrashHandles(crash_monitor);


    SECURITY_ATTRIBUTES inherited { sizeof(inherited), nullptr, TRUE };
    base::win::ScopedHandle frame_mapping(::CreateFileMappingW(INVALID_HANDLE_VALUE,
        &inherited, PAGE_READWRITE, 0, static_cast<DWORD>(kFrameMappingSize), nullptr));
    if (!frame_mapping.is_valid()) {
        *error = "failed to create renderer frame mapping";
        return false;
    }
    void* initialize = ::MapViewOfFile(frame_mapping.get(), FILE_MAP_WRITE, 0, 0,
        sizeof(renderer_protocol::FrameBufferHeader));
    if (!initialize) {
        *error = "failed to initialize renderer frame mapping";
        return false;
    }
    auto* header = new (initialize) renderer_protocol::FrameBufferHeader();
    header->capacity = static_cast<uint32_t>(renderer_protocol::kMaxFrameBytes);
    ::UnmapViewOfFile(initialize);

    base::win::ScopedHandle restricted_token;
    ScopedLocal appcontainer_sid;
    if (!CreateRendererToken(&restricted_token, &appcontainer_sid)) {
        *error = "failed to create AppContainer restricted token";
        return false;
    }

    SECURITY_CAPABILITIES security_capabilities {};
    security_capabilities.AppContainerSid = appcontainer_sid.get();
    security_capabilities.CapabilityCount = 0;
    security_capabilities.Capabilities = nullptr;

    SIZE_T attribute_bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
    std::vector<uint8_t> attribute_storage(attribute_bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!::InitializeProcThreadAttributeList(attributes, 2, 0, &attribute_bytes)) {
        *error = "failed to initialize renderer process attributes";
        return false;
    }
    auto delete_attributes = std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(&::DeleteProcThreadAttributeList)>(
        attributes, &::DeleteProcThreadAttributeList);
    if (!::UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &security_capabilities,
            sizeof(security_capabilities), nullptr, nullptr)) {
        *error = "failed to set renderer AppContainer attributes";
        return false;
    }
    HANDLE inherited_handles[] = { child_read.get(), child_write.get(),
        child_control.get(), frame_mapping.get(), crash_handles.mapping,
        crash_handles.requested, crash_handles.completed };
    if (!::UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles,
            sizeof(inherited_handles), nullptr, nullptr)) {
        *error = "failed to restrict renderer handle inheritance";
        return false;
    }

    base::win::ScopedHandle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job.is_valid()) {
        *error = "failed to create renderer job";
        return false;
    }
    const size_t memory_limit = RendererMemoryLimit();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS
        | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION
        | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = memory_limit;
    if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        *error = "failed to configure renderer job";
        return false;
    }
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui {};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS
        | JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS
        | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD
        | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (!::SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &ui, sizeof(ui))) {
        *error = "failed to configure renderer UI restrictions";
        return false;
    }

    wchar_t executable[MAX_PATH];
    DWORD executable_size = ::GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!executable_size || executable_size == MAX_PATH) {
        *error = "failed to locate renderer executable";
        return false;
    }
    std::wstring command = L"\"" + std::wstring(executable, executable_size)
        + L"\" --type=renderer";
    command += HandleArgument(L"renderer-read-handle", child_read.get());
    command += HandleArgument(L"renderer-write-handle", child_write.get());
    command += HandleArgument(L"renderer-control-handle", child_control.get());
    command += HandleArgument(L"renderer-frame-handle", frame_mapping.get());
    command += L" --renderer-frame-size=" + base::NumberToWString(kFrameMappingSize);
    command += HandleArgument(L"renderer-crash-mapping", crash_handles.mapping);
    command += HandleArgument(L"renderer-crash-requested", crash_handles.requested);
    command += HandleArgument(L"renderer-crash-completed", crash_handles.completed);
    command += L" --renderer-auth-high=" + base::NumberToWString(auth_high);
    command += L" --renderer-auth-low=" + base::NumberToWString(auth_low);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process_info {};
    DWORD creation_flags = CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT
        | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    if (!::CreateProcessAsUserW(restricted_token.get(), executable,
            mutable_command.data(), nullptr, nullptr, TRUE, creation_flags, nullptr,
            nullptr, &startup.StartupInfo, &process_info)) {
        *error = "failed to launch renderer with AppContainer restricted token";
        return false;
    }
    base::win::ScopedHandle process(process_info.hProcess);
    base::win::ScopedHandle thread(process_info.hThread);
    if (!::AssignProcessToJobObject(job.get(), process.get())) {
        ::TerminateProcess(process.get(), 1);
        *error = "failed to assign renderer to restricted job";
        return false;
    }
    if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        ::TerminateProcess(process.get(), 1);
        *error = "failed to resume renderer process";
        return false;
    }

    void* frame_memory = ::MapViewOfFile(frame_mapping.get(), FILE_MAP_READ, 0, 0,
        kFrameMappingSize);
    if (!frame_memory) {
        ::TerminateProcess(process.get(), 1);
        *error = "failed to map renderer frame read-only";
        return false;
    }

    output->read_pipe = parent_read.Take();
    output->write_pipe = parent_write.Take();
    output->control_pipe = parent_control.Take();
    output->frame_mapping = frame_mapping.Take();
    output->frame_memory = frame_memory;
    output->frame_mapping_size = kFrameMappingSize;
    output->process_id = static_cast<int>(process_info.dwProcessId);
    output->process = process.Take();
    output->job = job.Take();
    output->crash_monitor = std::move(crash_monitor);
    return true;
}

bool CreateInProcessRendererResources(RendererPlatformProcess* output,
    std::string* error)
{
    base::win::ScopedHandle mapping(::CreateFileMappingW(INVALID_HANDLE_VALUE,
        nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kFrameMappingSize), nullptr));
    if (!mapping.is_valid()) {
        *error = "failed to create local renderer frame mapping";
        return false;
    }
    HANDLE current_process = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(),
            ::GetCurrentProcess(), &current_process, 0, FALSE,
            DUPLICATE_SAME_ACCESS)) {
        *error = "failed to open local renderer process handle";
        return false;
    }
    base::win::ScopedHandle process(current_process);
    void* memory = ::MapViewOfFile(mapping.get(),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kFrameMappingSize);
    if (!memory) {
        *error = "failed to map local renderer frame";
        return false;
    }
    auto* header = new (memory) renderer_protocol::FrameBufferHeader();
    header->capacity = static_cast<uint32_t>(renderer_protocol::kMaxFrameBytes);
    output->frame_memory = memory;
    output->frame_mapping_size = kFrameMappingSize;
    output->frame_mapping = mapping.Take();
    output->process = process.Take();
    output->process_id = static_cast<int>(::GetCurrentProcessId());
    return true;
}

bool IsRendererProcessAlive(const RendererPlatformProcess& process)
{
    return process.process && ::WaitForSingleObject(process.process, 0) == WAIT_TIMEOUT;
}

RendererProcessStatus ObserveRendererProcess(RendererPlatformProcess* process,
    int timeout_milliseconds)
{
    RendererProcessStatus status;
    if (!process)
        return status;
    if (process->termination_observed) {
        status.state = RendererProcessState::kExited;
        status.exit_code = process->exit_code;
        status.crashed = process->crashed;
        status.sandbox_memory_limit_exceeded =
            process->sandbox_memory_limit_exceeded;
        return status;
    }
    if (!process->process
        || ::WaitForSingleObject(process->process,
               static_cast<DWORD>(std::max(0, timeout_milliseconds)))
            != WAIT_OBJECT_0) {
        return status;
    }

    DWORD exit_code = STILL_ACTIVE;
    if (!::GetExitCodeProcess(process->process, &exit_code)
        || exit_code == STILL_ACTIVE) {
        return status;
    }
    process->sandbox_memory_limit_exceeded = exit_code == base::win::kSandboxFatalMemoryExceeded
        ? std::optional<bool>(true) : RendererMemoryLimitExceeded(process->crash_monitor);
    process->termination_observed = true;
    process->exit_code = exit_code;
    process->crashed = (exit_code & 0x80000000u) != 0;
    process->process_id = 0;

    status.state = RendererProcessState::kExited;
    status.exit_code = process->exit_code;
    status.crashed = process->crashed;
    status.sandbox_memory_limit_exceeded =
        process->sandbox_memory_limit_exceeded;
    return status;
}

bool WaitForRendererProcess(RendererPlatformProcess* process, int timeout_milliseconds)
{
    return ObserveRendererProcess(process, timeout_milliseconds).state
        == RendererProcessState::kExited;
}

void TerminateRendererProcess(RendererPlatformProcess* process)
{
    if (process->job)
        ::TerminateJobObject(process->job, 1);
    else if (process->process)
        ::TerminateProcess(process->process, 1);
}

void CloseRendererPlatformProcess(RendererPlatformProcess* process)
{
    StopRendererCrashMonitor(process->crash_monitor);
    process->crash_monitor.reset();
    if (process->frame_memory)
        ::UnmapViewOfFile(process->frame_memory);
    process->frame_memory = nullptr;
    if (process->control_pipe)
        ::CloseHandle(process->control_pipe);
    if (process->frame_mapping)
        ::CloseHandle(process->frame_mapping);
    if (process->process)
        ::CloseHandle(process->process);
    if (process->job)
        ::CloseHandle(process->job);
    process->control_pipe = nullptr;
    process->frame_mapping = nullptr;
    process->process = nullptr;
    process->job = nullptr;
}

bool ShareRendererFrameBuffer(const RendererPlatformProcess& source,
    RendererPlatformProcess* target, int guest_contents_id, std::string* error)
{
    HANDLE remote = nullptr;
    if (!target || !source.frame_mapping || !source.frame_mapping_size
        || !target->process || !target->control_pipe
        || !::DuplicateHandle(::GetCurrentProcess(), source.frame_mapping,
            target->process, &remote, FILE_MAP_READ, FALSE, 0)) {
        *error = "failed to duplicate guest frame mapping";
        return false;
    }
    renderer_protocol::FrameControlMessage message {};
    message.guest_contents_id = guest_contents_id;
    message.frame_handle = reinterpret_cast<uintptr_t>(remote);
    message.frame_mapping_size = source.frame_mapping_size;
    DWORD written = 0;
    if (!::WriteFile(target->control_pipe, &message, sizeof(message), &written, nullptr)
        || written != sizeof(message)) {
        HANDLE closed = nullptr;
        ::DuplicateHandle(target->process, remote, ::GetCurrentProcess(), &closed,
            0, FALSE, DUPLICATE_CLOSE_SOURCE);
        if (closed)
            ::CloseHandle(closed);
        *error = "failed to register guest frame mapping";
        return false;
    }
    return true;
}

bool EnterRendererSandbox(std::string* error)
{
    HANDLE raw_token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        *error = "renderer cannot inspect process token";
        return false;
    }
    base::win::ScopedHandle token(raw_token);
    DWORD is_appcontainer = 0;
    DWORD has_restrictions = 0;
    DWORD size = 0;
    if (!::GetTokenInformation(token.get(), TokenIsAppContainer, &is_appcontainer,
            sizeof(is_appcontainer), &size) || !is_appcontainer
        || !::GetTokenInformation(token.get(), TokenHasRestrictions,
            &has_restrictions, sizeof(has_restrictions), &size)
        || !has_restrictions) {
        *error = "renderer process is not AppContainer restricted";
        return false;
    }
    BOOL in_job = FALSE;
    if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) || !in_job) {
        *error = "renderer process is not job restricted";
        return false;
    }
    return true;
}

} // namespace atom

#endif // BUILDFLAG(IS_WIN)
