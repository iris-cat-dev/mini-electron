// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/win/scoped_handle.h"
#include "src/runtime/electron/common/renderer_crash_diagnostics.h"
#include "src/runtime/electron/common/renderer_platform.h"

namespace {
constexpr DWORD kOomException = 0xe0000008;
HANDLE g_ipc_closed = nullptr;
HANDLE g_exit_released = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_original_filter = nullptr;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", message, ::GetLastError());
        std::abort();
    }
}

HANDLE ParseHandle(const char* argument)
{
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::strtoull(argument, nullptr, 10)));
}

LONG WINAPI DelayUnhandledExit(EXCEPTION_POINTERS* pointers)
{
    // Preserve production exception capture, then hold the process alive after
    // capture completion. This deterministically separates IPC loss from the
    // OS process-handle signal without any production timing/test hooks.
    const LONG result = g_original_filter(pointers);
    ::SetEvent(g_ipc_closed);
    ::WaitForSingleObject(g_exit_released, 15000);
    return result;
}

void RequireDumpContext(std::string& dump, DWORD exception, DWORD thread_id)
{
    MINIDUMP_DIRECTORY* directory = nullptr;
    void* stream = nullptr;
    ULONG size = 0;
    Require(::MiniDumpReadDumpStream(dump.data(), ExceptionStream,
                &directory, &stream, &size)
            && size >= sizeof(MINIDUMP_EXCEPTION_STREAM),
        "the final dump contains a usable exception stream");
    const auto* captured = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);
    Require(captured->ThreadId == thread_id
            && captured->ExceptionRecord.ExceptionCode == exception,
        "the dump preserves the original fatal thread and exception");
    const auto context = captured->ThreadContext;
    Require(context.DataSize >= sizeof(CONTEXT) && context.Rva <= dump.size()
            && context.DataSize <= dump.size() - context.Rva,
        "the exception context is complete and contained in the dump");
    CONTEXT registers;
    std::memcpy(&registers, dump.data() + context.Rva, sizeof(registers));
    Require((registers.ContextFlags & CONTEXT_CONTROL) == CONTEXT_CONTROL
            && registers.Rip != 0 && registers.Rsp != 0,
        "the dump retains the original AMD64 instruction and stack pointers");
}

int RunChild(int argc, char** argv)
{
    Require(argc == 8, "child receives crash-channel and synchronization handles");
    const DWORD exception = static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10));
    atom::RendererCrashHandles handles {
        ParseHandle(argv[3]), ParseHandle(argv[4]), ParseHandle(argv[5])
    };
    g_ipc_closed = ParseHandle(argv[6]);
    g_exit_released = ParseHandle(argv[7]);
    Require(atom::InitializeRendererCrashDiagnostics(handles),
        "child initializes production crash capture");
    if (!exception) {
        ::SetEvent(g_ipc_closed);
        Require(::WaitForSingleObject(g_exit_released, 15000) == WAIT_OBJECT_0,
            "ordinary live renderer is explicitly released");
        return 0;
    }
    g_original_filter = ::SetUnhandledExceptionFilter(&DelayUnhandledExit);
    Require(g_original_filter != nullptr, "production unhandled filter is retained");
    ::RaiseException(exception, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 1;
}

std::wstring HandleArgument(HANDLE handle)
{
    return L" " + std::to_wstring(reinterpret_cast<uintptr_t>(handle));
}

