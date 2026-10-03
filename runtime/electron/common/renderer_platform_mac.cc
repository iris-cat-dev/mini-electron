// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/common/renderer_platform.h"

#if BUILDFLAG(IS_MAC)

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
#include <string>
#include <thread>
#include <vector>

#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "runtime/electron/common/renderer_protocol.h"

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

bool LaunchRendererSandbox(uint64_t auth_high, uint64_t auth_low,
    RendererPlatformProcess* output, std::string* error)
{
    int to_child[2] = { -1, -1 };
    int from_child[2] = { -1, -1 };
    int control[2] = { -1, -1 };
    if (::pipe(to_child) || ::pipe(from_child)
        || ::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control)) {
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

    std::string shared_name = "/mini-electron-renderer-"
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
    };
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, to_child[0], kRendererReadFd);
    posix_spawn_file_actions_adddup2(&actions, from_child[1], kRendererWriteFd);
    posix_spawn_file_actions_adddup2(&actions, frame_fd, kRendererFrameFd);
    posix_spawn_file_actions_adddup2(&actions, control[1], kRendererControlFd);
    for (int fd : { to_child[0], to_child[1], from_child[0], from_child[1],
             frame_fd, control[0], control[1] }) {
        if (fd != kRendererReadFd && fd != kRendererWriteFd
            && fd != kRendererFrameFd && fd != kRendererControlFd)
            posix_spawn_file_actions_addclose(&actions, fd);
    }
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t pid = 0;
    int spawn_error = ::posix_spawn(&pid, executable.c_str(), &actions,
        &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
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
    return true;
}

bool IsRendererProcessAlive(const RendererPlatformProcess& process)
{
    return process.process_id > 0
        && (::kill(static_cast<pid_t>(process.process_id), 0) == 0 || errno == EPERM);
}

bool WaitForRendererProcess(RendererPlatformProcess* process, int timeout_milliseconds)
{
    if (process->process_id <= 0)
        return true;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(0, timeout_milliseconds));
    do {
        pid_t result = ::waitpid(static_cast<pid_t>(process->process_id), nullptr, WNOHANG);
        if (result == process->process_id || (result < 0 && errno == ECHILD)) {
            process->process_id = 0;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void TerminateRendererProcess(RendererPlatformProcess* process)
{
    if (process->process_id > 0) {
        ::kill(static_cast<pid_t>(process->process_id), SIGKILL);
        ::waitpid(static_cast<pid_t>(process->process_id), nullptr, 0);
        process->process_id = 0;
    }
}

void CloseRendererPlatformProcess(RendererPlatformProcess* process)
{
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
        "(allow file-read* (literal \"/dev/null\") (literal \"/dev/urandom\") "
        "(subpath \"/System/Library\") (subpath \"/Library/Fonts\") (subpath "
        + SandboxQuoted(bundle_directory) + "))\n"
        "(allow mach-lookup (global-name \"com.apple.FontObjectsServer\") "
        "(global-name \"com.apple.fonts\") "
        "(global-name \"com.apple.coreservices.launchservicesd\") "
        "(global-name \"com.apple.windowserver.active\"))\n";
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
