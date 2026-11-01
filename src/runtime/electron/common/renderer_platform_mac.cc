// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "src/runtime/electron/common/renderer_platform.h"

#if BUILDFLAG(IS_MAC)

#include <CoreFoundation/CoreFoundation.h>

#include <mach-o/dyld.h>
#include <fcntl.h>
#include <sandbox.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/rand_util.h"
#include "base/command_line.h"
#include "src/runtime/electron/common/renderer_crash_diagnostics.h"
#include "base/strings/string_number_conversions.h"
#include "src/runtime/electron/common/renderer_protocol.h"
#include "src/platform/macos/metal_compositor.h"
#include "src/runtime/engine/renderer/webgl_context_provider.h"
#include "src/runtime/engine/renderer/webgpu_context_provider.h"
#include "third_party/libcurl/include/curl/curl.h"
#include "third_party/icu/source/common/unicode/uclean.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"

extern char** environ;

namespace atom {
namespace {

constexpr int kRendererReadFd = 198;
constexpr int kRendererWriteFd = 199;
constexpr int kRendererFrameFd = 200;
constexpr int kRendererControlFd = 201;
constexpr size_t kFrameMappingSize = sizeof(renderer_protocol::FrameBufferHeader)
    + renderer_protocol::kMaxFrameBytes;

bool SetCloseOnExec(int fd)
{
    int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

std::string ExecutablePath()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return {};
    return path.data();
}

std::string SandboxQuoted(std::string value)
{
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    for (char character : value) {
        if (character == '\\' || character == '"')
            output.push_back('\\');
        output.push_back(character);
    }
    output.push_back('"');
    return output;
}

} // namespace

std::vector<std::string> GetPreferredSystemLanguages()
{
    CFArrayRef preferred = CFLocaleCopyPreferredLanguages();
    if (!preferred)
        return {};
    const CFIndex count = CFArrayGetCount(preferred);
    std::vector<std::string> languages;
    languages.reserve(count);
    for (CFIndex index = 0; index < count; ++index) {
        auto language = static_cast<CFStringRef>(CFArrayGetValueAtIndex(preferred, index));
        char tag[256];
        if (CFStringGetCString(language, tag, sizeof(tag), kCFStringEncodingUTF8))
            languages.emplace_back(tag);
    }
    CFRelease(preferred);
    return languages;
}

bool LaunchRendererSandbox(uint64_t auth_high, uint64_t auth_low,
    RendererPlatformProcess* output, std::string* error)
{
    auto diagnostics = CreateRendererCrashMonitor(error);
    if (!diagnostics)
        return false;
    const int diagnostics_mapping = MacRendererDiagnosticsMapping(diagnostics);
    const int diagnostics_socket = MacRendererDiagnosticsSocket(diagnostics);
    int to_child[2] = { -1, -1 };
    int from_child[2] = { -1, -1 };
    int control[2] = { -1, -1 };
    // macOS has no AF_UNIX SOCK_SEQPACKET; datagram socketpairs keep the
    // fixed-size control message boundaries the renderer expects.
    if (::pipe(to_child) || ::pipe(from_child)
        || ::socketpair(AF_UNIX, SOCK_DGRAM, 0, control)) {
        for (int fd : { to_child[0], to_child[1], from_child[0], from_child[1],
                 control[0], control[1] }) {
            if (fd >= 0)
                ::close(fd);
        }
        *error = "failed to create private renderer channels";
        return false;
    }
    auto close_channels = [&] {
        for (int fd : { to_child[0], to_child[1], from_child[0], from_child[1],
                 control[0], control[1] }) {
            if (fd >= 0)
                ::close(fd);
        }
    };
    for (int fd : { to_child[0], to_child[1], from_child[0], from_child[1],
             control[0], control[1] }) {
        if (!SetCloseOnExec(fd)) {
            close_channels();
            *error = "failed to protect renderer channel descriptors";
            return false;
        }
    }

    // macOS limits POSIX shared memory names to PSHMNAMLEN (31) characters;
    // the name is unlinked immediately after both descriptors are opened.
    std::string shared_name = "/me-r-"
        + base::NumberToString(base::RandUint64());
    int frame_fd = ::shm_open(shared_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (frame_fd < 0) {
        close_channels();
        *error = "failed to create anonymous renderer frame mapping";
        return false;
    }
    if (::ftruncate(frame_fd, kFrameMappingSize)) {
        ::close(frame_fd);
        ::shm_unlink(shared_name.c_str());
        close_channels();
        *error = "failed to size anonymous renderer frame mapping";
        return false;
    }
    // SCM_RIGHTS preserves the open-file access mode. Keep a separate
    // descriptor that the guest cannot remap writable or resize.
    int frame_read_fd = ::shm_open(shared_name.c_str(), O_RDONLY, 0);
    if (frame_read_fd < 0 || !SetCloseOnExec(frame_fd)
        || !SetCloseOnExec(frame_read_fd) || ::shm_unlink(shared_name.c_str())) {
        if (frame_read_fd >= 0)
            ::close(frame_read_fd);
        ::close(frame_fd);
        ::shm_unlink(shared_name.c_str());
        close_channels();
        *error = "failed to protect renderer frame descriptors";
        return false;
    }
    void* initialize = ::mmap(nullptr, sizeof(renderer_protocol::FrameBufferHeader),
        PROT_READ | PROT_WRITE, MAP_SHARED, frame_fd, 0);
    if (initialize == MAP_FAILED) {
        ::close(frame_read_fd);
        ::close(frame_fd);
        close_channels();
        *error = "failed to initialize renderer frame mapping";
        return false;
    }
    auto* header = new (initialize) renderer_protocol::FrameBufferHeader();
    header->capacity = static_cast<uint32_t>(renderer_protocol::kMaxFrameBytes);
    ::munmap(initialize, sizeof(*header));

    std::string executable = ExecutablePath();
    if (executable.empty()) {
        ::close(frame_read_fd);
        ::close(frame_fd);
        close_channels();
        *error = "failed to locate renderer executable";
        return false;
    }
    std::vector<std::string> arguments = {
        executable,
        "--type=renderer",
        "--renderer-read-handle=" + base::NumberToString(kRendererReadFd),
        "--renderer-write-handle=" + base::NumberToString(kRendererWriteFd),
        "--renderer-control-handle=" + base::NumberToString(kRendererControlFd),
        "--renderer-frame-handle=" + base::NumberToString(kRendererFrameFd),
        "--renderer-frame-size=" + base::NumberToString(kFrameMappingSize),
        "--renderer-auth-high=" + base::NumberToString(auth_high),
        "--renderer-auth-low=" + base::NumberToString(auth_low),
        "--renderer-diagnostics-mapping=202",
        "--renderer-diagnostics-socket=203",
    };
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    // A source can already occupy any of the six child destinations. Preserve
    // every source before constructing actions so no dup2 overwrites another
    // channel (including the dedicated diagnostics transport).
    const int sources[] = { to_child[0], from_child[1], frame_fd, control[1],
        diagnostics_mapping, diagnostics_socket };
    const int destinations[] = { kRendererReadFd, kRendererWriteFd,
        kRendererFrameFd, kRendererControlFd, 202, 203 };
    int safe_sources[] = { -1, -1, -1, -1, -1, -1 };
    auto close_safe_sources = [&] {
        for (int fd : safe_sources) {
            if (fd >= 0)
                ::close(fd);
        }
    };
    for (size_t index = 0; index < 6; ++index) {
        if (!SetCloseOnExec(sources[index])) {
            const int descriptor_error = errno;
            close_safe_sources();
            ::close(frame_read_fd);
            ::close(frame_fd);
            close_channels();
            *error = std::string("failed to protect renderer source: ")
                + std::strerror(descriptor_error);
            return false;
        }
        safe_sources[index] = ::fcntl(sources[index], F_DUPFD_CLOEXEC, 204);
        if (safe_sources[index] < 0) {
            const int descriptor_error = errno;
            close_safe_sources();
            ::close(frame_read_fd);
            ::close(frame_fd);
            close_channels();
            *error = std::string("failed to preserve renderer source: ")
                + std::strerror(descriptor_error);
            return false;
        }
    }

    posix_spawn_file_actions_t actions;
    int spawn_error = posix_spawn_file_actions_init(&actions);
    const bool actions_initialized = spawn_error == 0;
    // CLOEXEC_DEFAULT closes even standard streams unless explicitly inherited.
    // Preserve the browser's diagnostic stream without inheriting other handles.
    if (!spawn_error) {
        if (::fcntl(STDERR_FILENO, F_GETFD) >= 0)
            spawn_error = posix_spawn_file_actions_adddup2(
                &actions, STDERR_FILENO, STDERR_FILENO);
        else if (errno != EBADF)
            spawn_error = errno;
    }
    for (size_t index = 0; !spawn_error && index < 6; ++index)
        spawn_error = posix_spawn_file_actions_adddup2(
            &actions, safe_sources[index], destinations[index]);
    for (int fd : safe_sources) {
        if (!spawn_error)
            spawn_error = posix_spawn_file_actions_addclose(&actions, fd);
    }
    for (int fd : { to_child[0], to_child[1], from_child[0], from_child[1],
             frame_fd, frame_read_fd, control[0], control[1],
             diagnostics_mapping, diagnostics_socket }) {
        // Originals in this interval now name destinations, not their old
        // channels. All other originals can be explicitly closed in the child.
        if (!spawn_error && (fd < kRendererReadFd || fd > 203))
            spawn_error = posix_spawn_file_actions_addclose(&actions, fd);
    }
    posix_spawnattr_t attributes;
    bool attributes_initialized = false;
    if (!spawn_error) {
        spawn_error = posix_spawnattr_init(&attributes);
        attributes_initialized = spawn_error == 0;
    }
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    if (!spawn_error)
        spawn_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t pid = 0;
    if (!spawn_error)
        spawn_error = ::posix_spawn(&pid, executable.c_str(), &actions,
            &attributes, argv.data(), environ);
    if (attributes_initialized)
        posix_spawnattr_destroy(&attributes);
    if (actions_initialized)
        posix_spawn_file_actions_destroy(&actions);
    close_safe_sources();
    ::close(to_child[0]);
    to_child[0] = -1;
    ::close(from_child[1]);
    from_child[1] = -1;
    ::close(control[1]);
    control[1] = -1;
    if (spawn_error) {
        ::close(frame_read_fd);
        ::close(frame_fd);
        close_channels();
        *error = std::string("failed to spawn renderer: ") + std::strerror(spawn_error);
        return false;
    }

    void* frame_memory = ::mmap(nullptr, kFrameMappingSize, PROT_READ,
        MAP_SHARED, frame_read_fd, 0);
    if (frame_memory == MAP_FAILED) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        ::close(frame_read_fd);
        ::close(frame_fd);
        close_channels();
        *error = "failed to map renderer frame read-only";
        return false;
    }
    output->read_pipe = from_child[0];
    output->write_pipe = to_child[1];
    output->control_socket = control[0];
    output->frame_fd = frame_fd;
    output->frame_read_fd = frame_read_fd;
    output->frame_memory = frame_memory;
    output->frame_mapping_size = kFrameMappingSize;
    output->process_id = static_cast<int>(pid);
    output->crash_monitor = std::move(diagnostics);
    return true;
}

bool CreateInProcessRendererResources(RendererPlatformProcess* output,
    std::string* error)
{
    const std::string name = "/me-r-"
        + base::NumberToString(base::RandUint64());
    int frame_fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    int read_fd = frame_fd >= 0 ? ::shm_open(name.c_str(), O_RDONLY, 0) : -1;
    const bool unlinked = frame_fd < 0 || ::shm_unlink(name.c_str()) == 0;
    void* memory = MAP_FAILED;
    if (frame_fd >= 0 && read_fd >= 0 && unlinked
        && SetCloseOnExec(frame_fd) && SetCloseOnExec(read_fd)
        && ::ftruncate(frame_fd, kFrameMappingSize) == 0) {
        memory = ::mmap(nullptr, kFrameMappingSize, PROT_READ | PROT_WRITE,
            MAP_SHARED, frame_fd, 0);
    }
    if (memory == MAP_FAILED) {
        if (frame_fd >= 0)
            ::close(frame_fd);
        if (read_fd >= 0)
            ::close(read_fd);
        if (!unlinked)
            ::shm_unlink(name.c_str());
        *error = "failed to allocate local renderer frame";
        return false;
    }
    auto* header = new (memory) renderer_protocol::FrameBufferHeader();
    header->capacity = static_cast<uint32_t>(renderer_protocol::kMaxFrameBytes);
    output->frame_memory = memory;
    output->frame_mapping_size = kFrameMappingSize;
    output->frame_fd = frame_fd;
    output->frame_read_fd = read_fd;
    output->process_id = static_cast<int>(::getpid());
    return true;
}

bool IsRendererProcessAlive(const RendererPlatformProcess& process)
{
    return process.process_id > 0
        && (::kill(static_cast<pid_t>(process.process_id), 0) == 0 || errno == EPERM);
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
        return status;
    }
    if (process->process_id <= 0)
        return status;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(0, timeout_milliseconds));
    for (;;) {
        int wait_status = 0;
        const pid_t result = ::waitpid(
            static_cast<pid_t>(process->process_id), &wait_status, WNOHANG);
        if (result == process->process_id) {
            process->termination_observed = true;
            if (WIFEXITED(wait_status)) {
                process->exit_code =
                    static_cast<uint32_t>(WEXITSTATUS(wait_status));
            } else if (WIFSIGNALED(wait_status)) {
                process->exit_code =
                    static_cast<uint32_t>(128 + WTERMSIG(wait_status));
                process->crashed = true;
            }
            process->process_id = 0;
            status.state = RendererProcessState::kExited;
            status.exit_code = process->exit_code;
            status.crashed = process->crashed;
            return status;
        }
        if (result < 0 && errno != EINTR)
            return status;
        if (std::chrono::steady_clock::now() >= deadline)
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool WaitForRendererProcess(RendererPlatformProcess* process, int timeout_milliseconds)
{
    return ObserveRendererProcess(process, timeout_milliseconds).state
        == RendererProcessState::kExited;
}

void TerminateRendererProcess(RendererPlatformProcess* process)
{
    if (process->process_id <= 0)
        return;
    ::kill(static_cast<pid_t>(process->process_id), SIGKILL);
    ObserveRendererProcess(process, 2000);
}

void CloseRendererPlatformProcess(RendererPlatformProcess* process)
{
    StopRendererCrashMonitor(process->crash_monitor);
    if (process->frame_memory)
        ::munmap(process->frame_memory, process->frame_mapping_size);
    if (process->frame_fd >= 0)
        ::close(process->frame_fd);
    if (process->frame_read_fd >= 0)
        ::close(process->frame_read_fd);
    if (process->control_socket >= 0)
        ::close(process->control_socket);
    process->frame_memory = nullptr;
    process->frame_fd = -1;
    process->frame_read_fd = -1;
    process->control_socket = -1;
}

bool ShareRendererFrameBuffer(const RendererPlatformProcess& source,
    RendererPlatformProcess* target, int guest_contents_id, std::string* error)
{
    if (source.frame_read_fd < 0 || source.frame_mapping_size == 0
        || !target || target->control_socket < 0) {
        *error = "guest frame transfer is unavailable";
        return false;
    }
    renderer_protocol::FrameControlMessage control {};
    control.guest_contents_id = guest_contents_id;
    control.frame_mapping_size = source.frame_mapping_size;
    iovec vector { &control, sizeof(control) };
    char ancillary[CMSG_SPACE(sizeof(int))] = {};
    msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = ancillary;
    message.msg_controllen = sizeof(ancillary);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &source.frame_read_fd, sizeof(int));
    if (::sendmsg(target->control_socket, &message, MSG_NOSIGNAL) != sizeof(control)) {
        *error = "failed to transfer guest frame descriptor";
        return false;
    }
    return true;
}