void ObserveIpcClose(DWORD exception)
{
    SECURITY_ATTRIBUTES inherited { sizeof(inherited), nullptr, TRUE };
    base::win::ScopedHandle ipc_closed(::CreateEventW(&inherited, TRUE, FALSE, nullptr));
    base::win::ScopedHandle exit_released(::CreateEventW(&inherited, TRUE, FALSE, nullptr));
    base::win::ScopedHandle job(::CreateJobObjectW(nullptr, nullptr));
    Require(ipc_closed.is_valid() && exit_released.is_valid() && job.is_valid(),
        "parent creates child synchronization and Job handles");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                &limits, sizeof(limits)) != FALSE,
        "the test owns only its disposable child process");

    std::string error;
    auto monitor = atom::CreateRendererCrashMonitor(&error);
    Require(monitor != nullptr, "parent creates the production crash monitor");
    const auto handles = atom::GetRendererCrashHandles(monitor);
    base::ScopedTempDir directory;
    Require(directory.CreateUniqueTempDir(), "parent creates a temporary dump directory");
    wchar_t executable[MAX_PATH];
    const DWORD executable_length = ::GetModuleFileNameW(nullptr, executable, MAX_PATH);
    Require(executable_length && executable_length < MAX_PATH, "parent locates this executable");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child "
        + std::to_wstring(exception) + HandleArgument(handles.mapping)
        + HandleArgument(handles.requested) + HandleArgument(handles.completed)
        + HandleArgument(ipc_closed.get()) + HandleArgument(exit_released.get());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child {};
    Require(::CreateProcessW(executable, mutable_command.data(), nullptr, nullptr,
                TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                &startup, &child) != FALSE,
        "parent launches a disposable child suspended");
    base::win::ScopedHandle process(child.hProcess);
    base::win::ScopedHandle thread(child.hThread);
    Require(::AssignProcessToJobObject(job.get(), process.get()) != FALSE,
        "parent assigns only the disposable child to its Job");
    Require(atom::StartRendererCrashMonitor(monitor, process.get(), job.get(),
                directory.GetPath().AsUTF8Unsafe(), &error),
        "parent starts production exception capture");
    atom::RendererPlatformProcess renderer;
    renderer.process = process.Take();
    renderer.job = job.Take();
    renderer.process_id = static_cast<int>(child.dwProcessId);
    renderer.crash_monitor = monitor;
    Require(::ResumeThread(thread.get()) != static_cast<DWORD>(-1), "parent resumes the child");
    Require(::WaitForSingleObject(ipc_closed.get(), 15000) == WAIT_OBJECT_0,
        "child reaches the simulated IPC-closed interval");

    const auto early = atom::ObserveRendererProcess(&renderer, 250);
    Require(early.state == atom::RendererProcessState::kRunning,
        "IPC loss does not fabricate an exit while the process is held alive");
    if (!exception) {
        Require(!atom::WaitForRendererCrashExit(monitor),
            "ordinary broken IPC does not enter the fatal-capture lifecycle");
        Require(atom::IsRendererProcessAlive(renderer),
            "ordinary broken IPC leaves the still-live renderer distinct");
        ::SetEvent(exit_released.get());
        const auto status = atom::ObserveRendererProcess(&renderer, 2000);
        Require(status.state == atom::RendererProcessState::kExited && !status.crashed
                && status.exit_code == 0,
            "ordinary renderer later reports its actual clean exit");
    } else {
        std::thread release([handle = exit_released.get()] {
            ::Sleep(500);
            ::SetEvent(handle);
        });
        const bool exited = atom::WaitForRendererCrashExit(monitor);
        const auto status = atom::ObserveRendererProcess(&renderer, 0);
        release.join();
        Require(exited && status.state == atom::RendererProcessState::kExited,
            "fatal capture waits past IPC loss until the real OS exit signal");
        Require(status.exit_code == exception && status.crashed,
            "fatal exit preserves the actual unsigned OOM/native exception code");
        base::Value::Dict details;
        atom::AppendRendererCrashDiagnostics(monitor, &details);
        const auto* capture_status = details.FindString("crashCaptureStatus");
        const auto* dump_path = details.FindString("crashDumpPath");
        Require(capture_status && *capture_status == "captured" && dump_path,
            "exit observation retains the completed production dump");
        Require(details.FindDouble("exceptionCode").value_or(0) == exception,
            "the dump records the exception actually returned by the OS");
        std::string dump;
        Require(base::ReadFileToString(base::FilePath::FromUTF8Unsafe(*dump_path), &dump)
                && dump.size() >= 32 && !std::memcmp(dump.data(), "MDMP", 4),
            "the final dump is a readable minidump, not a temporary path");
        RequireDumpContext(dump, exception,
            static_cast<DWORD>(details.FindDouble("exceptionThreadId").value_or(0)));
        Require(details.Find("crashDumpError") == nullptr,
            "the successful race case does not hide a capture error");
    }
    atom::CloseRendererPlatformProcess(&renderer);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--child"))
        return RunChild(argc, argv);
    ObserveIpcClose(kOomException);
    ObserveIpcClose(EXCEPTION_ACCESS_VIOLATION);
    ObserveIpcClose(0);
    std::puts("PASS: fatal capture waits for real OOM/native exits; live broken IPC stays distinct");
    return 0;
}