bool EnterRendererSandbox(std::string* error)
{
    const auto* command_line = base::CommandLine::ForCurrentProcess();
    int diagnostics_mapping = -1, diagnostics_socket = -1;
    if (!base::StringToInt(command_line->GetSwitchValueASCII("renderer-diagnostics-mapping"), &diagnostics_mapping)
        || !base::StringToInt(command_line->GetSwitchValueASCII("renderer-diagnostics-socket"), &diagnostics_socket)
        || diagnostics_mapping != 202 || diagnostics_socket != 203) {
        *error = "invalid private renderer diagnostic arguments";
        return false;
    }
    if (!InitializeMacRendererCrashDiagnostics(diagnostics_mapping, diagnostics_socket)) {
        *error = std::string("failed to initialize private renderer diagnostics: ") + std::strerror(errno);
        return false;
    }
    std::string executable = ExecutablePath();
    size_t bundle_marker = executable.find(".app/");
    std::string bundle_directory;
    if (bundle_marker != std::string::npos)
        bundle_directory = executable.substr(0, bundle_marker + 4);
    else {
        size_t slash = executable.rfind('/');
        bundle_directory = slash == std::string::npos ? executable
                                                       : executable.substr(0, slash);
    }
    const std::string profile =
        "(version 1)\n"
        "(deny default)\n"
        "(allow process-info*)\n"
        "(allow sysctl-read)\n"
        "(allow ipc-posix*)\n"
        // CoreText checks the /System ancestor when resolving system-ui CJK
        // fonts. Reading /System/Library alone leaves their glyphs unavailable.
        "(allow file-read-metadata (literal \"/System\"))\n"
        "(allow file-read* (literal \"/dev/null\") (literal \"/dev/urandom\") "
        "(subpath \"/System/Library\") (subpath \"/Library/Fonts\") (subpath "
        + SandboxQuoted(bundle_directory) + "))\n"
        "(allow mach-lookup (global-name \"com.apple.FontObjectsServer\") "
        "(global-name \"com.apple.fonts\") "
        "(global-name \"com.apple.coreservices.launchservicesd\") "
        "(global-name \"com.apple.windowserver.active\"))\n"
        + ((mini_electron::mac::PrepareMetalDevice() || content::InitializeWebGL() || content::InitializeWebGPU())
                ? "(allow mach-lookup (xpc-service-name \"com.apple.MTLCompilerService\"))\n"
                : "");
    // The system libcurl initializes LibreSSL, which reads
    // /private/etc/ssl/openssl.cnf and calls exit(1) when that read is denied.
    // Initialize it before lockdown and keep this reference for the process
    // lifetime so the loader's own curl_global_init only increments the count.
    if (::curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        *error = "libcurl initialization failed";
        return false;
    }
    // ICU's first default-zone lookup reads /etc/localtime. Cache it before
    // Seatbelt denies that file so Date and Intl retain the system time zone.
    UErrorCode icu_error = U_ZERO_ERROR;
    u_init(&icu_error);
    if (U_FAILURE(icu_error)) {
        *error = std::string("ICU initialization failed: ") + u_errorName(icu_error);
        return false;
    }
    const std::unique_ptr<icu::TimeZone> time_zone(icu::TimeZone::createDefault());
    char* sandbox_error = nullptr;
    if (::sandbox_init(profile.c_str(), 0, &sandbox_error) != 0) {
        *error = sandbox_error ? sandbox_error : "seatbelt initialization failed";
        if (sandbox_error)
            ::sandbox_free_error(sandbox_error);
        return false;
    }
    return true;
}

} // namespace atom

#endif // BUILDFLAG(IS_MAC)
