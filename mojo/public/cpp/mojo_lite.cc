
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/base/big_buffer_mojom_traits.h"
#include "mojo/public/cpp/bindings/associated_group.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/async_flusher.h"
#include "mojo/public/cpp/bindings/connector.h"
#include "mojo/public/cpp/bindings/enum_traits.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/lib/message_internal.h"
#include "mojo/public/cpp/bindings/lib/array_internal.h"
#include "mojo/public/cpp/bindings/lib/message_fragment.h"
#include "mojo/public/cpp/bindings/associated_group_controller.h"
#include "mojo/public/cpp/bindings/lib/associated_interface_ptr_state.h"
#include "mojo/public/cpp/bindings/lib/binding_state.h"
#include "mojo/public/cpp/bindings/lib/buffer.h"
#include "mojo/public/cpp/bindings/lib/may_auto_lock.h"
#include "mojo/public/cpp/bindings/lib/interface_ptr_state.h"
#include "mojo/public/cpp/bindings/lib/message_quota_checker.h"
#include "mojo/public/cpp/bindings/lib/pending_receiver_state.h"
#include "mojo/public/cpp/bindings/lib/pending_remote_state.h"
#include "mojo/public/cpp/bindings/lib/unserialized_message_context.h"
#include "mojo/public/cpp/bindings/lib/task_runner_helper.h"
#include "mojo/public/cpp/bindings/message.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/scoped_interface_endpoint_handle.h"
#include "mojo/public/cpp/bindings/sync_handle_watcher.h"
#include "mojo/public/cpp/system/buffer.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/handle.h"
#include "mojo/public/cpp/system/wait.h"
#include "mojo/public/cpp/system/wait_set.h"
#include "mojo/public/cpp/bindings/lib/validation_context.h"
#include "mojo/public/mojom/base/big_buffer.mojom-shared-internal.h"
#include "mojo/public/mojom/base/big_buffer.mojom-shared.h"
#include "skia/public/mojom/image_info.mojom-shared.h"

#include "mojo/public/cpp/bindings/struct_ptr.h"
#include "mojo/public/mojom/base/big_buffer.mojom-shared-internal.h"
#include "mojo/public/mojom/base/string16.mojom-shared-internal.h"
#include "mojo/public/mojom/base/time.mojom-shared-internal.h"
#include "mojo/public/mojom/base/unguessable_token.mojom-shared-internal.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_union_arraybuffer_arraybufferview.h"
#include "third_party/blink/renderer/core/mojo/mojo_watcher.h"
#include "third_party/blink/renderer/core/mojo/mojo_handle.h"
#include "third_party/blink/renderer/core/mojo/test/mojo_interface_request_event.h"
#include "third_party/blink/renderer/core/mojo/test/mojo_interface_interceptor.h"
#include "third_party/blink/renderer/core/messaging/blink_transferable_message.h"
#include "third_party/blink/renderer/core/messaging/blink_transferable_message_mojom_traits.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/public/common/messaging/cloneable_message_mojom_traits.h"
#include "third_party/blink/public/mojom/messaging/transferable_message.mojom-blink.h"
#include "ui/gfx/geometry/mojom/geometry.mojom-shared-internal.h"
#include "url/mojom/origin.mojom-shared-internal.h"
#include "third_party/blink/renderer/platform/wtf/threading_primitives.h"
#if defined(OS_WIN)
#include "base/threading/platform_thread_win.h"
#endif
#include "ipc/ipc_channel_proxy.h"
#include "ipc/ipc_message_macros.h"
#include "mojo/public/cpp/mojo_lite_msg.h"
#include "base/process/process.h"
#include "base/task/single_thread_task_executor.h"
#include "base/run_loop.h"
#include "base/task/bind_post_task.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/data_pipe_getter.mojom.h"
#include "services/network/public/mojom/data_pipe_getter.mojom-blink.h"
#include "services/network/public/mojom/chunked_data_pipe_getter.mojom.h"
#include "services/network/public/mojom/chunked_data_pipe_getter.mojom-blink.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "base/strings/utf_string_conversions.h"
#include "base/hash/hash.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include <unordered_map>
#include <windows.h>
#include <algorithm>
#include <deque>
#include <limits>
#include <atomic>

#ifdef _DEBUG
#include <dbghelp.h>
#include <intrin.h>
#pragma comment(lib, "dbghelp.lib")
#endif // DEBUG

// #if !defined(NDEBUG) && !defined(COMPONENT_BUILD)
// sasdf
// #endif


static int s_MojoHandleEntryCount = 0;
static constexpr uint32_t kCrossProcessMessagePipeHandleKind = 1;
static void onConnectorClose(void* ptr);
static thread_local mojo::Message* s_dispatchingMessage = nullptr;

namespace content {
void printCallstack();
}

#define _DEBUG_RECODE_STACKTRACE_

bool g_enableRecoreCallstack = true;

#ifdef _DEBUG
HANDLE g_printCallstackProcess = NULL;

struct SymThreadInfo {
    DWORD threadId = 0;
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
};
std::vector<SymThreadInfo> g_symThreadInfos;

std::string printStackTrace2()
{
    std::string out;
    DWORD threadId = GetCurrentThreadId();
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    static bool s_init = false;
    if (!s_init)
        SymInitialize(process, NULL, TRUE);
    s_init = true;

    CloseHandle(process);
    CloseHandle(thread);

    return out;
}

std::string printStackTrace()
{
    std::string out;
    DWORD threadId = GetCurrentThreadId();
    bool find = false;
    SymThreadInfo info;
    if (!find) {
        info.process = GetCurrentProcess();
        info.thread = GetCurrentThread();
        info.threadId = threadId;
        //g_symThreadInfos.push_back(info);
    }
    if (!g_printCallstackProcess) {
        g_printCallstackProcess = info.process;
        SymInitialize(info.process, NULL, TRUE);
    }

    HANDLE process = info.process;
    HANDLE thread = info.thread;
#if 1
    CONTEXT context;
    RtlCaptureContext(&context);
    STACKFRAME64 stackFrame;
    ZeroMemory(&stackFrame, sizeof(STACKFRAME64));

    DWORD machineType;

#ifdef _M_IX86
    machineType = IMAGE_FILE_MACHINE_I386;
    stackFrame.AddrPC.Offset = context.Eip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Ebp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Esp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
    machineType = IMAGE_FILE_MACHINE_AMD64;
    stackFrame.AddrPC.Offset = context.Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Rsp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_IA64
    machineType = IMAGE_FILE_MACHINE_IA64;
    stackFrame.AddrPC.Offset = context.StIIP;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.IntSp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrBStore.Offset = context.RsBSP;
    stackFrame.AddrBStore.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.IntSp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#endif

    int count = 0;

    while (StackWalk64(machineType, process, thread, &stackFrame, &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
        count++;
        if (count < 4)
            continue;
        if (count > 12)
            break;
        DWORD64 address = stackFrame.AddrPC.Offset;
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;

        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        std::string name = "no name";
        if (SymFromAddr(process, address, 0, symbol)) {
            name = ((const char*)(symbol->Name));
        }

        char* output = (char*)malloc(name.size() + 2000);
        sprintf(output, "call: [%s], %x\n", name.c_str(), symbol->Address);
        //OutputDebugStringA(output);
        out += output;
        free(output);

        if (address == 0)
            break;
    }
#endif

    CloseHandle(process);
    CloseHandle(thread);

    return out;
}
#endif // DEBUG

struct DebugRecordMojo {
    unsigned char* buf1 = nullptr;
    size_t len1 = 0;
    size_t len2 = 0;
};
DebugRecordMojo* g_testMojo = nullptr;

class MojoHandleMgr;

enum class ConnectorType {
    kNormal = 0,
    kCon0IsRemoteService,
    kCon0IsRemoteClient,
    kCon1IsRemoteService,
    // kCon1IsRemoteClient, // con1不可能是远程客户端模式。因为创建客户端的是本文件内部设置的
};

struct MojoProcessInfo : public IPC::ChannelProxy::MessageFilter {
    uintptr_t pid = 0;
    scoped_refptr<base::SequencedTaskRunner> m_mainThread;
    IPC::ChannelProxy* channelProxy = nullptr;
    // 把别的进程的handle转换成本进程的。父进程 -》本地进程
    std::unordered_map<MojoHandle, MojoHandle> m_crossProcessMojoHandleMap;
    std::unordered_map<MojoHandle, mojo::ScopedHandle> m_exportedHandles;

    MojoHandleMgr* mgr = nullptr;

    // IPC::ChannelProxy::MessageFilter
    /*virtual*/ bool OnMessageReceived(const IPC::Message& message) override;
    /*virtual*/ void OnChannelConnected(int32 peerPid) override;
    /*virtual*/ void OnChannelError() override;
    //--
    void onPostMessage(uintptr_t senderPid, MojoHandle handle, int senderState, const std::vector<char>& data, std::vector<uintptr_t> handles);

    void onPostMessageOnIoThread(uintptr_t senderPid, uintptr_t handle, int senderState, const std::vector<char>& data,
        const MojoLiteMessagePipeHandles& transferred);
};

// 模仿mojo::Connector的
class CrossProcessConnector {
public:
    CrossProcessConnector(base::ProcessId pid)
    {
        char output[100] = { 0 };
        sprintf(output, "CrossProcessConnector: %p\n", this);
        OutputDebugStringA(output);

        m_pid = pid;
    }

    bool postMessageNoLock(
        MojoHandleMgr* mgr, MojoHandle handle, ConnectorType state, std::unique_ptr<std::vector<char>> data, std::vector<uintptr_t> handles);

    base::ProcessId getPid() const { return m_pid; }

private:
    base::ProcessId m_pid; // 要发送的对端的进程pid
};

namespace atom {
void* testChannel();
}

class MojoHandleMgr {
public:
    MojoHandleMgr()
    {
        m_mojoRunner = base::SequencedTaskRunner::GetCurrentDefault();

        m_entryAllocSet.resize(18000);
    }

    enum EntryType {
        kMessage,
        kData,
        kBlinkMessagePort,
        kTrapEventHandler,
    };

    //     class TrapInfo {
    //     public:
    //         EntryType m_type = kTrapEventHandler; // 这个必须放头部
    //         MojoTrapEventHandler m_handler = nullptr;
    //         MojoHandle m_associatedHandle = 0; // 关联的句柄，只允许有一个
    //     };

    struct TrapEventEntry {
        TrapEventEntry(/*TrapInfo* trapInfo, uintptr_t context*/)
        {
            //m_trapInfo = trapInfo;
            //m_context = context;
        }
        //TrapInfo* m_trapInfo = nullptr;
        EntryType m_type = kTrapEventHandler; // 这个必须放头部
        MojoHandle m_trapHandler = 0; // 记录一下本结构对应的句柄值
        MojoTrapEventHandler m_callback = nullptr;
        MojoHandle m_associatedHandle = 0; // 关联的句柄，只允许有一个
        uintptr_t m_context = 0;
        MojoHandleSignals m_signals = MOJO_HANDLE_SIGNAL_READABLE;
        bool m_armed = false;
        MojoTriggerCondition m_condition = MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED;
        std::vector<std::unique_ptr<TrapEventEntry>> m_triggers;
    };

    // Scoped C++ Message handoff storage, not a raw message-pipe backend.
    struct RawMessage {
        mojo::internal::Buffer payload;
        std::vector<mojo::ScopedHandle> handles;
        mojo::ReportBadMessageCallback bad_message_callback;
    };

    struct TypedCall {
        base::OnceCallback<void(void*)> callback;
        uint64_t serial;
        bool is_message;
    };

    struct TypedWaiter {
        base::RepeatingClosure quit;
        std::atomic<bool> dispatched { false };
        uint64_t barrier_serial = 0; // Zero waits for an actual interface call.
    };

    struct MojoHandleEntry {
        MojoHandleEntry(MojoHandle handle0, MojoHandle handle1, EntryType type)
        {
            m_handle0 = handle0;
            m_handle1 = handle1;
            m_type = type;
            s_MojoHandleEntryCount++;
        }

        ~MojoHandleEntry()
        {
            s_MojoHandleEntryCount--;
            //char output[50] = { 0 };
            //sprintf(output, "MojoHandleEntry count: %d\n", s_MojoHandleEntryCount);
            //OutputDebugStringA(output);
        }
        EntryType m_type; // 这个必须放头部

        // ----给Remote\Receiver用的
        void* m_interfacePtr = nullptr;
        uint32_t m_disconnectReason = 0;
        std::string m_disconnectDescription;
        //mojo::internal::ReceiverImplBase* m_receiverImplBase = nullptr; // 发送端析构的话，通知接收端
        //mojo::internal::RemoteImplBase* m_remoteImplBase = nullptr; // 接收端析构的话，通知发送端
        int64_t m_receiverImplBaseId = 0;
        int64_t m_remoteImplBaseId = 0;
        scoped_refptr<base::SequencedTaskRunner> m_receiverRunner; // 接收端的runner。表示接收端必须在这个runner线程上跑
        bool m_isCloseing = false;
        bool m_enableUnassociatedUsage = false;
        bool m_handle0IsRemote = false;
        bool m_isPostingArmTrapEventDelay = false;
        // =====

        //---给Connector用
        ConnectorType m_connectorType = ConnectorType::kNormal;
        bool isCon0RemoteClientPending = false; // kCon0IsRemoteClient的时候，这个值才有用
        //std::set<size_t> m_connector0Set; // 记录的是Connector的id，
        //std::set<size_t> m_connector1Set;
        void* m_connector0 = nullptr; // mojo::Connector的指针。正确性由查询map来保障
        void* m_connector1 = nullptr;
        bool isConnector0(void* connector)
        {
            //bool b = m_connector0Set.find(id) != m_connector0Set.end();
            //return b;
            return (connector == m_connector0);
        }
        //====

        //--TrapEventHandler
        std::vector<TrapEventEntry*> m_trapEventEntrys;

        bool m_readTrapDirty = false; // 读取操作，决定是否需要trap。防止多次写入，然后触发多次的read trap
        bool m_closeTrapDirty = false; // 关闭操作，只有发送了close trap才允许BeginReadData返回MOJO_RESULT_FAILED_PRECONDITION。不然
        // ResourceScriptStreamer::OnDataPipeReadable会断言错误
        //====

        //---给data pipe用的
        std::vector<char> m_dataPipe;
        bool m_isBeginReadingData = false;
        size_t m_readPos = 0;
        size_t m_dataSize = 0;
        uint32_t m_elementSize = 1;
        uint32_t m_readAvailable = 0;
        uint32_t m_writeAvailable = 0;
        bool m_isBeginWritingData = false;
        enum DataTrapState {
            kNotDataNotTrap,
            kHasDataNotTrap,
            kHadTraped,
        };
        DataTrapState m_dataTrapState = kNotDataNotTrap;
        bool m_isDelayClose = false; // 仅给mbnet::asynMojoClose用
        //====

        uint32_t m_allocIndex = -1;

        uint32_t capacityNumBytes = 0;

        intptr_t m_handle0Ref = 1;
        bool m_handle0HadClosed = false;
        MojoHandle m_handle0 = MOJO_HANDLE_INVALID;

        intptr_t m_handle1Ref = 1;
        bool m_handle1HadClosed = false;
        MojoHandle m_handle1 = MOJO_HANDLE_INVALID;

        bool m_isMessageChannel = false;
        std::deque<TypedCall> m_typedCalls;
        std::vector<std::shared_ptr<TypedWaiter>> m_typedWaiters;
        uint64_t m_typedSerial = 0;
        uint64_t m_receiverGeneration = 0;
        bool m_typedPaused = false;
        bool m_typedScheduled = false;
        unsigned m_typedDispatchDepth = 0;
        MojoHandle m_typedDeferredClose = MOJO_HANDLE_INVALID;
    };

    static MojoHandle receiverHandle(const MojoHandleEntry* entry)
    {
        return entry->m_handle0IsRemote ? entry->m_handle1 : entry->m_handle0;
    }

    static bool receiverIsOpen(const MojoHandleEntry* entry)
    {
        return !(entry->m_handle0IsRemote ? entry->m_handle1HadClosed : entry->m_handle0HadClosed);
    }

    static void wakeTypedWaiters(MojoHandleEntry* entry)
    {
        auto waiters = std::move(entry->m_typedWaiters);
        entry->m_typedWaiters.clear();
        for (const auto& waiter : waiters)
            waiter->quit.Run();
    }

    void scheduleTypedDispatch(MojoHandleEntry* entry)
    {
        if (entry->m_typedScheduled || entry->m_typedPaused || entry->m_typedCalls.empty() ||
            !entry->m_receiverImplBaseId || !entry->m_receiverRunner || !receiverIsOpen(entry))
            return;
        entry->m_typedScheduled = true;
        const MojoHandle handle = receiverHandle(entry);
        const int64_t id = entry->m_receiverImplBaseId;
        const uint64_t generation = entry->m_receiverGeneration;
        if (!entry->m_receiverRunner->PostTask(FROM_HERE, base::BindOnce(
                [](MojoHandle handle, int64_t id, uint64_t generation) {
                    MojoHandleMgr::GetInst()->dispatchTypedCall(handle, id, generation);
                }, handle, id, generation))) {
            entry->m_typedScheduled = false;
            wakeTypedWaiters(entry);
        }
    }

    void dispatchTypedCall(MojoHandle handle, int64_t id, uint64_t generation)
    {
        m_lock.lock();
        auto* entry = findEntryNotLock(handle);
        if (!entry || entry->m_receiverImplBaseId != id || entry->m_receiverGeneration != generation ||
            !receiverIsOpen(entry)) {
            m_lock.unlock();
            return;
        }
        DCHECK(entry->m_receiverRunner->RunsTasksInCurrentSequence());
        entry->m_typedScheduled = false;
        if (entry->m_typedPaused || entry->m_typedCalls.empty()) {
            m_lock.unlock();
            return;
        }
        auto call = std::move(entry->m_typedCalls.front());
        entry->m_typedCalls.pop_front();
        std::vector<std::shared_ptr<TypedWaiter>> completed;
        for (auto it = entry->m_typedWaiters.begin(); it != entry->m_typedWaiters.end();) {
            const auto& waiter = *it;
            if (waiter->barrier_serial ? call.serial >= waiter->barrier_serial : call.is_message) {
                completed.push_back(std::move(*it));
                it = entry->m_typedWaiters.erase(it);
            } else {
                ++it;
            }
        }
        void* implementation = entry->m_interfacePtr;
        CHECK(implementation && implementation != reinterpret_cast<void*>(-1));
        ++entry->m_typedDispatchDepth;
        m_lock.unlock();
        if (call.callback)
            std::move(call.callback).Run(implementation);
        for (const auto& waiter : completed) {
            waiter->dispatched.store(true);
            waiter->quit.Run();
        }
        m_lock.lock();
        entry = findEntryNotLock(handle);
        if (entry && entry->m_receiverImplBaseId == id && entry->m_receiverGeneration == generation && receiverIsOpen(entry)) {
            --entry->m_typedDispatchDepth;
            scheduleTypedDispatch(entry);
            if (entry->m_typedCalls.empty() && !entry->m_typedDispatchDepth && entry->m_typedDeferredClose) {
                const MojoHandle closed = std::exchange(entry->m_typedDeferredClose, MOJO_HANDLE_INVALID);
                handleMessageClose(closed, entry);
            }
        }
        m_lock.unlock();
    }

    void enqueueTypedCall(MojoHandle sender, base::OnceCallback<void(void*)> callback, bool is_message)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = findEntryNotLock(sender);
        if (!entry || entry->m_type != kMessage || sender == receiverHandle(entry) ||
            entry->m_handle0HadClosed || entry->m_handle1HadClosed)
            return;
        entry->m_typedCalls.push_back({ std::move(callback), ++entry->m_typedSerial, is_message });
        scheduleTypedDispatch(entry);
    }

    void pauseTypedReceiver(MojoHandle handle, bool paused)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = findEntryNotLock(handle);
        if (!entry || receiverHandle(entry) != handle || !receiverIsOpen(entry))
            return;
        entry->m_typedPaused = paused;
        if (!paused)
            scheduleTypedDispatch(entry); // Resume never dispatches inline.
    }

    bool waitForTypedCall(MojoHandle handle, bool flush)
    {
        base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
        auto waiter = std::make_shared<TypedWaiter>();
        waiter->quit = base::BindPostTask(base::SequencedTaskRunner::GetCurrentDefault(), loop.QuitClosure());
        {
            WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
            auto* entry = findEntryNotLock(handle);
            if (!entry || receiverHandle(entry) != handle || !receiverIsOpen(entry) ||
                (!entry->m_receiverImplBaseId && !flush) || (entry->m_typedPaused && !flush))
                return false;
            const bool peer_closed = entry->m_handle0IsRemote ? entry->m_handle0HadClosed : entry->m_handle1HadClosed;
            if (peer_closed && entry->m_typedCalls.empty())
                return false;
            if (flush) {
                waiter->barrier_serial = ++entry->m_typedSerial;
                entry->m_typedCalls.push_back({ {}, waiter->barrier_serial, false });
            }
            entry->m_typedWaiters.push_back(waiter);
            scheduleTypedDispatch(entry);
        }
        loop.Run();
        return waiter->dispatched.load();
    }

    RawMessage* findMessage(MojoMessageHandle handle)
    {
        auto it = m_messages.find(handle);
        return it == m_messages.end() ? nullptr : it->second.get();
    }

    std::unique_ptr<RawMessage> takeMessage(MojoMessageHandle handle)
    {
        auto it = m_messages.find(handle);
        if (it == m_messages.end())
            return nullptr;
        auto message = std::move(it->second);
        m_messages.erase(it);
        return message;
    }

    MojoMessageHandle addMessage(std::unique_ptr<RawMessage> message)
    {
        MojoMessageHandle handle = ++m_messageSeed;
        CHECK(handle);
        m_messages.emplace(handle, std::move(message));
        return handle;
    }

    void restoreMessage(MojoMessageHandle handle, std::unique_ptr<RawMessage> message)
    {
        CHECK(m_messages.emplace(handle, std::move(message)).second);
    }

    // Only unbound pipe endpoints have transferable state in the lite backend.
    // There is no OS-handle/shared-memory dispatcher behind these numeric IDs.
    MojoResult canTransferHandle(MojoHandle handle)
    {
        MojoHandleSignalsState state;
        if (querySignals(handle, &state) != MOJO_RESULT_OK)
            return MOJO_RESULT_INVALID_ARGUMENT;
        auto* entry = findEntryNotLock(handle);
        if (entry->m_connectorType != ConnectorType::kNormal)
            return MOJO_RESULT_UNIMPLEMENTED;
        const bool first = handle == entry->m_handle0;
        const bool remote = first == entry->m_handle0IsRemote;
        const bool watched = std::any_of(entry->m_trapEventEntrys.begin(), entry->m_trapEventEntrys.end(),
            [handle](const TrapEventEntry* trigger) { return trigger->m_associatedHandle == handle; });
        if ((first ? entry->m_connector0 : entry->m_connector1) ||
            (remote ? entry->m_remoteImplBaseId : entry->m_receiverImplBaseId) || watched ||
            entry->m_isBeginReadingData || entry->m_isBeginWritingData)
            return MOJO_RESULT_BUSY;
        return MOJO_RESULT_OK;
    }


    static MojoHandleMgr* GetInst()
    {
        static MojoHandleMgr* const instance = new MojoHandleMgr();
        return instance;
    }

    void lock()
    {
        m_lock.lock();
    }

    void unlock()
    {
        m_lock.unlock();
    }

    bool isHandle0(MojoHandle handle)
    {
        return ((handle % 2) == 0);
    }

    void makeIsMessageChannelFlag(MojoHandle handle)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        MojoHandleEntry* entry = findEntryNotLock(handle);
        CHECK(entry);
        if (entry->m_isMessageChannel)
            return;
        entry->m_isMessageChannel = true;

        m_totalMsgConnelEntryCount++;
    }

    void makeDelayCloseFlag(MojoHandle handle)
    {
        closeHandleImpl(handle, true);
    }

    void closeHandle(MojoHandle handle)
    {
        if (!closeTrap(handle))
            closeHandleImpl(handle, false);
    }

    void closeHandleImpl(MojoHandle handle, bool needMakeDelayClose)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        CHECK(m_reentryCount == 0);
        m_reentryCount++;

        MojoHandleEntry* entry = findEntryNotLock(handle);
        if (!entry) {
            m_reentryCount--;
            return;
        }
        if (handle == entry->m_handle0 ? entry->m_handle0HadClosed : entry->m_handle1HadClosed) {
            m_reentryCount--;
            return;
        }
        CHECK(entry->m_type == kMessage || entry->m_type == kData || entry->m_type == kBlinkMessagePort);

        if (needMakeDelayClose) {
            CHECK(entry->m_type == kData);
            entry->m_isDelayClose = true;
        }

        if (entry->m_handle0 == handle) {
            entry->m_handle0Ref--;
            if (0 == entry->m_handle0Ref)
                entry->m_handle0HadClosed = true;
        } else if (entry->m_handle1 == handle) {
            entry->m_handle1Ref--;
            if (0 == entry->m_handle1Ref)
                entry->m_handle1HadClosed = true;
        }
        m_reentryCount--;
        std::deque<TypedCall> discarded_typed_calls;
        if (handle == receiverHandle(entry)) {
            discarded_typed_calls = std::move(entry->m_typedCalls);
            entry->m_typedCalls.clear();
            ++entry->m_receiverGeneration;
            entry->m_typedScheduled = false;
            entry->m_typedPaused = false;
            wakeTypedWaiters(entry);
        } else if (entry->m_typedCalls.empty() && !entry->m_typedDispatchDepth) {
            wakeTypedWaiters(entry);
        }

        for (;;) {
            auto it = std::find_if(entry->m_trapEventEntrys.begin(), entry->m_trapEventEntrys.end(),
                [handle](const TrapEventEntry* trigger) { return trigger->m_associatedHandle == handle; });
            if (it == entry->m_trapEventEntrys.end())
                break;
            removeTrigger((*it)->m_trapHandler, (*it)->m_context);
            entry = findEntryNotLock(handle);
            if (!entry)
                return;
        }

        handleMessageClose(handle, entry);

        entry = findEntryNotLock(handle); // 上面可能会导致entry已被删除，所以要重新获取
        if (!entry)
            return;
        if (0 == entry->m_handle0Ref && 0 == entry->m_handle1Ref) {
            std::unordered_map<MojoHandle, MojoHandleEntry*>::iterator it = m_handle0ToEntrys.find(entry->m_handle0);
            CHECK(it != m_handle0ToEntrys.end());
            m_handle0ToEntrys.erase(it);

            it = m_handle1ToEntrys.find(entry->m_handle1);
            CHECK(it != m_handle1ToEntrys.end());
            m_handle1ToEntrys.erase(it);

            deleteMojoHandleEntry(entry);

            return;
        }

        armTrapEventDelayNotLock(FROM_HERE, entry, MOJO_HANDLE_SIGNAL_PEER_CLOSED);
    }

    void handleBlinkMessagePortClose(MojoHandle handle, MojoHandleEntry* entry)
    {
        if (0 != entry->m_handle0Ref || 0 != entry->m_handle1Ref)
            return;

        void* connector = !isHandle0(handle) ? entry->m_connector0 : entry->m_connector1;

        if (connector && findConnectorNotLock(connector)) {
            mojo::Connector* otherConnector = (mojo::Connector*)connector;

            base::SequencedTaskRunner* task_runner = otherConnector->task_runner();
            if (!task_runner)
                task_runner = base::SequencedTaskRunner::GetCurrentDefault().get();
            task_runner->PostTask(FROM_HERE, base::BindOnce(&onConnectorClose, otherConnector));
        }
    }

    // pending remote关闭的时候，如果还没实例化成remote，就要通知接收端。反之亦然
    void handleMessageClose(MojoHandle origHandle, MojoHandleEntry* entry)
    {
        if ((entry->m_type != kMessage && entry->m_type != kBlinkMessagePort) || (0 != entry->m_handle0Ref && 0 != entry->m_handle1Ref))
            return;
        if (origHandle != receiverHandle(entry) &&
            (!entry->m_typedCalls.empty() || entry->m_typedDispatchDepth)) {
            entry->m_typedDeferredClose = origHandle;
            scheduleTypedDispatch(entry);
            return;
        }
        wakeTypedWaiters(entry);

        if (entry->m_type == kBlinkMessagePort) {
            handleBlinkMessagePortClose(origHandle, entry);
        }

        int64_t id = 0;
        MojoHandle handle = 0;
        if (0 == entry->m_handle0Ref && 0 != entry->m_handle1Ref) {
            handle = entry->m_handle1;
            if (entry->m_handle0IsRemote && entry->m_receiverImplBaseId != 0) { // 0 是发送器，且被关闭了，则通知接收器
                id = entry->m_receiverImplBaseId;
                entry->m_receiverImplBaseId = 0;
                entry->m_remoteImplBaseId = 0;
                m_lock.unlock();
                mojo::internal::ReceiverImplBase::OnRemoteImplBaseClose(id, handle, base::OnceClosure());
                m_lock.lock();
            } else if (!entry->m_handle0IsRemote && entry->m_remoteImplBaseId != 0) { // 0 是接收器，且被关闭了，则通知发送器
                id = entry->m_remoteImplBaseId;
                entry->m_receiverImplBaseId = 0;
                entry->m_remoteImplBaseId = 0;
                entry->m_isCloseing = true;
                entry->m_interfacePtr = (void*)(-1);
                m_lock.unlock();
                mojo::internal::RemoteImplBase::OnReceiverImplBaseClose(id, handle, base::OnceClosure());
                m_lock.lock();
            }
        }
        if (0 == entry->m_handle1Ref && 0 != entry->m_handle0Ref) {
            handle = entry->m_handle0;
            if (!entry->m_handle0IsRemote && entry->m_receiverImplBaseId != 0) { // 1 是发送器，且被关闭了，则通知接收器
                id = entry->m_receiverImplBaseId;
                entry->m_receiverImplBaseId = 0;
                entry->m_remoteImplBaseId = 0;
                m_lock.unlock();
                mojo::internal::ReceiverImplBase::OnRemoteImplBaseClose(id, handle, base::OnceClosure());
                m_lock.lock();
            } else if (entry->m_handle0IsRemote && entry->m_remoteImplBaseId != 0) { // 1 是接收器，且被关闭了，则通知发送器
                id = entry->m_remoteImplBaseId;
                entry->m_receiverImplBaseId = 0;
                entry->m_remoteImplBaseId = 0;
                entry->m_isCloseing = true;
                entry->m_interfacePtr = (void*)(-1);
                m_lock.unlock();
                mojo::internal::RemoteImplBase::OnReceiverImplBaseClose(id, handle, base::OnceClosure());
                m_lock.lock();
            }
        }
    }

    TrapEventEntry* findTrapEventEntryNotLock(MojoHandle trapHandler)
    {
        std::unordered_map<MojoHandle, TrapEventEntry*>::const_iterator it;
        it = m_trapEventSet.find(trapHandler);
        if (m_trapEventSet.end() == it)
            return nullptr;
        return it->second;
    }

    MojoHandle createTrap(MojoTrapEventHandler callback)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        CHECK_LE(m_seedHandle, std::numeric_limits<MojoHandle>::max() - 1u);
        TrapEventEntry* info = new TrapEventEntry();
        info->m_callback = callback;
        info->m_trapHandler = m_seedHandle;
        m_trapEventSet.insert(std::pair<MojoHandle, TrapEventEntry*>(info->m_trapHandler, info));
        m_seedHandle += 2; // 必须前进2位，不然m_handle0ToEntrys这些就对应不上了
        return info->m_trapHandler;
    }

    MojoResult addTrigger(MojoHandle handle, MojoHandle trapHandle, uintptr_t context, MojoHandleSignals signals, MojoTriggerCondition condition)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = findEntryNotLock(handle);
        auto* trap = findTrapEventEntryNotLock(trapHandle);
        if (!entry || !trap || !signals || (signals & ~(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED)) ||
            (condition != MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED && condition != MOJO_TRIGGER_CONDITION_SIGNALS_UNSATISFIED) ||
            (handle == entry->m_handle0 ? entry->m_handle0HadClosed : entry->m_handle1HadClosed))
            return MOJO_RESULT_INVALID_ARGUMENT;
        for (const auto& trigger : trap->m_triggers) {
            if (trigger->m_context == context || trigger->m_associatedHandle == handle)
                return MOJO_RESULT_ALREADY_EXISTS;
        }
        auto trigger = std::make_unique<TrapEventEntry>();
        trigger->m_trapHandler = trapHandle;
        trigger->m_associatedHandle = handle;
        trigger->m_context = context;
        trigger->m_signals = signals;
        trigger->m_condition = condition;
        entry->m_trapEventEntrys.push_back(trigger.get());
        trap->m_triggers.push_back(std::move(trigger));
        armTrapEventDelayNotLock(FROM_HERE, entry, signals);
        return MOJO_RESULT_OK;
    }

    MojoResult removeTrigger(MojoHandle trapHandle, uintptr_t context)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* trap = findTrapEventEntryNotLock(trapHandle);
        if (!trap)
            return MOJO_RESULT_INVALID_ARGUMENT;
        for (auto it = trap->m_triggers.begin(); it != trap->m_triggers.end(); ++it) {
            if ((*it)->m_context != context)
                continue;
            auto trigger = std::move(*it);
            trap->m_triggers.erase(it);
            auto* entry = findEntryNotLock(trigger->m_associatedHandle);
            if (entry) {
                auto& triggers = entry->m_trapEventEntrys;
                triggers.erase(std::remove(triggers.begin(), triggers.end(), trigger.get()), triggers.end());
            }
            MojoTrapEvent event = { sizeof(MojoTrapEvent) };
            event.flags = MOJO_TRAP_EVENT_FLAG_WITHIN_API_CALL;
            event.trigger_context = context;
            event.result = MOJO_RESULT_CANCELLED;
            trap->m_callback(&event);
            return MOJO_RESULT_OK;
        }
        return MOJO_RESULT_NOT_FOUND;
    }

    bool closeTrap(MojoHandle handle)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* trap = findTrapEventEntryNotLock(handle);
        if (!trap)
            return false;
        while (trap && !trap->m_triggers.empty()) {
            removeTrigger(handle, trap->m_triggers.front()->m_context);
            trap = findTrapEventEntryNotLock(handle);
        }
        if (trap) {
            m_trapEventSet.erase(handle);
            delete trap;
        }
        return true;
    }

    static MojoHandle findOtherHandle(MojoHandle handle, MojoHandleMgr::MojoHandleEntry* entry)
    {
        if (entry->m_handle0 == handle)
            return entry->m_handle1;
        if (entry->m_handle1 == handle)
            return entry->m_handle0;
        DebugBreak();
        return 0;
    }

    MojoHandleEntry* findEntryNotLock(MojoHandle handle)
    {
        std::unordered_map<MojoHandle, MojoHandleEntry*>::const_iterator it;
        if ((handle % 2) == 0) {
            it = m_handle0ToEntrys.find(handle);
            if (it == m_handle0ToEntrys.end())
                return nullptr;
        } else {
            it = m_handle1ToEntrys.find(handle);
            if (it == m_handle1ToEntrys.end())
                return nullptr;
        }
        return it->second;
    }

#ifdef _DEBUG
    struct HandleEntryTrace {
        std::string callstack;
        int count = 0;
    };

    void setDebugMojoHandleStr(MojoHandle handle, const std::string& str)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        MojoHandleEntryDummy* entry = (MojoHandleEntryDummy*)findEntryNotLock(handle);
        entry->callstack = str;
    }

    void printStackTraceHash()
    {
        std::map<uint32_t, HandleEntryTrace*> entrys;
        std::vector<HandleEntryTrace*> result;

        size_t allSize = m_entryAllocSet.size();
        for (size_t i = m_entryAllocSetIndex; i < allSize; ++i) {
            MojoHandleEntryDummy* dummy = (MojoHandleEntryDummy*)(&(m_entryAllocSet[i]));
            if (!dummy->isSet)
                continue;
            uint32_t callstackHash = base::Hash(dummy->callstack);
            std::map<uint32_t, HandleEntryTrace*>::iterator it = entrys.find(callstackHash);
            if (it == entrys.end()) {
                HandleEntryTrace* trace = new HandleEntryTrace();
                trace->callstack = dummy->callstack;
                trace->count++;
                entrys[callstackHash] = trace;
            } else {
                it->second->count++;
            }
        }
        for (std::map<uint32_t, HandleEntryTrace*>::iterator it = entrys.begin(); it != entrys.end(); ++it) {
            if (result.empty()) {
                result.push_back(it->second);
                continue;
            }
            for (size_t i = 0; i < result.size(); ++i) {
                if (it->second->count < result[i]->count) {
                    result.insert(result.begin() + i, it->second);
                    break;
                }
            }
        }
        for (std::map<uint32_t, HandleEntryTrace*>::iterator it = entrys.begin(); it != entrys.end(); ++it) {
            HandleEntryTrace* second = it->second;
            delete second;
        }
    }
#endif // _DEBUG

    MojoHandleEntry* createMojoHandleEntry(MojoHandle handle0, MojoHandle handle1, EntryType type)
    {
        size_t allSize = m_entryAllocSet.size();
        size_t index = 0;

        for (size_t i = m_entryAllocSetIndex; index < allSize; ++i, i = (i % allSize), ++index) {
            if (0 == m_entryAllocSet[i].isSet) {
                MojoHandleEntryDummy* dummy = (MojoHandleEntryDummy*)(&(m_entryAllocSet[i]));
                dummy->type = type;
#if 0 // def _DEBUG_RECODE_STACKTRACE_
                if (m_totalEntryCount > 200 && ::GetTickCount() % 5 == 1)
                    dummy->callstack = printStackTrace();
#endif
                dummy->isSet = 1;
                MojoHandleEntry* ret = new (dummy) MojoHandleEntry(handle0, handle1, type);
                ret->m_allocIndex = i;
                m_totalEntryCount++;
#if 0 // def _DEBUG_RECODE_STACKTRACE_
                if (m_totalEntryCount == 1700)
                    printStackTraceHash();
#endif
                base::Time now = base::Time::Now();
                double nowT = now.InSecondsFSinceUnixEpoch();
                double gcTime = m_gcTime.InSecondsFSinceUnixEpoch();

                if (m_totalEntryCount > 6000 && (nowT - gcTime > 30)) {
                    m_gcTime = now;
                    content::RenderThreadImpl::get()->garbageCollectionDelay(500);
                }

                ret->m_receiverRunner = mojo::internal::GetTaskRunnerToUseFromUserProvidedTaskRunner(nullptr);
                return ret;
            }
        }
        CHECK(false);
        return nullptr;
    }

    void deleteMojoHandleEntry(MojoHandleEntry* entry)
    {
        if (entry->m_isMessageChannel) {
            m_totalMsgConnelEntryCount--;
        }
        m_totalEntryCount--;

        MojoHandleEntryDummy* dummy = (MojoHandleEntryDummy*)entry;
        CHECK(dummy->isSet == 1);
        dummy->isSet = 0;
#ifdef _DEBUG
        dummy->fromAddrDebug = nullptr;
        if (std::string::npos != dummy->callstack.find("StorageAreaObserver>::PendingReceiver")) {
            OutputDebugStringA("deleteMojoHandleEntry, StorageAreaObserver\n");
        }
        dummy->callstack = "";
#endif
        entry->MojoHandleEntry::~MojoHandleEntry();
    }

    MojoHandleEntry* createMessagePipe(MojoHandle* handle0, MojoHandle* handle1)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        CHECK_LE(m_seedHandle, std::numeric_limits<MojoHandle>::max() - 1u);
        *handle0 = (MojoHandle)m_seedHandle;
        m_seedHandle++;
        *handle1 = (MojoHandle)m_seedHandle;
        m_seedHandle++;

        MojoHandleEntry* entry = createMojoHandleEntry(*handle0, *handle1, EntryType::kMessage);
        m_handle0ToEntrys.insert(std::pair<MojoHandle, MojoHandleEntry*>(*handle0, entry));
        m_handle1ToEntrys.insert(std::pair<MojoHandle, MojoHandleEntry*>(*handle1, entry));
        return entry;
    }

    void recordCall(MojoHandle handle, const std::string& callstack)
    {
#ifdef _DEBUG
        if (!g_enableRecoreCallstack)
            return;

        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        MojoHandleMgr::MojoHandleEntry* entry = findEntryNotLock(handle);

        if (entry) {
            MojoHandleEntryDummy* dummy = (MojoHandleEntryDummy*)entry;
            //dummy->callstack = callstack;

            if (std::string::npos != dummy->callstack.find("StorageAreaObserver>::PendingReceiver")) {
                OutputDebugStringA("recordCall, StorageAreaObserver\n");
            }
        }
#endif // _DEBUG
    }

    void setMessagePipeIsRemoteOrReceiver(MojoHandle handle, bool isRemote)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        MojoHandleMgr::MojoHandleEntry* entry = findEntryNotLock(handle);
        CHECK(entry);

        if (entry->m_handle0 == handle) {
            entry->m_handle0IsRemote = isRemote;
        } else if (entry->m_handle1 == handle) {
            entry->m_handle0IsRemote = !isRemote;
        } else
            CHECK(false);
    }

    MojoResult createDataPipe(const MojoCreateDataPipeOptions* options, MojoHandle* producer, MojoHandle* consumer)
    {
        if (!producer || !consumer || producer == consumer || (options && (options->struct_size < sizeof(*options) || options->flags != 0)))
            return MOJO_RESULT_INVALID_ARGUMENT;
        uint32_t element_size = options ? options->element_num_bytes : 1;
        if (!element_size)
            return MOJO_RESULT_INVALID_ARGUMENT;
        uint32_t capacity = options ? options->capacity_num_bytes : 0;
        if (!capacity)
            capacity = std::max<uint32_t>(1, 65536 / element_size) * element_size;
        if (capacity % element_size)
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        CHECK_LE(m_seedHandle, std::numeric_limits<MojoHandle>::max() - 1u);
        MojoHandle handle0 = m_seedHandle++;
        MojoHandle handle1 = m_seedHandle++;
        auto* entry = createMojoHandleEntry(handle0, handle1, EntryType::kData);
        entry->capacityNumBytes = capacity;
        entry->m_elementSize = element_size;
        entry->m_dataPipe.resize(capacity);
        m_handle0ToEntrys.emplace(handle0, entry);
        m_handle1ToEntrys.emplace(handle1, entry);
        *producer = handle0;
        *consumer = handle1;
        return MOJO_RESULT_OK;
    }

    MojoResult querySignals(MojoHandle handle, MojoHandleSignalsState* state)
    {
        if (!state)
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = findEntryNotLock(handle);
        if (!entry)
            return MOJO_RESULT_INVALID_ARGUMENT;
        bool first = handle == entry->m_handle0;
        if (first ? entry->m_handle0HadClosed : entry->m_handle1HadClosed)
            return MOJO_RESULT_INVALID_ARGUMENT;
        bool peer_closed = first ? entry->m_handle1HadClosed : entry->m_handle0HadClosed;
        state->satisfied_signals = peer_closed ? MOJO_HANDLE_SIGNAL_PEER_CLOSED : 0;
        state->satisfiable_signals = MOJO_HANDLE_SIGNAL_PEER_CLOSED;
        if (entry->m_type == kData) {
            if (first) {
                if (!peer_closed) {
                    state->satisfiable_signals |= MOJO_HANDLE_SIGNAL_WRITABLE;
                    if (entry->m_dataSize < entry->capacityNumBytes && !entry->m_isBeginWritingData)
                        state->satisfied_signals |= MOJO_HANDLE_SIGNAL_WRITABLE;
                }
            } else {
                if (entry->m_dataSize || !peer_closed)
                    state->satisfiable_signals |= MOJO_HANDLE_SIGNAL_READABLE;
                if (entry->m_dataSize && !entry->m_isBeginReadingData)
                    state->satisfied_signals |= MOJO_HANDLE_SIGNAL_READABLE;
            }
        } else {
            if (!peer_closed)
                state->satisfiable_signals |= MOJO_HANDLE_SIGNAL_READABLE;
            if (!peer_closed) {
                state->satisfiable_signals |= MOJO_HANDLE_SIGNAL_WRITABLE;
                state->satisfied_signals |= MOJO_HANDLE_SIGNAL_WRITABLE;
            }
        }
        return MOJO_RESULT_OK;
    }

    bool triggerEvent(const TrapEventEntry& trigger, MojoTrapEvent* event)
    {
        *event = { sizeof(MojoTrapEvent) };
        event->trigger_context = trigger.m_context;
        if (querySignals(trigger.m_associatedHandle, &event->signals_state) != MOJO_RESULT_OK)
            return false;
        if (trigger.m_condition == MOJO_TRIGGER_CONDITION_SIGNALS_UNSATISFIED) {
            if ((event->signals_state.satisfied_signals & trigger.m_signals) != trigger.m_signals) {
                event->result = MOJO_RESULT_OK;
                return true;
            }
        } else if (event->signals_state.satisfied_signals & trigger.m_signals) {
            event->result = MOJO_RESULT_OK;
            return true;
        } else if (!(event->signals_state.satisfiable_signals & trigger.m_signals)) {
            event->result = MOJO_RESULT_FAILED_PRECONDITION;
            return true;
        }
        return false;
    }

    MojoResult armTrap(MojoHandle handle, uint32_t* count, MojoTrapEvent* events)
    {
        if (count && !events)
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* trap = findTrapEventEntryNotLock(handle);
        if (!trap)
            return MOJO_RESULT_INVALID_ARGUMENT;
        if (trap->m_triggers.empty())
            return MOJO_RESULT_NOT_FOUND;
        uint32_t written = 0;
        bool blocked = false;
        for (const auto& trigger : trap->m_triggers) {
            MojoTrapEvent event;
            if (!triggerEvent(*trigger, &event))
                continue;
            blocked = true;
            if (count && written < *count)
                events[written++] = event;
        }
        trap->m_armed = !blocked;
        if (blocked && count)
            *count = written;
        return blocked ? MOJO_RESULT_FAILED_PRECONDITION : MOJO_RESULT_OK;
    }

    scoped_refptr<base::SequencedTaskRunner> getMojoHandleRunnerTask(MojoHandle handle)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        MojoHandleMgr::MojoHandleEntry* entry = findEntryNotLock(handle);
        if (!entry)
            return m_mojoRunner;
        return entry->m_receiverRunner;
    }


    void armTrapEventDelayNotLock(const base::Location&, MojoHandleEntry* entry, MojoHandleSignals)
    {
        struct PendingEvent {
            MojoHandle trap;
            MojoHandle watched;
            MojoTrapEvent event;
        };
        std::vector<PendingEvent> pending;
        for (const auto* trigger : entry->m_trapEventEntrys) {
            auto* trap = findTrapEventEntryNotLock(trigger->m_trapHandler);
            MojoTrapEvent event;
            if (!trap || !trap->m_armed || !triggerEvent(*trigger, &event))
                continue;
            trap->m_armed = false;
            event.flags |= MOJO_TRAP_EVENT_FLAG_WITHIN_API_CALL;
            pending.push_back({ trigger->m_trapHandler, trigger->m_associatedHandle, event });
        }
        // Core trap handlers are not sequence-affine. In particular Wait()
        // must be signalled even when its owning sequence is blocked.
        // SimpleWatcher/Blink watchers perform their own sequence hop.
        for (const auto& ready : pending) {
            auto* trap = findTrapEventEntryNotLock(ready.trap);
            if (!trap)
                continue;
            const bool registered = std::any_of(trap->m_triggers.begin(), trap->m_triggers.end(),
                [&ready](const auto& trigger) {
                    return trigger->m_context == ready.event.trigger_context &&
                        trigger->m_associatedHandle == ready.watched;
                });
            if (registered)
                trap->m_callback(&ready.event);
        }
    }

    MojoHandleEntry* dataEntry(MojoHandle handle, bool producer)
    {
        auto* entry = findEntryNotLock(handle);
        if (!entry || entry->m_type != kData ||
            handle != (producer ? entry->m_handle0 : entry->m_handle1) ||
            (producer ? entry->m_handle0HadClosed : entry->m_handle1HadClosed))
            return nullptr;
        return entry;
    }

    MojoResult writeData(MojoHandle handle, const void* elements, uint32_t* count, const MojoWriteDataOptions* options)
    {
        if (!count || (options && (options->struct_size < sizeof(*options) ||
                (options->flags & ~MOJO_WRITE_DATA_FLAG_ALL_OR_NONE))))
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = dataEntry(handle, true);
        if (!entry || (*count % entry->m_elementSize) || (*count && !elements))
            return MOJO_RESULT_INVALID_ARGUMENT;
        if (entry->m_isBeginWritingData)
            return MOJO_RESULT_BUSY;
        if (entry->m_handle1HadClosed)
            return MOJO_RESULT_FAILED_PRECONDITION;
        size_t available = entry->capacityNumBytes - entry->m_dataSize;
        if (options && (options->flags & MOJO_WRITE_DATA_FLAG_ALL_OR_NONE) && *count > available)
            return MOJO_RESULT_OUT_OF_RANGE;
        if (*count && !available)
            return MOJO_RESULT_SHOULD_WAIT;
        *count = std::min<size_t>(*count, available);
        if (*count) {
            size_t position = (entry->m_readPos + entry->m_dataSize) % entry->capacityNumBytes;
            size_t first = std::min<size_t>(*count, entry->capacityNumBytes - position);
            memcpy(entry->m_dataPipe.data() + position, elements, first);
            memcpy(entry->m_dataPipe.data(), static_cast<const char*>(elements) + first, *count - first);
            entry->m_dataSize += *count;
            armTrapEventDelayNotLock(FROM_HERE, entry, MOJO_HANDLE_SIGNAL_READABLE);
        }
        return MOJO_RESULT_OK;
    }

    MojoResult readData(MojoHandle handle, const MojoReadDataOptions* options, void* elements, uint32_t* count)
    {
        constexpr uint32_t allowed = MOJO_READ_DATA_FLAG_ALL_OR_NONE | MOJO_READ_DATA_FLAG_DISCARD |
            MOJO_READ_DATA_FLAG_QUERY | MOJO_READ_DATA_FLAG_PEEK;
        uint32_t flags = options ? options->flags : 0;
        uint32_t modes = flags & ~MOJO_READ_DATA_FLAG_ALL_OR_NONE;
        if (!count || (options && options->struct_size < sizeof(*options)) || (flags & ~allowed) ||
            (modes && (modes & (modes - 1))))
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = dataEntry(handle, false);
        if (!entry)
            return MOJO_RESULT_INVALID_ARGUMENT;
        if (entry->m_isBeginReadingData)
            return MOJO_RESULT_BUSY;
        if (flags & MOJO_READ_DATA_FLAG_QUERY) {
            *count = entry->m_dataSize;
            return MOJO_RESULT_OK;
        }
        if ((*count % entry->m_elementSize) || (*count && !elements && !(flags & MOJO_READ_DATA_FLAG_DISCARD)))
            return MOJO_RESULT_INVALID_ARGUMENT;
        if ((flags & MOJO_READ_DATA_FLAG_ALL_OR_NONE) && *count > entry->m_dataSize)
            return entry->m_handle0HadClosed ? MOJO_RESULT_FAILED_PRECONDITION : MOJO_RESULT_OUT_OF_RANGE;
        if (*count && !entry->m_dataSize)
            return entry->m_handle0HadClosed ? MOJO_RESULT_FAILED_PRECONDITION : MOJO_RESULT_SHOULD_WAIT;
        *count = std::min<size_t>(*count, entry->m_dataSize);
        if (*count && !(flags & MOJO_READ_DATA_FLAG_DISCARD)) {
            size_t first = std::min<size_t>(*count, entry->capacityNumBytes - entry->m_readPos);
            memcpy(elements, entry->m_dataPipe.data() + entry->m_readPos, first);
            memcpy(static_cast<char*>(elements) + first, entry->m_dataPipe.data(), *count - first);
        }
        if (!(flags & MOJO_READ_DATA_FLAG_PEEK)) {
            entry->m_readPos = (entry->m_readPos + *count) % entry->capacityNumBytes;
            entry->m_dataSize -= *count;
            armTrapEventDelayNotLock(FROM_HERE, entry, MOJO_HANDLE_SIGNAL_WRITABLE);
        }
        return MOJO_RESULT_OK;
    }

    MojoResult beginReadData(MojoHandle handle, const MojoBeginReadDataOptions* options, const void** buffer, uint32_t* count)
    {
        if (!buffer || !count || (options && (options->struct_size < sizeof(*options) || options->flags)))
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = dataEntry(handle, false);
        if (!entry)
            return MOJO_RESULT_INVALID_ARGUMENT;
        if (entry->m_isBeginReadingData)
            return MOJO_RESULT_BUSY;
        if (!entry->m_dataSize)
            return entry->m_handle0HadClosed ? MOJO_RESULT_FAILED_PRECONDITION : MOJO_RESULT_SHOULD_WAIT;
        entry->m_readAvailable = std::min<size_t>(entry->m_dataSize, entry->capacityNumBytes - entry->m_readPos);
        entry->m_isBeginReadingData = true;
        *buffer = entry->m_dataPipe.data() + entry->m_readPos;
        *count = entry->m_readAvailable;
        return MOJO_RESULT_OK;
    }

    MojoResult endReadData(MojoHandle handle, uint32_t count, const MojoEndReadDataOptions* options)
    {
        if (options && (options->struct_size < sizeof(*options) || options->flags))
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = dataEntry(handle, false);
        if (!entry)
            return MOJO_RESULT_INVALID_ARGUMENT;
        if (!entry->m_isBeginReadingData)
            return MOJO_RESULT_FAILED_PRECONDITION;
        entry->m_isBeginReadingData = false;
        if (count > entry->m_readAvailable || count % entry->m_elementSize)
            return MOJO_RESULT_INVALID_ARGUMENT;
        entry->m_readPos = (entry->m_readPos + count) % entry->capacityNumBytes;
        entry->m_dataSize -= count;
        armTrapEventDelayNotLock(FROM_HERE, entry, MOJO_HANDLE_SIGNAL_WRITABLE);
        return MOJO_RESULT_OK;
    }

    MojoResult beginWriteData(MojoHandle handle, const MojoBeginWriteDataOptions* options, void** buffer, uint32_t* count)
    {
        if (!buffer || !count || (options && (options->struct_size < sizeof(*options) ||
                (options->flags & ~MOJO_BEGIN_WRITE_DATA_FLAG_ALL_OR_NONE))))
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = dataEntry(handle, true);
        if (!entry)
            return MOJO_RESULT_INVALID_ARGUMENT;
        if (entry->m_isBeginWritingData)
            return MOJO_RESULT_BUSY;
        if (entry->m_handle1HadClosed)
            return MOJO_RESULT_FAILED_PRECONDITION;
        size_t position = (entry->m_readPos + entry->m_dataSize) % entry->capacityNumBytes;
        size_t available = std::min<size_t>(entry->capacityNumBytes - entry->m_dataSize, entry->capacityNumBytes - position);
        if (options && (options->flags & MOJO_BEGIN_WRITE_DATA_FLAG_ALL_OR_NONE) && *count > available)
            return MOJO_RESULT_OUT_OF_RANGE;
        if (!available)
            return MOJO_RESULT_SHOULD_WAIT;
        entry->m_writeAvailable = available;
        entry->m_isBeginWritingData = true;
        *buffer = entry->m_dataPipe.data() + position;
        *count = available;
        return MOJO_RESULT_OK;
    }

    MojoResult endWriteData(MojoHandle handle, uint32_t count, const MojoEndWriteDataOptions* options)
    {
        if (options && (options->struct_size < sizeof(*options) || options->flags))
            return MOJO_RESULT_INVALID_ARGUMENT;
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        auto* entry = dataEntry(handle, true);
        if (!entry)
            return MOJO_RESULT_INVALID_ARGUMENT;
        if (!entry->m_isBeginWritingData)
            return MOJO_RESULT_FAILED_PRECONDITION;
        entry->m_isBeginWritingData = false;
        if (count > entry->m_writeAvailable || count % entry->m_elementSize)
            return MOJO_RESULT_INVALID_ARGUMENT;
        if (entry->m_handle1HadClosed)
            return MOJO_RESULT_FAILED_PRECONDITION;
        entry->m_dataSize += count;
        armTrapEventDelayNotLock(FROM_HERE, entry, MOJO_HANDLE_SIGNAL_READABLE);
        return MOJO_RESULT_OK;
    }

    WTF::RecursiveMutex* getLock()
    {
        return &m_lock;
    }

    void insertConnectorNotLock(/*size_t id, */ void* ptr)
    {
        //m_connectors[id] = ptr;
        m_connectors.insert(ptr);
    }

    void removeConnectorNotLock(/*size_t id*/ void* ptr)
    {
        //m_connectors.erase(id);
        m_connectors.erase(ptr);
    }

    /*void**/ bool findConnectorNotLock(/*size_t id*/ void* ptr)
    {
        //         std::map<size_t, void*>::const_iterator it = m_connectors.find(id);
        //         if (it == m_connectors.end())
        //             return nullptr;
        //         return it->second;
        std::set<void*>::const_iterator it = m_connectors.find(ptr);
        return it != m_connectors.end();
    }

    void bindIpcChannelProxy(uintptr_t pid, IPC::ChannelProxy* channelProxy);

    void unbindIpcChannelProxy(uintptr_t pid)
    {
        //TODO
    }

    // 把这个句柄转换成远程服务端模式
    void changeToRemoteServiceMode(base::ProcessId pid, MojoHandle handle)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        MojoHandleEntry* entry = findEntryNotLock(handle);
        if (!entry) {
            OutputDebugStringA("changeToRemoteServiceMode fail\n");
            return;
        }
        CHECK(entry->m_connectorType == ConnectorType::kNormal);
        if (isHandle0(handle)) {
            CHECK(!entry->m_connector0);
            entry->m_connectorType = ConnectorType::kCon0IsRemoteService;
            entry->m_connector0 = new CrossProcessConnector(pid);
        } else {
            CHECK(!entry->m_connector1);
            entry->m_connectorType = ConnectorType::kCon1IsRemoteService;
            entry->m_connector1 = new CrossProcessConnector(pid);
        }

        char output[100] = { 0 };
        sprintf(output, "changeToRemoteServiceMode: %d\n", handle);
        OutputDebugStringA(output);
    }

    bool changeToRemoteClientModeNoLock(MojoHandle handle, base::ProcessId pid, MojoHandle* localHandle)
    {
        char output[100] = { 0 };
        sprintf(output, "changeToRemoteClientModeNoLock: %d\n", handle);
        OutputDebugStringA(output);

        std::unordered_map<base::ProcessId, MojoProcessInfo*>::const_iterator it = m_processInfo.find(pid);
        if (it == m_processInfo.end()) {
            OutputDebugStringA("changeToRemoteClientModeNoLock fail\n");
            return false;
        }
        MojoProcessInfo* info = it->second;

        MojoHandle handle0;
        MojoHandle handle1;
        MojoHandleEntry* entry = createMessagePipe(&handle0, &handle1);
        entry->m_connectorType = ConnectorType::kCon0IsRemoteClient;
        entry->m_connector0 = new CrossProcessConnector(pid);
        entry->isCon0RemoteClientPending = true;
        *localHandle = handle1;

        info->m_crossProcessMojoHandleMap[handle] = handle0;
        return true;
    }

    bool findRemoteServiceHandle(MojoHandle handle, base::ProcessId pid, MojoHandle* out)
    {
        char output[100] = { 0 };
        std::unordered_map<base::ProcessId, MojoProcessInfo*>::const_iterator it = getProcessInfo().find(pid);
        if (it == getProcessInfo().end()) {
            sprintf(output, "findRemoteServiceHandle fail 1: %d %d\n", handle, pid);
            OutputDebugStringA(output);
            return false;
        }

        std::unordered_map<MojoHandle, MojoHandle>::const_iterator item = it->second->m_crossProcessMojoHandleMap.begin();
        for (; item != it->second->m_crossProcessMojoHandleMap.end(); item++) {
            if (item->second == handle) {
                *out = item->first;
                return true;
            }
        }

        sprintf(output, "findRemoteServiceHandle fail 2: %d %d\n", handle, pid);
        OutputDebugStringA(output);
        return false;
    }

    bool changeToRemoteClientMode(MojoHandle handle, base::ProcessId pid, MojoHandle* localHandle)
    {
        WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
        return changeToRemoteClientModeNoLock(handle, pid, localHandle);
    }

    std::unordered_map<base::ProcessId, MojoProcessInfo*>& getProcessInfo()
    {
        return m_processInfo;
    }

    scoped_refptr<base::SequencedTaskRunner> getDefaultRunner() const
    {
        return m_mojoRunner;
    }

private:
    uint64_t m_seedHandle = 2;
    WTF::RecursiveMutex m_lock;

    int m_reentryCount = 0;

    //---给Connector用
    std::unordered_map<base::ProcessId, MojoProcessInfo*> m_processInfo;

    std::unordered_map<MojoHandle, MojoHandleEntry*> m_handle0ToEntrys;
    std::unordered_map<MojoHandle, MojoHandleEntry*> m_handle1ToEntrys;
    std::unordered_map<MojoHandle, TrapEventEntry*> m_trapEventSet;
    MojoMessageHandle m_messageSeed = 0;
    std::unordered_map<MojoMessageHandle, std::unique_ptr<RawMessage>> m_messages;

    std::unordered_map<int64_t /*pid*/, void* /* IPC::ChannelProxy*/> m_channelProxyMap;

    static const int kMojoHandleEntrySize = sizeof(MojoHandleEntry);
    struct MojoHandleEntryDummy {
        char dummy[kMojoHandleEntrySize];
        EntryType type;
        bool isSet = false;
#ifdef _DEBUG
        void* fromAddrDebug = nullptr;
        std::string callstack;
        uint32_t callstackHash = 0;
#endif // _DEBUG
    };
    std::vector<MojoHandleEntryDummy> m_entryAllocSet;
    int m_entryAllocSetIndex = 0;

    int m_totalEntryCount = 0;
    int m_totalMsgConnelEntryCount = 0;
    base::Time m_gcTime;


    //std::map<size_t, void*> m_connectors; // id查Connector
    std::set<void*> m_connectors;

    scoped_refptr<base::SequencedTaskRunner> m_mojoRunner;

    static MojoHandleMgr* m_inst;
};
MojoHandleMgr* MojoHandleMgr::m_inst = nullptr;

void MojoHandleMgr::bindIpcChannelProxy(uintptr_t pid, IPC::ChannelProxy* channelProxy)
{
    WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
    MojoProcessInfo* info = new MojoProcessInfo();
    info->pid = pid;
    info->m_mainThread = base::SequencedTaskRunner::GetCurrentDefault();
    info->channelProxy = channelProxy;
    channelProxy->AddFilter(info);
    m_processInfo.insert(std::pair<MojoHandle, MojoProcessInfo*>(pid, info));
}

extern "C" {

MojoResult MojoInitialize(const struct MojoInitializeOptions* options)
{
    CHECK(!options);
    MojoHandleMgr::GetInst();
    return MOJO_RESULT_OK;
}

MojoResult MojoCreateMessagePipe(const MojoCreateMessagePipeOptions* options, MojoHandle* handle0, MojoHandle* handle1)
{
    if (!handle0 || !handle1 || handle0 == handle1 ||
        (options && (options->struct_size < sizeof(*options) || options->flags)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->createMessagePipe(handle0, handle1);
    return MOJO_RESULT_OK;
}

void MojoRecordCall(MojoHandle handle, void* addr, const char* from)
{
#if 1 // def _DEBUG_RECODE_STACKTRACE_
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    //std::string stack = printStackTrace();
    mgr->recordCall(handle, /*stack*/ from);
#endif
}

MojoResult MojoSetMessagePipeIsRemoteOrReceiver(MojoHandle handle, bool isRemote)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->setMessagePipeIsRemoteOrReceiver(handle, isRemote);
    return MOJO_RESULT_OK;
}

MojoResult MojoClose(MojoHandle handle)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    MojoHandleSignalsState state;
    if (!mgr->findTrapEventEntryNotLock(handle) && mgr->querySignals(handle, &state) != MOJO_RESULT_OK)
        return MOJO_RESULT_INVALID_ARGUMENT;
    mgr->closeHandle(handle);
    return MOJO_RESULT_OK;
}

// 仅给mbnet的asynMojoClose用，表示生产端已经关闭了，就等消费端关闭就可以把这个handle整个关了。
// 之所以不用MojoClose，是为了等待ResourceScriptStreamer::OnDataPipeReadable里读取完管道里的数据
MojoResult MojoMakeDelayCloseFlag(MojoHandle handle)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->makeDelayCloseFlag(handle);
    return MOJO_RESULT_OK;
}

MojoResult MojoSetDebugMojoHandleStr(MojoHandle handle, const std::string& str)
{
#ifdef _DEBUG
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->setDebugMojoHandleStr(handle, str);
#endif
    return MOJO_RESULT_OK;
}

// 标记是js的MessageChannel
MojoResult MojoMakeIsMessageChannelFlag(MojoHandle handle)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->makeIsMessageChannelFlag(handle);
    return MOJO_RESULT_OK;
}

MojoResult MojoCreateDataPipe(const MojoCreateDataPipeOptions* options, MojoHandle* dataPipeProducerHandle, MojoHandle* dataPipeConsumerHandle)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    return mgr->createDataPipe(options, dataPipeProducerHandle, dataPipeConsumerHandle);
}

MojoResult MojoWriteData(MojoHandle dataPipeProducerHandle, const void* elements, uint32_t* numBytes, const MojoWriteDataOptions* options)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    return mgr->writeData(dataPipeProducerHandle, elements, numBytes, options);
}

MojoResult MojoBeginReadData(MojoHandle dataPipeConsumerHandle, const MojoBeginReadDataOptions* options, const void** buffer, uint32_t* bufferNumBytes)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    return mgr->beginReadData(dataPipeConsumerHandle, options, buffer, bufferNumBytes);
    //return MOJO_RESULT_OK;
}

MojoResult MojoEndReadData(MojoHandle dataPipeConsumerHandle, uint32_t num_bytes_read, const MojoEndReadDataOptions* options)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    return mgr->endReadData(dataPipeConsumerHandle, num_bytes_read, options);
}

MojoResult MojoCreateMessage(const MojoCreateMessageOptions* options, MojoMessageHandle* message)
{
    if (!message || (options && (options->struct_size < sizeof(*options) ||
            (options->flags & ~MOJO_CREATE_MESSAGE_FLAG_UNLIMITED_SIZE))))
        return MOJO_RESULT_INVALID_ARGUMENT;
    // Optional raw Mojo (including MojoJS) has no message-pipe backend here.
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoDestroyMessage(MojoMessageHandle message)
{
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto owned = mgr->takeMessage(message);
    return owned ? MOJO_RESULT_OK : MOJO_RESULT_INVALID_ARGUMENT;
}

MojoResult MojoAppendMessageData(MojoMessageHandle handle, uint32_t payload_size, const MojoHandle* handles, uint32_t num_handles,
    const MojoAppendMessageDataOptions* options, void** buffer, uint32_t* buffer_size)
{
    if ((num_handles && !handles) || (options && (options->struct_size < sizeof(*options) ||
            (options->flags & ~MOJO_APPEND_MESSAGE_DATA_FLAG_COMMIT_SIZE))))
        return MOJO_RESULT_INVALID_ARGUMENT;
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto* message = mgr->findMessage(handle);
    if (!message)
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoGetMessageData(MojoMessageHandle handle, const MojoGetMessageDataOptions* options,
    void** buffer, uint32_t* num_bytes, MojoHandle* handles, uint32_t* num_handles)
{
    if (options && (options->struct_size < sizeof(*options) || (options->flags & ~MOJO_GET_MESSAGE_DATA_FLAG_IGNORE_HANDLES)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    bool ignore_handles = options && (options->flags & MOJO_GET_MESSAGE_DATA_FLAG_IGNORE_HANDLES);
    if (!ignore_handles && num_handles && *num_handles && !handles)
        return MOJO_RESULT_INVALID_ARGUMENT;
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto* message = mgr->findMessage(handle);
    if (!message)
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoWriteMessage(MojoHandle pipe, MojoMessageHandle handle, const MojoWriteMessageOptions* options)
{
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto message = mgr->takeMessage(handle); // Writes consume their message even on failure.
    auto* entry = mgr->findEntryNotLock(pipe);
    if (!message || !entry || entry->m_type == MojoHandleMgr::kData ||
        (pipe == entry->m_handle0 ? entry->m_handle0HadClosed : entry->m_handle1HadClosed) ||
        (options && (options->struct_size < sizeof(*options) || options->flags)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    // MessagePort and utility IPC use Connector::Accept, not this optional API.
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoReadMessage(MojoHandle pipe, const MojoReadMessageOptions* options, MojoMessageHandle* output)
{
    if (options && (options->struct_size < sizeof(*options) || options->flags))
        return MOJO_RESULT_INVALID_ARGUMENT;
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto* entry = mgr->findEntryNotLock(pipe);
    if (!output || !entry || entry->m_type == MojoHandleMgr::kData ||
        (pipe == entry->m_handle0 ? entry->m_handle0HadClosed : entry->m_handle1HadClosed))
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoSetMessageContext(MojoMessageHandle handle, uintptr_t context, MojoMessageContextSerializer serializer,
    MojoMessageContextDestructor destructor, const MojoSetMessageContextOptions* options)
{
    if ((!context && (serializer || destructor)) || (options && (options->struct_size < sizeof(*options) || options->flags)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto* message = mgr->findMessage(handle);
    if (!message)
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoGetMessageContext(MojoMessageHandle handle, const MojoGetMessageContextOptions* options, uintptr_t* context)
{
    if (!context || (options && (options->struct_size < sizeof(*options) || options->flags)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto* message = mgr->findMessage(handle);
    if (!message)
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoSerializeMessage(MojoMessageHandle handle, const MojoSerializeMessageOptions* options)
{
    if (options && (options->struct_size < sizeof(*options) || options->flags))
        return MOJO_RESULT_INVALID_ARGUMENT;
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto* message = mgr->findMessage(handle);
    if (!message)
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoNotifyBadMessage(MojoMessageHandle handle, const char* error, uint32_t size, const MojoNotifyBadMessageOptions* options)
{
    if ((size && !error) || (options && (options->struct_size < sizeof(*options) || options->flags)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto* message = mgr->findMessage(handle);
    if (!message)
        return MOJO_RESULT_INVALID_ARGUMENT;
    LOG(ERROR) << "Invalid Mojo message: " << base::StringPiece(error, size);
    if (message->bad_message_callback)
        std::move(message->bad_message_callback).Run(base::StringPiece(error, size));
    return MOJO_RESULT_OK;
}

// DataPipeBytesConsumer::Notify会调用
MojoResult MojoQueryHandleSignalsState(MojoHandle handle, struct MojoHandleSignalsState* signals_state)
{
    return MojoHandleMgr::GetInst()->querySignals(handle, signals_state);
}

MojoResult MojoReadData(MojoHandle data_pipe_consumer_handle, const struct MojoReadDataOptions* options, void* elements, uint32_t* num_bytes)
{
    return MojoHandleMgr::GetInst()->readData(data_pipe_consumer_handle, options, elements, num_bytes);
}

// Lite has no OS shared-memory dispatcher or cross-process mapping protocol.
// Report that capability boundary rather than manufacturing a usable handle.
MojoResult MojoCreateSharedBuffer(uint64_t num_bytes, const MojoCreateSharedBufferOptions* options, MojoHandle* output)
{
    if (!output || !num_bytes || (options && options->struct_size < sizeof(*options)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoDuplicateBufferHandle(MojoHandle handle, const MojoDuplicateBufferHandleOptions* options, MojoHandle* output)
{
    if (!output || !handle || (options && options->struct_size < sizeof(*options)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoMapBuffer(MojoHandle handle, uint64_t offset, uint64_t num_bytes, const MojoMapBufferOptions* options, void** output)
{
    if (!output || !handle || !num_bytes || offset > UINT64_MAX - num_bytes ||
        (options && options->struct_size < sizeof(*options)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoUnmapBuffer(void* buffer)
{
    return buffer ? MOJO_RESULT_UNIMPLEMENTED : MOJO_RESULT_INVALID_ARGUMENT;
}

MojoResult MojoGetBufferInfo(MojoHandle handle, const MojoGetBufferInfoOptions* options, MojoSharedBufferInfo* info)
{
    if (!handle || !info || info->struct_size < sizeof(*info) || (options && options->struct_size < sizeof(*options)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MOJO_RESULT_UNIMPLEMENTED;
}

MojoResult MojoBeginWriteData(MojoHandle data_pipe_producer_handle, const struct MojoBeginWriteDataOptions* options, void** buffer, uint32_t* buffer_num_bytes)
{
    return MojoHandleMgr::GetInst()->beginWriteData(data_pipe_producer_handle, options, buffer, buffer_num_bytes);
}

MojoResult MojoEndWriteData(MojoHandle data_pipe_producer_handle, uint32_t num_bytes_written, const struct MojoEndWriteDataOptions* options)
{
    return MojoHandleMgr::GetInst()->endWriteData(data_pipe_producer_handle, num_bytes_written, options);
}

MojoResult MojoBindIpcChannelProxy(int64_t pid, void* /* IPC::ChannelProxy*/ channelProxy)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->bindIpcChannelProxy(pid, (IPC::ChannelProxy*)channelProxy);
    return MOJO_RESULT_OK;
}

// 设置这个handle对应的是跨进程模式
MojoResult MojoChangeToRemoteServiceMode(int64_t pid, MojoHandle handle)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->changeToRemoteServiceMode((base::ProcessId)pid, handle);
    return MOJO_RESULT_OK;
}

MojoResult MojoChangeToRemoteClientMode(MojoHandle handle, int64_t pid, MojoHandle* out)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    bool b = mgr->changeToRemoteClientMode(handle, (base::ProcessId)pid, out);
    if (!b)
        return MOJO_RESULT_ABORTED;
    return MOJO_RESULT_OK;
}

} // extern "C"

struct MyMallocHead {
    size_t magic;
    size_t size;
};

const size_t kPostLen = 0x100;

void* malloc_check(size_t numBytes)
{
    CHECK_LE(numBytes, std::numeric_limits<size_t>::max() - sizeof(MyMallocHead) - kPostLen);
    size_t allSize = numBytes + sizeof(MyMallocHead) + kPostLen;
    void* ptr = malloc(allSize);
    CHECK(ptr);
#ifdef _DEBUG
    memset(ptr, 0xff, allSize);
#endif
    MyMallocHead* head = (MyMallocHead*)ptr;
    head->magic = '1234';
    head->size = numBytes;
    void* ret = head + 1;

    return ret;
}

void free_check(void* ptr)
{
    MyMallocHead* head = (MyMallocHead*)ptr;
    head -= 1;
    //unsigned char* bt = (unsigned char*)head;

    if (head->magic != '1234')
        DebugBreak();

    //     size_t i = head->size + sizeof(MyMallocHead);
    //     size_t j = 0;
    //     for (; j < kPostLen; ++i, ++j) {
    //         if (bt[i] != 0xff)
    //             DebugBreak();
    //     }
    free(head);
}

size_t mojo::internal::Buffer::Allocate(size_t numBytes, bool align)
{
    CHECK_LE(numBytes, std::numeric_limits<size_t>::max() - 7);
    const size_t allocation = align ? (numBytes + 7) & ~size_t(7) : numBytes;
    CHECK_LE(allocation, std::numeric_limits<size_t>::max() - cursor_);
    const size_t end = cursor_ + allocation;
    if (end > size_) {
        const size_t capacity = std::max(end, std::min<size_t>(std::numeric_limits<uint32_t>::max(),
            size_ <= std::numeric_limits<size_t>::max() / 2 ? std::max<size_t>(64, size_ * 2) : end));
        void* storage = malloc_check(capacity);
        if (cursor_)
            memcpy(storage, data_, cursor_);
        if (data_)
            free_check(data_);
        data_ = storage;
        size_ = capacity;
    }
    const size_t start = cursor_;
    if (allocation)
        memset(static_cast<uint8_t*>(data_) + start, 0, allocation);
    cursor_ = end;
    return start;
}

mojo::internal::Buffer::Buffer() = default;

mojo::internal::Buffer::Buffer(void* data, size_t size, size_t cursor)
    : data_(data)
    , size_(size)
    , cursor_(cursor)
{
    //     DebugBreak();
    //     DCHECK(IsAligned(data_));
}

mojo::internal::Buffer::Buffer(MessageHandle message, size_t message_payload_size, void* data, size_t size)
    : message_(message)
    , message_payload_size_(message_payload_size)
    , data_(data)
    , size_(size)
    , cursor_(0)
{
    //     DebugBreak();
    //     DCHECK(IsAligned(data_));
}

mojo::internal::Buffer::Buffer(Buffer&& other)
{
    *this = std::move(other);
}

mojo::internal::Buffer& mojo::internal::Buffer::operator=(mojo::internal::Buffer&& other)
{
    if (this == &other)
        return *this;
    Reset();
    message_ = std::exchange(other.message_, MessageHandle());
    message_payload_size_ = std::exchange(other.message_payload_size_, 0);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    cursor_ = std::exchange(other.cursor_, 0);
    return *this;
}

mojo::internal::Buffer::~Buffer()
{
    Reset();
}

void mojo::internal::Buffer::Reset()
{
    if (data_)
        free_check(data_);
    message_payload_size_ = 0;
    message_ = MessageHandle();
    data_ = nullptr;
    size_ = 0;
    cursor_ = 0;
}

uint32_t mojo::Message::payload_num_interface_ids() const
{
    auto* ids = version() < 2 ? nullptr : header_v2()->payload_interface_ids.Get();
    return ids ? ids->size() : 0;
}

const uint32_t* mojo::Message::payload_interface_ids() const
{
    auto* ids = version() < 2 ? nullptr : header_v2()->payload_interface_ids.Get();
    return ids ? ids->storage() : nullptr;
}

void mojo::Message::SerializeHandles(AssociatedGroupController* group)
{
    // Ordinary handles remain owned by Message until TakeMojoMessage transfers
    // them into the raw table. Associated handles require wire interface IDs.
    if (associated_endpoint_handles_.empty())
        return;
    CHECK(group);
    const uint32_t payload_size = payload_num_bytes();
    const size_t count = associated_endpoint_handles_.size();
    Message replacement(name(), header()->flags, payload_size, count, &handles_);
    replacement.header()->interface_id = header()->interface_id;
    replacement.header()->trace_nonce = header()->trace_nonce;
    if (version() >= 1)
        replacement.header_v1()->request_id = header_v1()->request_id;
    replacement.receiver_connection_group_ = receiver_connection_group_;
    replacement.bad_message_callback_ = std::move(bad_message_callback_);
    replacement.heap_profiler_tag_ = heap_profiler_tag_;
    replacement.associated_endpoint_handles_ = std::move(associated_endpoint_handles_);
    if (payload_size)
        memcpy(replacement.payload_buffer_.AllocateAndGet(payload_size), payload(), payload_size);
    *this = std::move(replacement);
    internal::MessageFragment<internal::Array_Data<uint32_t>> fragment(*this);
    fragment.AllocateArrayData(count);
    header_v2()->payload_interface_ids.Set(fragment.data());
    for (size_t i = 0; i < count; ++i)
        fragment->storage()[i] = group->AssociateInterface(std::move(associated_endpoint_handles_[i]));
    associated_endpoint_handles_.clear();
}

bool mojo::Message::DeserializeAssociatedEndpointHandles(AssociatedGroupController* group)
{
    associated_endpoint_handles_.clear();
    const uint32_t count = payload_num_interface_ids();
    if (!count)
        return true;
    if (!group)
        return false;
    auto* ids = header_v2()->payload_interface_ids.Get()->storage();
    associated_endpoint_handles_.reserve(count);
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
        auto handle = group->CreateLocalEndpointHandle(ids[i]);
        if (IsValidInterfaceId(ids[i]) && !handle.is_valid())
            valid = false;
        associated_endpoint_handles_.push_back(std::move(handle));
        ids[i] = kInvalidInterfaceId;
    }
    return valid;
}

// const uint8_t* mojo::Message::payload() const
// {
//     return (const uint8_t*)payload_buffer_.data();
// }
//
// uint32_t mojo::Message::payload_num_bytes() const
// {
//     return payload_buffer_.size();
// }

const uint8_t* mojo::Message::payload() const
{
    const mojo::internal::MessageHeader* headerPtr = header();
    if (version() < 2)
        return data() + headerPtr->num_bytes;

    DCHECK(!header_v2()->payload.is_null());
    return static_cast<const uint8_t*>(header_v2()->payload.Get());
}

uint32_t mojo::Message::payload_num_bytes() const
{
    size_t dataNumBytes = data_num_bytes();
    size_t numBytes = header()->num_bytes;
    DCHECK_GE(dataNumBytes, numBytes);
    size_t numBytesNew;
    if (version() < 2) {
        numBytesNew = data_num_bytes() - numBytes;
    } else {
        uintptr_t payloadBegin = reinterpret_cast<uintptr_t>(header_v2()->payload.Get());
        uintptr_t payloadEnd = reinterpret_cast<uintptr_t>(header_v2()->payload_interface_ids.Get());
        if (!payloadEnd)
            payloadEnd = reinterpret_cast<uintptr_t>(data() + data_num_bytes());
        DCHECK_GE(payloadEnd, payloadBegin);
        numBytesNew = payloadEnd - payloadBegin;
    }
    DCHECK(base::IsValueInRangeForNumericType<uint32_t>(numBytesNew));
    return static_cast<uint32_t>(numBytesNew);
}


// mojo::AssociatedGroup::AssociatedGroup(mojo::AssociatedGroup const&) { *(int*)1 = 1; }
// mojo::AssociatedGroup::~AssociatedGroup(void) { *(int*)1 = 1; }
mojo::AsyncFlusher::AsyncFlusher(mojo::AsyncFlusher&&)
{
    ;
}
mojo::AsyncFlusher::AsyncFlusher(void)
{
    ;
}
mojo::AsyncFlusher::~AsyncFlusher(void)
{
    ;
}


void mojo::Message::SerializeIfNecessary()
{
    serialized_ = true; // 消除原来的意思，现在这个表示payload_buffer_有内存被申请了
}


template <typename HeaderType> void allocateHeaderFromBuffer(mojo::internal::Buffer* buffer, HeaderType** header)
{
    *header = buffer->AllocateAndGet<HeaderType>();
    (*header)->num_bytes = sizeof(HeaderType);
}

static void writeMessageHeader(uint32_t name, uint32_t flags, uint32_t trace_nonce, size_t payload_interface_id_count, mojo::internal::Buffer* payload_buffer)
{
    if (payload_interface_id_count > 0) {
        // Version 2
        mojo::internal::MessageHeaderV2* header;
        allocateHeaderFromBuffer(payload_buffer, &header);
        header->version = 2;
        header->name = name;
        header->flags = flags;
        header->trace_nonce = trace_nonce;
        // The payload immediately follows the header.
        header->payload.Set(header + 1);
    } else if (flags & (mojo::Message::kFlagExpectsResponse | mojo::Message::kFlagIsResponse)) {
        // Version 1
        mojo::internal::MessageHeaderV1* header;
        allocateHeaderFromBuffer(payload_buffer, &header);
        header->version = 1;
        header->name = name;
        header->flags = flags;
        header->trace_nonce = trace_nonce;
    } else {
        mojo::internal::MessageHeader* header;
        allocateHeaderFromBuffer(payload_buffer, &header);
        header->version = 0;
        header->name = name;
        header->flags = flags;
        header->trace_nonce = trace_nonce;
    }
}

mojo::Message::Message(std::unique_ptr<mojo::internal::UnserializedMessageContext> context, MojoCreateMessageFlags create_message_flags)
{
    //     mojo::Message new_message;
    //     context->Serialize(new_message);

    mojo::internal::MessageHeaderV1* headerV1 = context->header();
    writeMessageHeader(headerV1->name, headerV1->flags, /*trace_nonce*/ 0, /*payload_interface_id_count=*/0, &payload_buffer_);
    transferable_ = true;
    serialized_ = true;
    this->header()->interface_id = headerV1->interface_id;
    if (headerV1->flags & (kFlagExpectsResponse | kFlagIsResponse))
        header_v1()->request_id = headerV1->request_id;
    //     int64_t pid = (int64_t)base::GetCurrentProcId();
    //     uint32_t pidLow32 = static_cast<uint32_t>(pid & 0xFFFFFFFF);
    //     uint32_t pidHigh32 = static_cast<uint32_t>((pid >> 32) & 0xFFFFFFFF);
    //     writeMessageHeader(pidLow32, headerV1->flags, pidHigh32, /*payload_interface_id_count=*/0, &payload_buffer_);

    context->Serialize(*this);
    set_heap_profiler_tag("mojom_Message_UnserializedMessageContext");

    //     blink::BlinkTransferableMessage* newMsg = new blink::BlinkTransferableMessage(std::move(msg)); // TODO: 内存泄露
    //
    //     base::span<const uint8_t> data((const uint8_t*)&newMsg, sizeof(blink::BlinkTransferableMessage*));
    //     mojo::Message ret(data, base::span<mojo::ScopedHandle>());
    //     ret.SerializeIfNecessary();
    //
    //     ret.set_heap_profiler_tag("mojom_blink_TransferableMessage_WrapAsMessage");
    //     return ret;
}

std::unique_ptr<mojo::internal::UnserializedMessageContext> mojo::Message::TakeUnserializedContext(const mojo::internal::UnserializedMessageContext::Tag*)
{
    // 见W:\mycode\mb108\mojo\public\cpp\bindings\lib\message.cc：Message::TakeUnserializedContext
    // 如果有Context，说明是本进程，就直接用；如果没有，说明是跨进程了，就需要反序列化
    return nullptr;
}

// Message CreateUnserializedMessage(std::unique_ptr<internal::UnserializedMessageContext> context, MojoCreateMessageFlags create_message_flags)
// {
//     ScopedMessageHandle handle;
//     MojoResult rv = CreateMessage(&handle, create_message_flags);
//     DCHECK_EQ(MOJO_RESULT_OK, rv);
//     DCHECK(handle.is_valid());
//
//     rv = MojoSetMessageContext(handle->value(), reinterpret_cast<uintptr_t>(context.release()), &SerializeUnserializedContext, &DestroyUnserializedContext, nullptr);
//     DCHECK_EQ(MOJO_RESULT_OK, rv);
//
//     return Message::CreateFromMessageHandle(&handle);
// }

// mojo::Message::Message(std::unique_ptr<internal::UnserializedMessageContext> context, MojoCreateMessageFlags create_message_flags)
//     : Message(CreateUnserializedMessage(std::move(context), create_message_flags))
// {
// }

mojo::Message::~Message()
{
    //     void* payloadData = payload_buffer_.data();
    //     if (payloadData && payload_buffer_.size() == 4 && serialized_) {
    //         blink::BlinkTransferableMessage** newMsg = (blink::BlinkTransferableMessage**)payloadData;
    //         delete (*newMsg);
    //     }
}

mojo::Message::Message(uint32_t name, uint32_t flags, size_t payload_size, size_t payload_interface_id_count, MojoCreateMessageFlags create_message_flags,
    std::vector<ScopedHandle>* handles)
{
    writeMessageHeader(name, flags, 0, payload_interface_id_count, &payload_buffer_);
    if (handles)
        handles_.swap(*handles);
    transferable_ = true;
    serialized_ = true;
}

mojo::Message::Message(uint32_t name, uint32_t flags, size_t payload_size, size_t payload_interface_id_count, std::vector<ScopedHandle>* handles)
    : Message(name, flags, payload_size, payload_interface_id_count, MOJO_CREATE_MESSAGE_FLAG_NONE, handles)
{
}

mojo::Message::Message(ScopedMessageHandle handle, const internal::MessageHeaderV1& header)
    : handle_(std::move(handle))
    , transferable_(true)
    , serialized_(true)
{
    writeMessageHeader(header.name, header.flags, header.trace_nonce, 0, &payload_buffer_);
    this->header()->interface_id = header.interface_id;
    if (header.flags & (kFlagExpectsResponse | kFlagIsResponse))
        header_v1()->request_id = header.request_id;
}

mojo::Message mojo::Message::CreateFromMessageHandle(ScopedMessageHandle* handle)
{
    if (!handle || !handle->is_valid())
        return Message();
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    auto* raw = mgr->findMessage(handle->get().value());
    if (!raw)
        return Message();
    auto owned = mgr->takeMessage(handle->release().value());
    Message result;
    result.payload_buffer_ = std::move(owned->payload);
    result.handles_ = std::move(owned->handles);
    result.bad_message_callback_ = std::move(owned->bad_message_callback);
    result.transferable_ = true;
    result.serialized_ = true;
    return result;
}

bool mojo::Message::IsValid() const
{
    return !IsNull();
}

void mojo::Message::Reset()
{
    handle_.reset();
    bad_message_callback_.Reset();
    payload_buffer_.Reset();
    handles_.clear();
    associated_endpoint_handles_.clear();
    receiver_connection_group_ = nullptr;
    transferable_ = false;
    serialized_ = false;
    heap_profiler_tag_ = nullptr;
}

mojo::ScopedMessageHandle mojo::Message::TakeMojoMessage()
{
    if (IsNull() || !associated_endpoint_handles_.empty())
        return ScopedMessageHandle();
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    for (const auto& handle : handles_) {
        if (mgr->canTransferHandle(handle.get().value()) != MOJO_RESULT_OK)
            return ScopedMessageHandle();
    }
    const MojoMessageHandle existing = handle_.is_valid() ? handle_->value() : 0;
    auto storage = existing ? mgr->takeMessage(existing) : std::make_unique<MojoHandleMgr::RawMessage>();
    if (!storage)
        return ScopedMessageHandle();
    storage->payload = std::move(payload_buffer_);
    storage->handles = std::move(handles_);
    storage->bad_message_callback = std::move(bad_message_callback_);
    MojoMessageHandle id;
    if (existing) {
        // Retain identity for a C++ Message constructed from existing storage.
        id = handle_.release().value();
        mgr->restoreMessage(id, std::move(storage));
    } else {
        id = mgr->addMessage(std::move(storage));
    }
    Reset();
    return ScopedMessageHandle(MessageHandle(id));
}

mojo::Message::Message() = default;

mojo::Message::Message(Message&& other)
    : handle_(std::move(other.handle_))
    , bad_message_callback_(std::move(other.bad_message_callback_))
    , payload_buffer_(std::move(other.payload_buffer_))
    , handles_(std::move(other.handles_))
    , associated_endpoint_handles_(std::move(other.associated_endpoint_handles_))
    , receiver_connection_group_(other.receiver_connection_group_)
    , transferable_(other.transferable_)
    , serialized_(other.serialized_)
    , heap_profiler_tag_(other.heap_profiler_tag_)
{
    other.transferable_ = false;
    other.serialized_ = false;
#if defined(ENABLE_IPC_FUZZER)
    interface_name_ = other.interface_name_;
    method_name_ = other.method_name_;
#endif
}

// v8FunInfoToMojoMessage里用到了这个
mojo::Message& mojo::Message::operator=(mojo::Message&& other)
{
    handle_ = std::move(other.handle_);
    bad_message_callback_ = std::move(other.bad_message_callback_);
    payload_buffer_ = std::move(other.payload_buffer_);
    handles_ = std::move(other.handles_);
    associated_endpoint_handles_ = std::move(other.associated_endpoint_handles_);
    receiver_connection_group_ = other.receiver_connection_group_;
    transferable_ = other.transferable_;
    other.transferable_ = false;
    serialized_ = other.serialized_;
    other.serialized_ = false;
    heap_profiler_tag_ = other.heap_profiler_tag_;
#if defined(ENABLE_IPC_FUZZER)
    interface_name_ = other.interface_name_;
    method_name_ = other.method_name_;
#endif
    return *this;
}

//
// Message::Message(
//     uint32_t name, uint32_t flags, size_t payload_size, size_t payload_interface_id_count, MojoCreateMessageFlags create_message_flags, std::vector<ScopedHandle>* handles)
// {
//     *(int*)1 = 1;
// //     uint32_t trace_nonce = static_cast<uint32_t>(base::trace_event::GetNextGlobalTraceId());
// //     TRACE_EVENT(TRACE_DISABLED_BY_DEFAULT("mojom"), "mojo::Message::Message", perfetto::Flow::Global(::mojo::GetTraceId(name, trace_nonce)), "name", name, "flags", flags,
// //         "trace_nonce", trace_nonce);
// //
// //     CreateSerializedMessageObject(name, flags, trace_nonce, payload_size, payload_interface_id_count, create_message_flags, handles, &handle_, &payload_buffer_);
//     transferable_ = true;
//     serialized_ = true;
// }
//
// mojo::Message::Message(uint32_t name, uint32_t flags, size_t payload_size, size_t payload_interface_id_count, std::vector<ScopedHandle>* handles)
//     : mojo::Message(name, flags, payload_size, payload_interface_id_count, MOJO_CREATE_MESSAGE_FLAG_NONE, handles)
// {
// }
//
// Message::Message(ScopedMessageHandle handle, const internal::MessageHeaderV1& header)
//     : handle_(std::move(handle))
//     , transferable_(true)
// {
//     const uint32_t trace_nonce = static_cast<uint32_t>(base::trace_event::GetNextGlobalTraceId());
//     TRACE_EVENT("mojom", "mojo::Message::Message_FromHandle", perfetto::Flow::Global(::mojo::GetTraceId(header.name, trace_nonce)), "this", this);
//
//     void* buffer;
//     uint32_t buffer_size;
//     MojoResult attach_result = MojoAppendMessageData(handle_.get().value(), 0, nullptr, 0, nullptr, &buffer, &buffer_size);
//     if (attach_result != MOJO_RESULT_OK)
//         return;
//
//     payload_buffer_ = internal::Buffer(handle_.get(), 0, buffer, buffer_size);
//     WriteMessageHeader(header.name, header.flags, trace_nonce,
//         /*payload_interface_id_count=*/0, &payload_buffer_);
//
//     // We need to copy additional header data which may have been set after
//     // original message construction, as this codepath may be reached at some
//     // arbitrary time between message send and message dispatch.
//     static_cast<internal::MessageHeader*>(buffer)->interface_id = header.interface_id;
//     if (header.flags & (Message::kFlagExpectsResponse | Message::kFlagIsResponse)) {
//         DCHECK_GE(header.version, 1u);
//         static_cast<internal::MessageHeaderV1*>(buffer)->request_id = header.request_id;
//     }
// }

mojo::Message::Message(base::span<const uint8_t> payload, base::span<mojo::ScopedHandle> handles)
{
    //     MojoResult rv = CreateMessage(&handle_, MOJO_CREATE_MESSAGE_FLAG_NONE);
    //     DCHECK_EQ(MOJO_RESULT_OK, rv);
    //     DCHECK(handle_.is_valid());
    //
    //     void* buffer;
    //     uint32_t buffer_size;
    //     DCHECK(base::IsValueInRangeForNumericType<uint32_t>(payload.size()));
    //     DCHECK(base::IsValueInRangeForNumericType<uint32_t>(handles.size()));
    //     MojoAppendMessageDataOptions options;
    //     options.struct_size = sizeof(options);
    //     options.flags = MOJO_APPEND_MESSAGE_DATA_FLAG_COMMIT_SIZE;
    //     rv = MojoAppendMessageData(handle_->value(), static_cast<uint32_t>(payload.size()), reinterpret_cast<MojoHandle*>(handles.data()), static_cast<uint32_t>(handles.size()),
    //         &options, &buffer, &buffer_size);
    //
    //     // TODO(crbug.com/1239934): Relax this assertion or fail more gracefully.
    //     CHECK_EQ(MOJO_RESULT_OK, rv);
    //
    //     // Handle ownership has been taken by MojoAppendMessageData.
    //     for (auto& handle : handles)
    //         std::ignore = handle.release();
    //
    //     payload_buffer_ = internal::Buffer(buffer, payload.size(), payload.size());
    //     std::copy(payload.begin(), payload.end(), static_cast<uint8_t*>(payload_buffer_.data()));

    for (auto& handle : handles)
        handles_.push_back(std::move(handle));
    payload_buffer_.Allocate(payload.size(), false);
    if (!payload.empty())
        memcpy(payload_buffer_.data(), payload.data(), payload.size());
    transferable_ = true;
    serialized_ = true;
}

//----
static void onConnectorClose(void* ptr)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mojo::Connector* connector = (mojo::Connector*)ptr;
    do {
        WTF::Locker<WTF::RecursiveMutex> locker(*(mgr->getLock()));
        if (!mgr->findConnectorNotLock(ptr))
            return;
    } while (false);

    if (connector)
        connector->RaiseError();
}

void mojo::Connector::RaiseError()
{
    if (!connection_error_handler_.is_null())
        std::move(connection_error_handler_).Run();
}

void mojo::Message::NotifyBadMessage(base::StringPiece error)
{
    LOG(ERROR) << "Invalid Mojo message: " << error;
    if (bad_message_callback_)
        std::move(bad_message_callback_).Run(error);
}

static void clearConnector(const mojo::ScopedMessagePipeHandle& message_pipe, mojo::Connector* connector, bool needRemove)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*(mgr->getLock()));

    MojoHandleMgr::MojoHandleEntry* entry = nullptr;
    do {
        if (!message_pipe.is_valid())
            break;
        entry = mgr->findEntryNotLock(message_pipe->value());
        if (!entry)
            break;

        if (entry->m_connector0 == connector)
            entry->m_connector0 = nullptr;

        if (entry->m_connector1 == connector)
            entry->m_connector1 = nullptr;
    } while (false);

    if (needRemove)
        mgr->removeConnectorNotLock(connector);
}

mojo::ScopedHandleBase<mojo::MessagePipeHandle> mojo::Connector::PassMessagePipe(void)
{
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    CancelWait();
    mojo::internal::MayAutoLock locker(&lock_);

    clearConnector(message_pipe_, this, false);

    ScopedMessagePipeHandle message_pipe = std::move(message_pipe_);
    weak_factory_.InvalidateWeakPtrs();
    //mojo_lite_id_ = 0;

    return message_pipe;
}

mojo::Connector::~Connector(void)
{
//     char output[100] = { 0 };
//     sprintf(output, "~Connector: %p\n", this);
//     OutputDebugStringA(output);

    clearConnector(message_pipe_, this, true);
}

mojo::Connector::Connector(mojo::ScopedMessagePipeHandle message_pipe, mojo::Connector::ConnectorConfig config, scoped_refptr<base::SequencedTaskRunner> runner,
    const char* interface_name)
    : Connector(std::move(message_pipe), config, interface_name)
{
    StartReceiving(std::move(runner));
}

// 一个MessagePipe对应一个Connector
mojo::Connector::Connector(mojo::ScopedHandleBase<mojo::MessagePipeHandle> pipe, mojo::Connector::ConnectorConfig config, char const* name)
    : message_pipe_(std::move(pipe))
    , error_(false)
    , interface_name_(name)
    , header_validator_(base::JoinString({ name ? name : "Generic", "MessageHeaderValidator" }, ""))
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*(mgr->getLock()));
    MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(message_pipe_->value());
    CHECK(entry);

//     char output[100] = { 0 };
//     sprintf(output, "Connector 1: %p\n", this);
//     OutputDebugStringA(output);

    //     static int s_idGen = 1;
    //     mojo_lite_id_ = s_idGen++; // 这个字段冒充id了
    //
    //     bool isHandle0 = mgr->isHandle0(message_pipe_->value());
    //     std::set<size_t>& connectorSet = isHandle0 ? entry->m_connector0Set : entry->m_connector1Set;
    //     connectorSet.insert(mojo_lite_id_);
    //     mgr->insertConnectorNotLock(mojo_lite_id_, this);

    bool isHandle0 = mgr->isHandle0(message_pipe_->value());
    if (isHandle0)
        entry->m_connector0 = this;
    else
        entry->m_connector1 = this;
    mgr->insertConnectorNotLock(this);

    entry->m_type = MojoHandleMgr::kBlinkMessagePort;
}

// static bool isMojoConnectorValid(size_t id)
// {
//     MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
//     WTF::Locker<WTF::RecursiveMutex> locker(*(mgr->getLock()));
//     return mgr->findConnectorNotLock(id);
// }

// void mojo::Connector::OnConnectorAcceptMsg(
//     base::WeakPtr<mojo::Connector> connector,
//     MojoHandle handle,
//     size_t id,
//     mojo::MessageReceiver* incomingReceiver,
//     std::vector<uint8_t>* data)
// {
//     void* ptr = data->data();
//     mojo::Message msg(base::span<const uint8_t>((const uint8_t*)(ptr), data->size()), base::span<mojo::ScopedHandle>());
//     msg.set_heap_profiler_tag("mojom_blink_TransferableMessage_WrapAsMessage");
//
//     mojo::Connector* connectorPtr = connector.get();
//     if (connectorPtr && connectorPtr->incoming_receiver_ && isMojoConnectorValid(id))
//         std::ignore = incomingReceiver->Accept(&msg);
//
//     void* payloadData = msg.payload_buffer()->data();
//     if (payloadData && msg.payload_buffer()->size() == 4) {
//         blink::BlinkTransferableMessage** blinkMsg = (blink::BlinkTransferableMessage**)payloadData;
//         delete (*blinkMsg);
//     }
//
//     delete data;
// }

static void onConnectorAcceptMsg(mojo::Connector* connector, uintptr_t senderPid, mojo::MessageReceiver* incoming_receiver,
    std::unique_ptr<std::vector<char>> data, std::vector<mojo::ScopedHandle> handles)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();

    mgr->getLock()->lock();
    if (!connector || !mgr->findConnectorNotLock(connector) || !incoming_receiver) {
        mgr->getLock()->unlock();
        return;
    }

    for (const auto& handle : handles) {
        if (senderPid != base::GetCurrentProcId()) {
            auto* entry = mgr->findEntryNotLock(handle.get().value());
            if (!entry || !entry->isCon0RemoteClientPending || entry->m_connectorType != ConnectorType::kCon0IsRemoteClient) {
                mgr->getLock()->unlock();
                return;
            }
            entry->isCon0RemoteClientPending = false;
        }
    }
    mgr->getLock()->unlock();

    void* ptr = data->data();
    mojo::Message msg(base::span<const uint8_t>((const uint8_t*)(ptr), data->size()), base::span<mojo::ScopedHandle>(handles.data(), handles.size()));
    msg.set_heap_profiler_tag("mojom_blink_TransferableMessage_WrapAsMessage");
    const MojoHandle receiving_handle = connector->handle().value();
    msg.SetBadMessageCallbackForTransport(base::BindOnce(
        [](mojo::Connector* target, MojoHandle handle, base::StringPiece error) {
            MojoHandleMgr* manager = MojoHandleMgr::GetInst();
            WTF::Locker<WTF::RecursiveMutex> locker(*manager->getLock());
            if (!manager->findConnectorNotLock(target) || target->handle().value() != handle)
                return;
            // Detach before notifying: the error handler may destroy the connector.
            auto pipe = target->PassMessagePipe();
            pipe.reset();
            target->RaiseError();
        }, connector, receiving_handle));
    mojo::Message* outer_message = std::exchange(s_dispatchingMessage, &msg);
    if (!incoming_receiver->Accept(&msg))
        msg.NotifyBadMessage("Incoming receiver rejected the message");
    s_dispatchingMessage = outer_message;

    //     void* payloadData = msg.payload_buffer()->data();
    //     if (payloadData && msg.payload_buffer()->size() == 4) {
    //         blink::BlinkTransferableMessage** blinkMsg = (blink::BlinkTransferableMessage**)payloadData;
    //         delete (*blinkMsg);
    //     }
}

void MojoProcessInfo::OnChannelError()
{
    m_mainThread->PostTask(FROM_HERE, base::BindOnce([](MojoProcessInfo* self) {
        auto* manager = MojoHandleMgr::GetInst();
        WTF::Locker<WTF::RecursiveMutex> locker(*manager->getLock());
        self->channelProxy = nullptr;
        auto exported = std::move(self->m_exportedHandles);
        auto clients = std::move(self->m_crossProcessMojoHandleMap);
        self->m_exportedHandles.clear();
        self->m_crossProcessMojoHandleMap.clear();
        for (const auto& client : clients)
            MojoClose(client.second);
    }, base::Unretained(this)));
}

void MojoProcessInfo::OnChannelConnected(int32 peerPid)
{
}

bool MojoProcessInfo::OnMessageReceived(const IPC::Message& message)
{
    bool handled = true;
#ifdef _WIN32
    bool deserializeSuccess = true;
    IPC_BEGIN_MESSAGE_MAP_EX(MojoProcessInfo, message, deserializeSuccess)
    IPC_MESSAGE_HANDLER(CrossProcessConnector_PostMessage, onPostMessageOnIoThread)
    IPC_MESSAGE_UNHANDLED(handled = false)
    IPC_END_MESSAGE_MAP_EX()
#endif
    return handled;
}

// 本函数跑在ipc io线程
void MojoProcessInfo::onPostMessageOnIoThread(
    uintptr_t senderPid, uintptr_t handle, int senderState, const std::vector<char>& data, const MojoLiteMessagePipeHandles& transferred)
{
    const auto& handles = transferred.second;
    const uint32_t handleKind = transferred.first;
    if (handleKind != kCrossProcessMessagePipeHandleKind || senderPid != pid ||
        !handle || handle > std::numeric_limits<MojoHandle>::max() ||
        (senderState != static_cast<int>(ConnectorType::kCon0IsRemoteClient) &&
            senderState != static_cast<int>(ConnectorType::kCon0IsRemoteService) &&
            senderState != static_cast<int>(ConnectorType::kCon1IsRemoteService)))
        return;
    for (size_t i = 0; i < handles.size(); ++i) {
        if (!handles[i] || handles[i] > std::numeric_limits<MojoHandle>::max() ||
            std::find(handles.begin(), handles.begin() + i, handles[i]) != handles.begin() + i)
            return;
    }
    m_mainThread->PostTask(FROM_HERE,
        base::BindOnce([](MojoProcessInfo* self, uintptr_t senderPid, MojoHandle handle, int senderState, const std::vector<char>& data,
                           const std::vector<uintptr_t>& handles) { self->onPostMessage(senderPid, handle, senderState, data, handles); },
            base::Unretained(this), senderPid, static_cast<MojoHandle>(handle), senderState, data, handles));
}

// 收到其他进程Connector::Accept传来的消息
void MojoProcessInfo::onPostMessage(uintptr_t senderPid, MojoHandle handle, int senderState, const std::vector<char>& data, std::vector<uintptr_t> handles)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*(mgr->getLock()));
    mojo::Connector* otherConnector = nullptr;
    MojoHandleMgr::MojoHandleEntry* entry = nullptr;
    // 客户端和服务端最大的区别是：服务端没保存客户端的句柄，所以客户端那边收到了要转换一下
    if (senderState == (int)ConnectorType::kCon0IsRemoteClient) {
        entry = mgr->findEntryNotLock(handle);
        if (!entry)
            return;
        if (entry->m_connectorType != ConnectorType::kCon0IsRemoteService &&
            entry->m_connectorType != ConnectorType::kCon1IsRemoteService)
            return;

        if (entry->m_connectorType == ConnectorType::kCon0IsRemoteService) {
            if (!mgr->isHandle0(handle))
                return;
            otherConnector = (mojo::Connector*)(entry->m_connector1);

        } else if (entry->m_connectorType == ConnectorType::kCon1IsRemoteService) {
            if (mgr->isHandle0(handle))
                return;
            otherConnector = (mojo::Connector*)(entry->m_connector0);
        }
    } else if (senderState == (int)ConnectorType::kCon0IsRemoteService || senderState == (int)ConnectorType::kCon1IsRemoteService) {
        std::unordered_map<base::ProcessId, MojoProcessInfo*>::const_iterator it = mgr->getProcessInfo().find(senderPid);
        if (it == mgr->getProcessInfo().end())
            return;
        std::unordered_map<MojoHandle, MojoHandle>::const_iterator it2 = it->second->m_crossProcessMojoHandleMap.find(handle);
        if (it2 == it->second->m_crossProcessMojoHandleMap.end())
            return;
        MojoHandle localHandle = it2->second;
        entry = mgr->findEntryNotLock(localHandle);
        if (!entry)
            return;
        if (!mgr->isHandle0(localHandle))
            return;
        otherConnector = (mojo::Connector*)(entry->m_connector1);
        if (!otherConnector && entry->isCon0RemoteClientPending) { // 表示形成这个port的Connector的onConnectorAcceptMsg还没执行
            m_mainThread->PostTask(FROM_HERE,
                base::BindOnce([](MojoProcessInfo* self, uintptr_t senderPid, MojoHandle handle, int senderState, const std::vector<char>& data,
                                   const std::vector<uintptr_t>& handles) { self->onPostMessage(senderPid, handle, senderState, data, handles); },
                    base::Unretained(this), senderPid, handle, senderState, data, handles));
            return;
        }
    } else {
        return;
    }

    if (!otherConnector || !mgr->findConnectorNotLock(otherConnector))
        return;

    // 把远程对端的句柄转换到本地的
    std::vector<mojo::ScopedHandle> ownedHandles;
    ownedHandles.reserve(handles.size());
    for (uintptr_t remoteHandle : handles) {
        MojoHandle localHandle;
        if (!mgr->changeToRemoteClientModeNoLock(static_cast<MojoHandle>(remoteHandle), static_cast<base::ProcessId>(senderPid), &localHandle))
            return;
        ownedHandles.emplace_back(mojo::Handle(localHandle));
    }

    std::unique_ptr<std::vector<char>> dataCopy(new std::vector<char>());
    dataCopy->resize(data.size());
    memcpy(dataCopy->data(), data.data(), data.size());

    otherConnector->task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&onConnectorAcceptMsg, otherConnector, senderPid, otherConnector->incoming_receiver(),
            std::move(dataCopy), std::move(ownedHandles)));
}

bool CrossProcessConnector::postMessageNoLock(
    MojoHandleMgr* mgr, MojoHandle handle, ConnectorType state, std::unique_ptr<std::vector<char>> data, std::vector<uintptr_t> handles)
{
#ifdef _WIN32
    std::unordered_map<base::ProcessId, MojoProcessInfo*>::const_iterator it = mgr->getProcessInfo().find(m_pid);
    if (it == mgr->getProcessInfo().end())
        return false;

    IPC::ChannelProxy* channel = it->second->channelProxy;
    if (!channel)
        return false;

    // The wire contains message-pipe IDs only, never data pipes, traps, or
    // platform buffers. Validate the entire set before changing any endpoint.
    for (size_t i = 0; i < handles.size(); ++i) {
        if (!handles[i] || handles[i] > std::numeric_limits<MojoHandle>::max())
            return false;
        auto* entry = mgr->findEntryNotLock(static_cast<MojoHandle>(handles[i]));
        if (!entry || (entry->m_type != MojoHandleMgr::kMessage && entry->m_type != MojoHandleMgr::kBlinkMessagePort) ||
            mgr->canTransferHandle(static_cast<MojoHandle>(handles[i])) != MOJO_RESULT_OK ||
            std::find(handles.begin(), handles.begin() + i, handles[i]) != handles.begin() + i)
            return false;
    }
    base::Process cur = base::Process::Current();
    const MojoLiteMessagePipeHandles transferred(kCrossProcessMessagePipeHandleKind, std::move(handles));
    if (!channel->Send(new CrossProcessConnector_PostMessage((uintptr_t)cur.Pid(), (uintptr_t)handle, (int)state, *data, transferred)))
        return false;
    for (uintptr_t attached : transferred.second) {
        const MojoHandle id = static_cast<MojoHandle>(attached);
        MojoChangeToRemoteServiceMode(m_pid, id);
        it->second->m_exportedHandles.emplace(id, mojo::ScopedHandle(mojo::Handle(id)));
    }
    return true;
#else
    return false;
#endif // _WIN32
}

// 发送消息给other port。本函数一般来自ApiUtilityProcess::postMessageApi
bool mojo::Connector::Accept(mojo::Message* msg)
{
    //CHECK(task_runner_->RunsTasksInCurrentSequence());

    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*(mgr->getLock()));
    MojoHandle handle = message_pipe_->value();
    MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle);
    if (!entry) {
        OutputDebugStringA("mojo::Connector::Accept fail 1\n");
        return false;
    }

    const std::string tag = msg->heap_profiler_tag() ? msg->heap_profiler_tag() : "";
    CHECK(MojoHandleMgr::kBlinkMessagePort == entry->m_type);
    bool isTransferableMessageWrapAsMessage = tag == "mojom_blink_TransferableMessage_WrapAsMessage";
    bool isTransferableMessageUnserializedMessageContex = tag == "mojom_Message_UnserializedMessageContext";
    CHECK(isTransferableMessageWrapAsMessage || isTransferableMessageUnserializedMessageContex);

    std::unique_ptr<std::vector<char>> data(new std::vector<char>());
    data->resize(msg->data_num_bytes());
    memcpy(data->data(), msg->data(), data->size()); // msg->data()里面是blink::BlinkTransferableMessage或者序列化后的数据

    const std::vector<mojo::ScopedHandle>& handles = *msg->mutable_handles();
    std::vector<uintptr_t> mojoHandles;
    for (size_t i = 0; i < handles.size(); ++i) {
        MojoHandleMgr::MojoHandleEntry* entryTest = mgr->findEntryNotLock(handles[i].get().value());
        content::printCallstack();

        mojoHandles.push_back(handles[i].get().value());
    }

    bool isConnector0 = entry->isConnector0(this);
    if (entry->m_connectorType == ConnectorType::kCon0IsRemoteClient || entry->m_connectorType == ConnectorType::kCon0IsRemoteService) {
        CrossProcessConnector* cpCon = (CrossProcessConnector*)entry->m_connector0;
        MojoHandle recvHandle = entry->m_handle0; // 如果本端是服务端，可以直接发句柄，否则要转换成对面（服务端）的句柄
        if (entry->m_connectorType == ConnectorType::kCon0IsRemoteClient) {
            base::ProcessId servicePid = cpCon->getPid();
            if (!mgr->findRemoteServiceHandle(recvHandle, servicePid, &recvHandle))
                return false;
        }
        const bool sent = cpCon->postMessageNoLock(mgr, recvHandle, entry->m_connectorType, std::move(data), std::move(mojoHandles));
        if (sent) {
            for (auto& attached : *msg->mutable_handles())
                std::ignore = attached.release();
            msg->mutable_handles()->clear();
        }
        return sent;
    } else if (entry->m_connectorType == ConnectorType::kCon1IsRemoteService) {

        CrossProcessConnector* cpCon = (CrossProcessConnector*)entry->m_connector1;
        const bool sent = cpCon->postMessageNoLock(mgr, entry->m_handle1, entry->m_connectorType, std::move(data), std::move(mojoHandles));
        if (sent) {
            for (auto& attached : *msg->mutable_handles())
                std::ignore = attached.release();
            msg->mutable_handles()->clear();
        }
        return sent;
    }

    mojo::Connector* otherConnector = (mojo::Connector*)(isConnector0 ? entry->m_connector1 : entry->m_connector0);
    if (!otherConnector || !mgr->findConnectorNotLock(otherConnector)) {
        OutputDebugStringA("mojo::Connector::Accept fail 4\n");
        return false;
    }

    if (!otherConnector->task_runner_)
        return false;
    return otherConnector->task_runner_->PostTask(FROM_HERE,
        base::BindOnce(&onConnectorAcceptMsg, otherConnector, (uintptr_t)base::GetCurrentProcId(), otherConnector->incoming_receiver_,
            std::move(data), std::move(*msg->mutable_handles())));
}

// mojo\public\cpp\bindings\lib\handle_serialization.cc
void mojo::internal::SerializeHandle(mojo::ScopedHandleBase<mojo::Handle> handle, mojo::Message& message, struct mojo::internal::Handle_Data& data)
{
    if (!handle.is_valid()) {
        data.value = kEncodedInvalidHandleValue;
        return;
    }

    std::vector<ScopedHandle>& handles = *message.mutable_handles();
    data.value = base::checked_cast<uint32_t>(handles.size());
    handles.push_back(std::move(handle));
}

mojo::ScopedHandleBase<mojo::Handle> mojo::internal::DeserializeHandle(const mojo::internal::Handle_Data& data, mojo::Message& message)
{
    if (!data.is_valid())
        return {};

    std::vector<mojo::ScopedHandle>& handles = *message.mutable_handles();
    DCHECK_LT(data.value, handles.size());
    //     const mojo::ScopedHandle& h = handles[data.value];
    //     const internal::MessageHeader* header = message.header();
    //     int64_t senderPid = static_cast<int64_t>(header->trace_nonce) << 32 | static_cast<int64_t>(header->name);
    //     int64_t currentPid = (int64_t)base::GetCurrentProcId();
    //     if (senderPid != currentPid) {
    //         MojoHandle localHandle = 0;
    //         MojoChangeToRemoteClientMode(h->value(), senderPid, &localHandle);
    //         mojo::Handle localH(localHandle);
    //         mojo::ScopedHandleBase<mojo::Handle> localScopedHandle(localH);
    //         return localScopedHandle;
    //     }
    return std::move(handles[data.value]);
}

bool mojo::internal::IsHandleOrInterfaceValid(const mojo::internal::Interface_Data& input)
{
    return input.handle.is_valid();
}

bool mojo::internal::IsHandleOrInterfaceValid(const mojo::internal::Handle_Data& input)
{
    return input.is_valid();
}

void mojo::Connector::StartReceiving(scoped_refptr<base::SequencedTaskRunner> taskRunner, bool allowWokenUpByOthers)
{
    DCHECK(!task_runner_);
    task_runner_ = std::move(taskRunner);
    allow_woken_up_by_others_ = allowWokenUpByOthers;

    DETACH_FROM_SEQUENCE(sequence_checker_);
}

bool mojo::Connector::PrefersSerializedMessages()
{
    // IPC::ChannelProxy transfers bytes, not UnserializedMessageContext objects.
    return true;
}

void mojo::Connector::CancelWait()
{
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    is_receiving_ = false;
    peer_remoteness_tracker_.reset();
    handle_watcher_.reset();
    sync_watcher_.reset();
}

// bool mojo::StructTraits<blink::mojom::TransferableMessageDataView, blink::BlinkTransferableMessage>::Read(
//     blink::mojom::TransferableMessageDataView data, blink::BlinkTransferableMessage* out)
// {
//     return mojo::StructTraits<blink::mojom::blink::TransferableMessage::DataView, blink::BlinkTransferableMessage>::Read(
//         (blink::mojom::blink::TransferableMessage::DataView)data, out);
// }


mojo::GenericPendingReceiver::GenericPendingReceiver(base::StringPiece interface_name, mojo::ScopedMessagePipeHandle receiving_pipe)
    : interface_name_(std::string(interface_name))
    , pipe_(std::move(receiving_pipe))
{
    //     std::string output = "GenericPendingReceiver:";
    //     output += std::string(interface_name.data(), interface_name.size());
    //     output += '\n';
    //     OutputDebugStringA(output.c_str());
}

// mojo::GenericPendingReceiver::GenericPendingReceiver(base::StringPiece interface_name, void** interface_addr)
//     : interface_name_(std::string(interface_name))
//     , interface_addr_(std::move(interface_addr))
// {
// }
//
// void mojo::GenericPendingReceiver::SetInterface(void** ptr)
// {
//     *interface_addr_ = *ptr;
// }

mojo::GenericPendingReceiver::GenericPendingReceiver(mojo::GenericPendingReceiver&&) = default;
mojo::GenericPendingReceiver::~GenericPendingReceiver(void) = default;

mojo::ScopedHandleBase<mojo::MessagePipeHandle> mojo::GenericPendingReceiver::PassPipe(void)
{
    DCHECK(is_valid());
    interface_name_.reset();
    return std::move(pipe_);
}

void mojo::GenericPendingReceiver::reset()
{
    interface_name_.reset();
    pipe_.reset();
}

mojo::ScopedHandleBase<class mojo::MessagePipeHandle> mojo::GenericPendingReceiver::PassPipeIfNameIs(const char* interface_name)
{
    DCHECK(is_valid());
    if (interface_name_ == interface_name)
        return PassPipe();
    return mojo::ScopedMessagePipeHandle();
}

mojo::PendingFlush::PendingFlush(mojo::AsyncFlusher*)
{
}
mojo::PendingFlush::PendingFlush(mojo::PendingFlush&&)
{
    ;
}
mojo::PendingFlush::~PendingFlush(void)
{
    ;
}

// mojo::ReceiverSetState::Entry::~Entry(void) { *(int*)1 = 1; }
// mojo::ReceiverSetState::ReceiverSetState(void) { *(int*)1 = 1; }
// mojo::ReceiverSetState::~ReceiverSetState(void) { *(int*)1 = 1; }

mojo::ReportBadMessageCallback mojo::GetBadMessageCallback()
{
    CHECK(s_dispatchingMessage);
    auto callback = s_dispatchingMessage->TakeBadMessageCallback();
    CHECK(callback);
    return callback;
}

bool mojo::IsInMessageDispatch()
{
    return s_dispatchingMessage != nullptr;
}

//----
#if 0
// class mojo::SimpleWatcher::Context : public base::RefCountedThreadSafe<mojo::SimpleWatcher::Context> {
// public:
//     mojo::SimpleWatcher::Context()
//     {
// 
//     }
// 
//     static void simpleWatcherNotify(base::WeakPtr<mojo::SimpleWatcher> simpleWatcher)
//     {
//         simpleWatcher->context_->m_watcherNotifyCount--;
//         if (!simpleWatcher.get() || simpleWatcher->ready_callback().is_null())
//             return;
//         mojo::HandleSignalsState state;
//         simpleWatcher->ready_callback().Run(MOJO_RESULT_OK, state);
//     }
// 
//     mojo::SimpleWatcher::Context(const Context&) = delete;
//     mojo::SimpleWatcher::Context& operator=(const Context&) = delete;
// 
//     int m_watcherNotifyCount = 0;
// };
// 
// mojo::SimpleWatcher::SimpleWatcher(
//     const base::Location& from_here, mojo::SimpleWatcher::ArmingPolicy arming_policy, scoped_refptr<base::SequencedTaskRunner> runner, const char* handler_tag)
//     : arming_policy_(arming_policy)
//     , task_runner_(std::move(runner))
//     , is_default_task_runner_(base::ThreadTaskRunnerHandle::IsSet() && task_runner_ == base::ThreadTaskRunnerHandle::Get())
//     , context_(new mojo::SimpleWatcher::Context())
//     , handler_tag_(handler_tag ? handler_tag : from_here.file_name())
// {
//    
// }
// 
// mojo::SimpleWatcher::~SimpleWatcher(void) 
// {
//     MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
//     base::AutoLock locker(*(mgr->getLock()));
//     MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle_.value());
//     if (!entry)
//         return;
//     CHECK(entry->m_watcher == this);
//     entry->m_watcher = nullptr;
// }
// 
// MojoResult mojo::SimpleWatcher::Arm(MojoResult* readyResult, mojo::HandleSignalsState* readyState)
// {
//     *(int*)1 = 1;
//     return 0;
// }
// 
// MojoResult mojo::SimpleWatcher::Watch(mojo::Handle handle, MojoHandleSignals signals, MojoTriggerCondition condition, mojo::SimpleWatcher::ReadyCallbackWithState callback)
// {
//     MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
//     base::AutoLock locker(*(mgr->getLock()));
//     MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle.value());
//     if (!entry)
//         return MOJO_RESULT_INVALID_ARGUMENT;
// 
//     handle_ = handle;
//     callback_ = callback;
// 
//     CHECK(!entry->m_watcher);
//     entry->m_watcher = this;
// 
//     if (entry->m_dataPipe.size() > 0)
//         ArmOrNotify();
// 
//     return MOJO_RESULT_OK;
// }
// 
// bool mojo::SimpleWatcher::IsWatching(void) const
// {
//     return false;
// }
// 
// void mojo::SimpleWatcher::ArmOrNotify()
// {
//     if (callback_.is_null() || context_->m_watcherNotifyCount != 0)
//         return;
//     context_->m_watcherNotifyCount++; // 有事没事都可以发个回调，因为回调里面会去判断有没数据来了
//     task_runner_->PostTask(FROM_HERE, base::BindOnce(&mojo::SimpleWatcher::Context::simpleWatcherNotify, weak_factory_.GetWeakPtr()));
// }
// 
// void mojo::SimpleWatcher::Cancel(void)
// {
//     callback_.Reset(); // TODO: 要注意跨线程安全
// }
#else

MojoResult MojoCreateTrap(MojoTrapEventHandler handler, const struct MojoCreateTrapOptions* options, MojoHandle* trapHandle)
{
    if (!handler || !trapHandle || (options && (options->struct_size < sizeof(*options) || options->flags)))
        return MOJO_RESULT_INVALID_ARGUMENT;
    //     MojoHandleMgr::TrapEventEntry* info = new MojoHandleMgr::TrapEventEntry();
    //     info->m_handler = handler;
    //     *trapHandle = (MojoHandle)info;

    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    *trapHandle = mgr->createTrap(handler);
    return MOJO_RESULT_OK;
}

MojoResult MojoAddTrigger(MojoHandle trapHandle, MojoHandle handle, MojoHandleSignals signals, MojoTriggerCondition condition, uintptr_t context,
    const struct MojoAddTriggerOptions* options)
{
    if (options && (options->struct_size < sizeof(*options) || options->flags))
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MojoHandleMgr::GetInst()->addTrigger(handle, trapHandle, context, signals, condition);
}

MojoResult MojoRemoveTrigger(MojoHandle trapHandle, uintptr_t context, const struct MojoRemoveTriggerOptions* options)
{
    if (options && (options->struct_size < sizeof(*options) || options->flags))
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MojoHandleMgr::GetInst()->removeTrigger(trapHandle, context);
}

MojoResult MojoArmTrap(MojoHandle trapHandle, const struct MojoArmTrapOptions* options, uint32_t* numBlockingEvents, struct MojoTrapEvent* blockingEvents)
{
    if (options && (options->struct_size < sizeof(*options) || options->flags))
        return MOJO_RESULT_INVALID_ARGUMENT;
    return MojoHandleMgr::GetInst()->armTrap(trapHandle, numBlockingEvents, blockingEvents);
}

#endif
//----

// mojo::internal::AssociatedInterfacePtrStateBase::AssociatedInterfacePtrStateBase(void)
// {
// }
// mojo::internal::AssociatedInterfacePtrStateBase::~AssociatedInterfacePtrStateBase(void)
// {
// }
//
// mojo::internal::AssociatedReceiverBase::AssociatedReceiverBase(void)
// {
// }

mojo::internal::ReceiverImplBase::ReceiverImplBase(void)
{
    //*(int*)1 = 1;
    id_ = common::LiveIdDetect::get()->constructed(this);
}

mojo::internal::ReceiverImplBase::~ReceiverImplBase(void)
{
    CHECK(common::LiveIdDetect::get()->getPtrLocked(id_));
    CHECK(!impl_);
    Close();
    common::LiveIdDetect::get()->unlock(id_, this);
    common::LiveIdDetect::get()->deconstructed(id_);
}

// ReceiverImplBase是给接收端用的
void mojo::internal::ReceiverImplBase::BindInternal2(mojo::internal::PendingReceiverState* receiver_state, scoped_refptr<base::SequencedTaskRunner> runner,
    const char* interface_name, uint32_t interface_version)
{
    CHECK(receiver_state->pipe->is_valid() && !handle_.is_valid());
    //     receiver_state->raw_ptr_pipe->ptr = impl_;

    // TODO：不确定是不是要reset。按理来说是需要的。因为pipe转移给本this里的handle_了
    MojoHandle handle = receiver_state->pipe.release().value();
    handle_.reset(mojo::MessagePipeHandle(handle));

    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->lock();
    MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle);

    CHECK(entry && !entry->m_interfacePtr);
    entry->m_interfacePtr = impl_ ? impl_ : last_impl_;
    entry->m_receiverImplBaseId = this->GetId();
    ++entry->m_receiverGeneration;
    entry->m_typedScheduled = false;
    entry->m_typedPaused = false;
    entry->m_receiverRunner = mojo::internal::GetTaskRunnerToUseFromUserProvidedTaskRunner(std::move(runner));
    runner_ = entry->m_receiverRunner;
    mgr->scheduleTypedDispatch(entry);
    const MojoHandle peer = mgr->findOtherHandle(handle, entry);
    if (peer == entry->m_handle0 ? entry->m_handle0HadClosed : entry->m_handle1HadClosed)
        mgr->handleMessageClose(peer, entry);
    mgr->unlock();
}

void mojo::internal::ReceiverImplBase::SetImplementation(void* implementation)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    impl_ = implementation;
    if (handle_.is_valid()) {
        auto* entry = mgr->findEntryNotLock(handle_->value());
        CHECK(entry);
        entry->m_interfacePtr = implementation;
    }
}

mojo::ScopedMessagePipeHandle mojo::internal::ReceiverImplBase::PassMessagePipeForUnbind()
{
    auto* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    if (handle_.is_valid()) {
        if (auto* entry = mgr->findEntryNotLock(handle_->value())) {
            entry->m_interfacePtr = nullptr;
            entry->m_receiverImplBaseId = 0;
            ++entry->m_receiverGeneration;
            entry->m_typedScheduled = false;
            entry->m_typedPaused = false;
            entry->m_typedDispatchDepth = 0;
            mgr->wakeTypedWaiters(entry);
        }
    }
    if (impl_)
        last_impl_ = impl_;
    impl_ = nullptr;
    return std::move(handle_);
}

void mojo::internal::RemoteImplBase::EnqueueCall(base::OnceCallback<void(void*)> call)
{
    if (handle_.is_valid())
        MojoHandleMgr::GetInst()->enqueueTypedCall(handle_->value(), std::move(call), true);
}

void mojo::internal::RemoteImplBase::FlushForTesting()
{
    if (!handle_.is_valid())
        return;
    auto* mgr = MojoHandleMgr::GetInst();
    MojoHandle receiver;
    {
        WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
        auto* entry = mgr->findEntryNotLock(handle_->value());
        if (!entry)
            return;
        receiver = mgr->receiverHandle(entry);
    }
    mgr->waitForTypedCall(receiver, true);
}

void mojo::internal::RemoteImplBase::FlushAsyncForTesting(base::OnceClosure callback)
{
    if (!handle_.is_valid())
        return;
    auto reply = base::BindPostTask(runner_, std::move(callback));
    MojoHandleMgr::GetInst()->enqueueTypedCall(handle_->value(), base::BindOnce(
        [](base::OnceClosure reply, void*) { std::move(reply).Run(); }, std::move(reply)), false);
}

void mojo::internal::ReceiverImplBase::PauseIncomingMethodCallProcessing()
{
    if (handle_.is_valid())
        MojoHandleMgr::GetInst()->pauseTypedReceiver(handle_->value(), true);
}

void mojo::internal::ReceiverImplBase::ResumeIncomingMethodCallProcessing()
{
    if (handle_.is_valid())
        MojoHandleMgr::GetInst()->pauseTypedReceiver(handle_->value(), false);
}

bool mojo::internal::ReceiverImplBase::WaitForIncomingMethodCall()
{
    return handle_.is_valid() && MojoHandleMgr::GetInst()->waitForTypedCall(handle_->value(), false);
}

void mojo::internal::ReceiverImplBase::FlushForTesting()
{
    if (handle_.is_valid())
        MojoHandleMgr::GetInst()->waitForTypedCall(handle_->value(), true);
}

void mojo::internal::ReceiverImplBase::set_connection_error_handler(base::OnceClosure error_handler)
{
    // #if DCHECK_IS_ON()
    //     DCHECK(sequence_checker_.CalledOnValidSequence());
    // #endif
    error_handler_ = std::move(error_handler);
    error_with_reason_handler_.Reset();
    //error_runner_ = base::SequencedTaskRunner::GetCurrentDefault();
    //CHECK(runner_->RunsTasksInCurrentSequence());
}

void mojo::internal::ReceiverImplBase::set_connection_error_handler_is_sync()
{
    error_handler_is_sync_ = true;
}

void mojo::internal::RemoteImplBase::set_connection_error_handler_is_sync()
{
    error_handler_is_sync_ = true;
}

void mojo::internal::ReceiverImplBase::set_connection_error_with_reason_handler(ConnectionErrorWithReasonCallback error_handler)
{
    // #if DCHECK_IS_ON()
    //     DCHECK(sequence_checker_.CalledOnValidSequence());
    // #endif
    error_with_reason_handler_ = std::move(error_handler);
    error_handler_.Reset();
    //error_runner_ = base::SequencedTaskRunner::GetCurrentDefault();
    //CHECK(runner_->RunsTasksInCurrentSequence());
}


// 此函数被赋予了新含义：专门给VizClient::destroy用的
void mojo::internal::ReceiverImplBase::PauseRemoteCallbacksUntilFlushCompletes(mojo::PendingFlush flush)
{
}

void mojo::internal::ReceiverImplBase::OnRemoteImplBaseClose(int64_t id, MojoHandle handle, base::OnceClosure cb)
{
    auto* self = static_cast<mojo::internal::ReceiverImplBase*>(common::LiveIdDetect::get()->getPtrLocked(id));
    if (!self)
        return;
    auto runner = self->runner_;
    bool synchronous = self->error_handler_is_sync_;
    self->encountered_error_ = true;
    self->impl_ = nullptr;
    common::LiveIdDetect::get()->unlock(id, self);
    auto notify = base::BindOnce([](int64_t id, MojoHandle handle, base::OnceClosure cb) {
        auto* receiver = static_cast<mojo::internal::ReceiverImplBase*>(common::LiveIdDetect::get()->getPtrLocked(id));
        if (!receiver)
            return;
        base::OnceClosure handler;
        mojo::ConnectionErrorWithReasonCallback reason_handler;
        if (receiver->handle_.is_valid() && receiver->handle_->value() == handle) {
            handler = std::move(receiver->error_handler_);
            reason_handler = std::move(receiver->error_with_reason_handler_);
        }
        common::LiveIdDetect::get()->unlock(id, receiver);
        uint32_t reason = 0;
        std::string description;
        auto* mgr = MojoHandleMgr::GetInst();
        {
            WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
            if (auto* entry = mgr->findEntryNotLock(handle)) {
                reason = entry->m_disconnectReason;
                description = entry->m_disconnectDescription;
            }
        }
        if (handler)
            std::move(handler).Run();
        else if (reason_handler)
            std::move(reason_handler).Run(reason, description);
        if (cb)
            std::move(cb).Run();
    }, id, handle, std::move(cb));
    if (synchronous)
        std::move(notify).Run();
    else if (runner)
        runner->PostTask(FROM_HERE, std::move(notify));
}

void mojo::internal::ReceiverImplBase::OnDisconnectError(MojoHandle handle, base::OnceClosure cb)
{
    //     MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    //     MojoHandleMgr::MojoHandleEntry* entry = nullptr;
    //     do {
    //         base::AutoLock locker(*(mgr->getLock()));
    //         if (!handle_.is_valid())
    //             break;
    //         entry = mgr->findEntryNotLock(handle);
    //         if (!entry /*|| entry->m_isCloseing*/)
    //             break;
    //
    //         entry->m_isCloseing = true;
    //         entry->m_interfacePtr = (void*)(-1); // 放到handle关闭的时候统一做
    //         CHECK(0 == entry->m_receiverImplBaseId);
    //     } while (false);

    handle_.reset();

    if (error_handler_) {
        std::move(error_handler_).Run(); // error_handler_可能需要handle访问MojoHandleEntry。比如走到ReceiverImplBase::Close()里
    } else if (error_with_reason_handler_) {
        std::move(error_with_reason_handler_).Run(0, std::string());
    }

    if (cb)
        std::move(cb).Run();

    impl_ = nullptr;
}

void mojo::internal::ReceiverImplBase::Close()
{
    //     MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    //     mojo::internal::RemoteImplBase* remoteImplBase = nullptr;
    //     MojoHandleMgr::MojoHandleEntry* entry = nullptr;
    //     do {
    //         base::AutoLock locker(*(mgr->getLock()));
    //         if (!handle_.is_valid())
    //             break;
    //         entry = mgr->findEntryNotLock(handle_->value());
    //         if (!entry /*|| entry->m_isCloseing*/)
    //             break;
    //
    //         entry->m_isCloseing = true;
    //         entry->m_interfacePtr = (void*)(-1); // 不能在这里清空？因为还有发送器未发送的消息
    //         entry->m_receiverImplBase = nullptr;
    //         if (!entry->m_remoteImplBase)
    //             break;
    //         remoteImplBase = (mojo::internal::RemoteImplBase*)entry->m_remoteImplBase;
    //     } while (false);
    //
    //     if (remoteImplBase)
    //         remoteImplBase->OnReceiverImplBaseClose(std::move(reset_handler_)); // 本接收端析构的话，通知发送端

    handle_.reset();
    if (impl_)
        last_impl_ = impl_; // receiver.reset();后可能还要再绑定
    impl_ = nullptr;
}

void mojo::internal::ReceiverImplBase::CloseWithReason(uint32_t custom_reason, base::StringPiece description)
{
    auto* mgr = MojoHandleMgr::GetInst();
    {
        WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
        if (handle_.is_valid()) {
            if (auto* entry = mgr->findEntryNotLock(handle_->value())) {
                entry->m_disconnectReason = custom_reason;
                entry->m_disconnectDescription = std::string(description);
            }
        }
    }
    Close();
}

void mojo::internal::ReceiverImplBase::RaiseError()
{
    //endpoint_client_->RaiseError();
}

bool mojo::internal::ReceiverImplBase::is_bound() const
{
    return handle_.get().is_valid();
}

mojo::MessagePipeHandle mojo::internal::ReceiverImplBase::handle() const
{
    DCHECK(is_bound());
    return handle_.get();
}

mojo::internal::RemoteImplBase::RemoteImplBase()
{
    id_ = common::LiveIdDetect::get()->constructed(this);
    //     static int s_count = 0;
    //     char output[100] = { 0 };
    //     sprintf_s(output, 99, "RemoteImplBase: %p, %d\n", this, s_count);
    //     if (s_count == 121)
    //         OutputDebugStringA("");
    //     if (s_count == 83)
    //         OutputDebugStringA("");
    //     OutputDebugStringA(output);
    //     s_count++;
}

mojo::internal::RemoteImplBase::RemoteImplBase(mojo::internal::RemoteImplBase&& other)
    : RemoteImplBase()
{
    Swap(&other);
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
    if (handle_.is_valid()) {
        auto* entry = mgr->findEntryNotLock(handle_->value());
        CHECK(entry);
        entry->m_remoteImplBaseId = id_;
    }
}

mojo::internal::RemoteImplBase::~RemoteImplBase(void)
{
    //     static int s_count = 0;
    //     char output[100] = { 0 };
    //     sprintf_s(output, 99, "~RemoteImplBase: %p, %d\n", this, s_count);
    //     if (s_count == 55)
    //         OutputDebugStringA("");
    //     OutputDebugStringA(output);
    //     s_count++;

    CHECK(common::LiveIdDetect::get()->getPtrLocked(id_));
    Swap(nullptr);
    common::LiveIdDetect::get()->unlock(id_, this);
    common::LiveIdDetect::get()->deconstructed(id_);
}

void mojo::internal::RemoteImplBase::Swap(mojo::internal::RemoteImplBase* other)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mojo::internal::ReceiverImplBase* receiverImplBase = nullptr;
    mojo::internal::RemoteImplBase* otherRemoteImplBase = nullptr;

    // 分三种情况：1.me valid, 2.other valid, 3.other == null
    MojoHandle handle = 0;
    bool needNotifReceiver = true;
    if (handle_.is_valid()) {
        CHECK(!other || !other->handle_.is_valid());
        handle = handle_->value();
        if (other && !other->handle_.is_valid()) {
            otherRemoteImplBase = other;
            needNotifReceiver = false; // 本情况，handle只是转移，并不是销毁，所以不需要通知
        }
    } else if (other && other->handle_.is_valid()) {
        CHECK(!handle_.is_valid());
        handle = other->handle_->value();
        otherRemoteImplBase = this;
        needNotifReceiver = false; // 本情况，handle只是转移，并不是销毁，所以不需要通知
    } else if (!other) {
        if (handle_.is_valid())
            handle = handle_->value();
    } else if (!handle_.is_valid() && (!other || !other->handle_.is_valid())) {

    } else
        DebugBreak();

    //     MojoHandleMgr::MojoHandleEntry* entry = nullptr;
    //     do {
    //         base::AutoLock locker(*(mgr->getLock()));
    //         if (0 == handle)
    //             break;
    //         entry = mgr->findEntryNotLock(handle);
    //
    //         if (!entry /*|| !entry->m_isCloseing*/)
    //             break;
    //
    //         //entry->m_interfacePtr = nullptr;
    //         entry->m_isCloseing = true;
    //         entry->m_remoteImplBase = otherRemoteImplBase;
    //
    //         if (!entry->m_receiverImplBase)
    //             break;
    //         receiverImplBase = entry->m_receiverImplBase;
    //     } while (false);
    //
    //     if (needNotifReceiver && receiverImplBase)
    //         receiverImplBase->OnRemoteImplBaseClose(std::move(reset_handler_)); // 本发送端析构的话，通知接收端

    if (other) {
        handle_.swap(other->handle_);
        runner_.swap(other->runner_);
        std::swap(other->version_, version_);

        //std::swap(other->reset_handler_, reset_handler_);
        std::swap(other->error_handler_, error_handler_);
        std::swap(other->connection_error_handler_, connection_error_handler_);
    }
}

// void mojo::internal::RemoteImplBase::set_reset_handler(base::OnceClosure handler)
// {
//     reset_handler_ = std::move(handler);
// }
//
// void mojo::internal::ReceiverImplBase::set_reset_handler(base::OnceClosure handler)
// {
//     reset_handler_ = std::move(handler);
// }

mojo::internal::PendingRemoteState mojo::internal::RemoteImplBase::Unbind(void)
{
    ScopedMessagePipeHandle handle = std::move(handle_);
    Swap(nullptr);
    return mojo::internal::PendingRemoteState(std::move(handle), 0);
}

uint32_t mojo::internal::RemoteImplBase::version() const
{
    return version_;
}

bool mojo::internal::RemoteImplBase::is_bound() const
{
    return handle_.is_valid();
}

bool mojo::internal::RemoteImplBase::is_valid() const
{
    if (!is_bound())
        return false;
    return !!TryGetInstance();
}

mojo::MessagePipeHandle mojo::internal::RemoteImplBase::handle() const
{
    return handle_.get();
}

void mojo::internal::RemoteImplBase::CloseWithReason(uint32_t custom_reason, const std::string& description)
{
    auto* mgr = MojoHandleMgr::GetInst();
    {
        WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
        if (handle_.is_valid()) {
            if (auto* entry = mgr->findEntryNotLock(handle_->value())) {
                entry->m_disconnectReason = custom_reason;
                entry->m_disconnectDescription = description;
            }
        }
    }
    handle_.reset();
    cache_interface_ptr_ = nullptr;
}

void mojo::internal::RemoteImplBase::OnReceiverImplBaseClose(int64_t id, MojoHandle handle, base::OnceClosure reset_handler)
{
    auto* self = static_cast<mojo::internal::RemoteImplBase*>(common::LiveIdDetect::get()->getPtrLocked(id));
    if (!self)
        return;
    auto runner = self->runner_;
    bool synchronous = self->error_handler_is_sync_;
    self->cache_interface_ptr_ = nullptr;
    common::LiveIdDetect::get()->unlock(id, self);
    auto notify = base::BindOnce([](int64_t id, MojoHandle handle, base::OnceClosure cb) {
        auto* remote = static_cast<mojo::internal::RemoteImplBase*>(common::LiveIdDetect::get()->getPtrLocked(id));
        if (!remote)
            return;
        base::OnceClosure handler;
        mojo::ConnectionErrorWithReasonCallback reason_handler;
        if (remote->handle_.is_valid() && remote->handle_->value() == handle) {
            handler = std::move(remote->error_handler_);
            reason_handler = std::move(remote->connection_error_handler_);
        }
        common::LiveIdDetect::get()->unlock(id, remote);
        uint32_t reason = 0;
        std::string description;
        auto* mgr = MojoHandleMgr::GetInst();
        {
            WTF::Locker<WTF::RecursiveMutex> locker(*mgr->getLock());
            if (auto* entry = mgr->findEntryNotLock(handle)) {
                reason = entry->m_disconnectReason;
                description = entry->m_disconnectDescription;
            }
        }
        if (handler)
            std::move(handler).Run();
        else if (reason_handler)
            std::move(reason_handler).Run(reason, description);
        if (cb)
            std::move(cb).Run();
    }, id, handle, std::move(reset_handler));
    if (synchronous)
        std::move(notify).Run();
    else if (runner)
        runner->PostTask(FROM_HERE, std::move(notify));
}

void mojo::internal::RemoteImplBase::OnDisconnectError(MojoHandle handle, base::OnceClosure reset_handler)
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mojo::internal::RemoteImplBase* self = this;

    //     mgr->lock();
    //     do {
    //         MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle);
    //         if (!entry)
    //             break;
    //
    //         self = (mojo::internal::RemoteImplBase*)entry->m_remoteImplBase;
    //         if (!self)
    //             break;
    //     } while (false);
    //     mgr->unlock();

    if (self && self->error_handler_) {
        std::move(self->error_handler_).Run();
    } else if (self && self->connection_error_handler_) {
        std::move(self->connection_error_handler_).Run(0, std::string());
    }

    if (reset_handler)
        std::move(reset_handler).Run();
}

bool mojo::internal::RemoteImplBase::encountered_error() const
{
    if (cache_interface_ptr_)
        return false;

    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();

    mgr->lock();
    MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle_->value());
    CHECK(entry);

    int count = 0;
    void* interface_ptr = nullptr;
    while (true) {
        interface_ptr = entry->m_interfacePtr;
        if (interface_ptr || count > 2)
            break;
        mgr->unlock();
        base::PlatformThread::Sleep(base::Milliseconds(10));
        count++;
        mgr->lock();
    }
    mgr->unlock();

    return ((void*)(-1) == interface_ptr) || (nullptr == interface_ptr);
}

// RemoteImplBase是给发送端用的
void* mojo::internal::RemoteImplBase::GetInstance()
{
    CHECK(handle_.is_valid());

#if 0
    if (cache_interface_ptr_)
        return cache_interface_ptr_;
#endif
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();

    mgr->lock();
    MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle_->value());
    if (!entry) {
        mgr->unlock();
        return nullptr;
    }

    int count = 0;
    while (true) {
        cache_interface_ptr_ = entry->m_interfacePtr; // 有可能为-1，表示关闭了
        if (cache_interface_ptr_ /*|| count > 100*/)
            break;
        mgr->unlock();
        base::PlatformThread::Sleep(base::Milliseconds(10));
        count++;

        mgr->lock();
        if (count > 3 && entry->m_enableUnassociatedUsage)
            break;
    }
    mgr->unlock();

    if ((void*)(-1) == cache_interface_ptr_)
        cache_interface_ptr_ = nullptr;
    return cache_interface_ptr_;
}

void* mojo::internal::RemoteImplBase::TryGetInstance() const
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();

    mgr->lock();
    MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle_->value());
    if (!entry) {
        mgr->unlock();
        return nullptr;
    }

    cache_interface_ptr_ = entry->m_interfacePtr;
    mgr->unlock();

    if ((void*)(-1) == cache_interface_ptr_)
        cache_interface_ptr_ = nullptr;
    return cache_interface_ptr_;
}

scoped_refptr<base::SequencedTaskRunner> mojo::internal::RemoteImplBase::GetReceiverTaskRunner()
{
    scoped_refptr<base::SequencedTaskRunner> runner;
    if (!handle_.is_valid())
        return runner;

    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();

    mgr->lock();
    MojoHandleMgr::MojoHandleEntry* entry = mgr->findEntryNotLock(handle_->value());
    CHECK(entry);

    int count = 0;
    while (true) {
        cache_interface_ptr_ = entry->m_interfacePtr; // 有可能为-1，表示关闭了
        if (cache_interface_ptr_ /*|| count > 100*/)
            break;
        mgr->unlock();
        base::PlatformThread::Sleep(base::Milliseconds(10));
        count++;

        mgr->lock();
        if (count > 3 && entry->m_enableUnassociatedUsage)
            break;
    }

    if ((void*)(-1) == cache_interface_ptr_) {
        cache_interface_ptr_ = nullptr;
    } else {
        runner = entry->m_receiverRunner;
    }
    mgr->unlock();
    return runner;
}

base::OnceCallback<void __cdecl(base::StringPiece)> mojo::internal::ReceiverImplBase::GetBadMessageCallback()
{
    return base::BindOnce([](int64_t receiver_id, base::StringPiece error) {
        LOG(ERROR) << "Invalid Mojo message: " << error;
        auto* receiver = static_cast<mojo::internal::ReceiverImplBase*>(
            common::LiveIdDetect::get()->getPtrLocked(receiver_id));
        if (!receiver)
            return;
        receiver->Close();
        common::LiveIdDetect::get()->unlock(receiver_id, receiver);
    }, id_);
}

void mojo::internal::RemoteImplBase::Bind(mojo::internal::PendingRemoteState* remoteState, scoped_refptr<base::SequencedTaskRunner> taskRunner)
{
    DCHECK(!handle_.is_valid());
    DCHECK_EQ(0u, version_);

    //handle_ = std::move(remoteState->pipe);
    version_ = remoteState->version;

    // 这个raw_ptr_pipe可以是有值，也可以没值。有值表示是远端绑定好了送过来的
    // 没值表示本地即将送去远端
    //raw_ptr_message_pipe_ = remoteState->raw_ptr_pipe;

    MojoHandle pendingRemoteHandle = remoteState->pipe->value();
    CHECK(pendingRemoteHandle != MOJO_HANDLE_INVALID);
    // Publish a fully initialized remote before receiver closure can notify it.
    handle_ = std::move(remoteState->pipe);
    runner_ = mojo::internal::GetTaskRunnerToUseFromUserProvidedTaskRunner(std::move(taskRunner));

    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->lock();
    MojoHandleMgr::MojoHandleEntry* pendingRemoteEntry = mgr->findEntryNotLock(pendingRemoteHandle);
    CHECK(pendingRemoteEntry);
    pendingRemoteEntry->m_remoteImplBaseId = id_;
    if (!mgr->receiverIsOpen(pendingRemoteEntry))
        mgr->handleMessageClose(mgr->receiverHandle(pendingRemoteEntry), pendingRemoteEntry);
    mgr->unlock();

}

mojo::internal::PendingReceiverState::PendingReceiverState() = default;

mojo::internal::PendingReceiverState::PendingReceiverState(mojo::ScopedMessagePipeHandle pipe)
    : pipe(std::move(pipe))
{
}

void mojo::internal::PendingReceiverState::EnableUnassociatedUsage()
{
    MojoHandleMgr* mgr = MojoHandleMgr::GetInst();
    mgr->lock();
    MojoHandleMgr::MojoHandleEntry* pendingRemoteEntry = mgr->findEntryNotLock(pipe->value());
    CHECK(pendingRemoteEntry);
    pendingRemoteEntry->m_enableUnassociatedUsage = true;
    mgr->unlock();
}

// mojo::internal::PendingReceiverState::PendingReceiverState(scoped_refptr<RawPtrMessagePipe> pipe)
//     : raw_ptr_pipe(pipe)
// {
// }

mojo::internal::PendingReceiverState::PendingReceiverState(mojo::internal::PendingReceiverState&&) noexcept = default;

mojo::internal::PendingReceiverState::~PendingReceiverState() = default;

mojo::internal::PendingReceiverState& mojo::internal::PendingReceiverState::operator=(mojo::internal::PendingReceiverState&& other) noexcept = default;

void mojo::internal::PendingReceiverState::reset()
{
    pipe.reset();
}


// class mojo::ScopedInterfaceEndpointHandle::State : public base::RefCountedThreadSafe<mojo::ScopedInterfaceEndpointHandle::State> {
// public:
//     mojo::ScopedInterfaceEndpointHandle::State() = default;
//
//     mojo::ScopedInterfaceEndpointHandle::State(const State&) = delete;
//     mojo::ScopedInterfaceEndpointHandle::State& operator=(const ScopedInterfaceEndpointHandle::State&) = delete;
//
//     void* impl_= nullptr;
//     bool is_pending_association_ = false;
// };
//
// mojo::ScopedInterfaceEndpointHandle::ScopedInterfaceEndpointHandle()
//     : state_(base::MakeRefCounted<State>())
// {
// }
//
// mojo::ScopedInterfaceEndpointHandle::ScopedInterfaceEndpointHandle(ScopedInterfaceEndpointHandle&& other)
//     : state_(base::MakeRefCounted<State>())
// {
//     state_.swap(other.state_);
// }
//
// mojo::ScopedInterfaceEndpointHandle::~ScopedInterfaceEndpointHandle()
// {
//     //state_->Close(absl::nullopt);
// }
//
// mojo::ScopedInterfaceEndpointHandle& mojo::ScopedInterfaceEndpointHandle::operator=(mojo::ScopedInterfaceEndpointHandle&& other)
// {
//     reset();
//     state_.swap(other.state_);
//     return *this;
// }
//
// bool mojo::ScopedInterfaceEndpointHandle::is_valid(void) const
// {
//     return !!state_->impl_ || state_->is_pending_association_;
// }
//
// void mojo::ScopedInterfaceEndpointHandle::reset(void)
// {
//     scoped_refptr<State> new_state = base::MakeRefCounted<State>();
//     //state_->Close(reason);
//     state_.swap(new_state);
// }
//
// // !!!
// void mojo::ScopedInterfaceEndpointHandle::CreatePairPendingAssociation(mojo::ScopedInterfaceEndpointHandle* handle0, mojo::ScopedInterfaceEndpointHandle* handle1)
// {
//     ScopedInterfaceEndpointHandle result0;
//     ScopedInterfaceEndpointHandle result1;
//
//     result0.state_ = result1.state_;
//
//     result0.state_->is_pending_association_ = true;
//     result1.state_->is_pending_association_ = true;
//
//     *handle0 = std::move(result0);
//     *handle1 = std::move(result1);
// }
//
// mojo::ScopedInterfaceEndpointHandle mojo::internal::AssociatedInterfacePtrStateBase::PassHandle(void)
// {
//     return std::move(handle_);
// }
//
// void mojo::internal::AssociatedInterfacePtrStateBase::Bind2(
//     mojo::ScopedInterfaceEndpointHandle handle,
//     uint32_t version,
//     scoped_refptr<base::SequencedTaskRunner> runner,
//     const char* interface_name)
// {
//     DCHECK(handle_.state_->HasOneRef() && !(handle_.state_->impl_));
//     handle_.state_ = handle.state_;
// }

void mojo::internal::RemoteImplBase::FlushAsync(mojo::AsyncFlusher)
{
}

void mojo::internal::RemoteImplBase::PauseReceiverUntilFlushCompletes(mojo::PendingFlush)
{
}

// void mojo::internal::AssociatedInterfacePtrStateBase::Swap(class mojo::internal::AssociatedInterfacePtrStateBase* other)
// {
//     handle_ = std::move(other->handle_);
//     version_ = other->version();
// }
//
// void mojo::internal::AssociatedReceiverBase::BindImpl2(ScopedInterfaceEndpointHandle handle, void* impl,
//     scoped_refptr<base::SequencedTaskRunner> runner, uint32_t interface_version, const char* interface_name)
// {
//     if (handle.state_.get())
//         handle.state_->impl_ = impl;
// }
//
// void* mojo::internal::AssociatedInterfacePtrStateBase::GetInstanceImpl(void)
// {
//     return handle_.state_->impl_;
// }
//
// mojo::internal::AssociatedReceiverBase::~AssociatedReceiverBase(void)
// {
//     if (impl_)
//         *(int*)1 = 1;
// }
//
// void mojo::internal::AssociatedReceiverBase::set_disconnect_handler(base::OnceCallback<void __cdecl(void)>)
// {
//
// }
//
// void mojo::internal::AssociatedReceiverBase::set_disconnect_with_reason_handler(base::OnceCallback<void __cdecl(unsigned int, std::string const&)>)
// {
//
// }
//
// bool mojo::internal::AssociatedReceiverBase::is_bound() const { return !!endpoint_client_; }
// mojo::internal::AssociatedReceiverBase::operator bool() const { return !!endpoint_client_; }
//
// mojo::AssociatedGroup* mojo::InterfaceEndpointClient::associated_group(void)
// {
//     *(int*)1 = 1;
//     return nullptr;
// }

void mojo::InterfaceEndpointClient::CloseWithReason(uint32_t custom_reason, base::StringPiece)
{
    OutputDebugStringA("");
}

namespace mojo::internal {
namespace {

// These are typed lite messages: each closure owns its arguments until the
// receiver sequence dispatches it. No serialization or fake receiver is used.
class QueuedURLLoaderClient final : public network::mojom::URLLoaderClient {
public:
    explicit QueuedURLLoaderClient(RemoteImplBase* remote) : remote_(remote) {}

    void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr hints) override
    {
        remote_->EnqueueCall(base::BindOnce([](network::mojom::EarlyHintsPtr hints, void* receiver) {
            static_cast<network::mojom::URLLoaderClient*>(receiver)->OnReceiveEarlyHints(std::move(hints));
        }, std::move(hints)));
    }

    void OnReceiveResponse(network::mojom::URLResponseHeadPtr head, ScopedDataPipeConsumerHandle body,
        std::optional<mojo_base::BigBuffer> metadata) override
    {
        remote_->EnqueueCall(base::BindOnce([](network::mojom::URLResponseHeadPtr head, ScopedDataPipeConsumerHandle body,
            std::optional<mojo_base::BigBuffer> metadata, void* receiver) {
            static_cast<network::mojom::URLLoaderClient*>(receiver)->OnReceiveResponse(std::move(head), std::move(body), std::move(metadata));
        }, std::move(head), std::move(body), std::move(metadata)));
    }

    void OnReceiveRedirect(const net::RedirectInfo& info, network::mojom::URLResponseHeadPtr head) override
    {
        remote_->EnqueueCall(base::BindOnce([](net::RedirectInfo info, network::mojom::URLResponseHeadPtr head, void* receiver) {
            static_cast<network::mojom::URLLoaderClient*>(receiver)->OnReceiveRedirect(info, std::move(head));
        }, info, std::move(head)));
    }

    void OnUploadProgress(int64_t position, int64_t size, OnUploadProgressCallback callback) override
    {
        auto reply = base::BindPostTask(remote_->runner(), std::move(callback));
        remote_->EnqueueCall(base::BindOnce([](int64_t position, int64_t size, OnUploadProgressCallback callback, void* receiver) {
            static_cast<network::mojom::URLLoaderClient*>(receiver)->OnUploadProgress(position, size, std::move(callback));
        }, position, size, std::move(reply)));
    }

    void OnTransferSizeUpdated(int32_t difference) override
    {
        remote_->EnqueueCall(base::BindOnce([](int32_t difference, void* receiver) {
            static_cast<network::mojom::URLLoaderClient*>(receiver)->OnTransferSizeUpdated(difference);
        }, difference));
    }

    void OnComplete(const network::URLLoaderCompletionStatus& status) override
    {
        remote_->EnqueueCall(base::BindOnce([](network::URLLoaderCompletionStatus status, void* receiver) {
            static_cast<network::mojom::URLLoaderClient*>(receiver)->OnComplete(status);
        }, status));
    }

private:
    RemoteImplBase* const remote_;
};

template <typename Interface> class QueuedDataPipeGetter final : public Interface {
public:
    using ReadCallback = typename Interface::ReadCallback;
    explicit QueuedDataPipeGetter(RemoteImplBase* remote) : remote_(remote) {}

    void Read(ScopedDataPipeProducerHandle pipe, ReadCallback callback) override
    {
        auto reply = base::BindPostTask(remote_->runner(), std::move(callback));
        remote_->EnqueueCall(base::BindOnce([](ScopedDataPipeProducerHandle pipe, ReadCallback callback, void* receiver) {
            static_cast<Interface*>(receiver)->Read(std::move(pipe), std::move(callback));
        }, std::move(pipe), std::move(reply)));
    }

    void Clone(PendingReceiver<Interface> receiver) override
    {
        remote_->EnqueueCall(base::BindOnce([](PendingReceiver<Interface> clone, void* receiver) {
            static_cast<Interface*>(receiver)->Clone(std::move(clone));
        }, std::move(receiver)));
    }

private:
    RemoteImplBase* const remote_;
};

template <typename Interface> class QueuedChunkedDataPipeGetter final : public Interface {
public:
    using GetSizeCallback = typename Interface::GetSizeCallback;
    explicit QueuedChunkedDataPipeGetter(RemoteImplBase* remote) : remote_(remote) {}

    void GetSize(GetSizeCallback callback) override
    {
        auto reply = base::BindPostTask(remote_->runner(), std::move(callback));
        remote_->EnqueueCall(base::BindOnce([](GetSizeCallback callback, void* receiver) {
            static_cast<Interface*>(receiver)->GetSize(std::move(callback));
        }, std::move(reply)));
    }

    void StartReading(ScopedDataPipeProducerHandle pipe) override
    {
        remote_->EnqueueCall(base::BindOnce([](ScopedDataPipeProducerHandle pipe, void* receiver) {
            static_cast<Interface*>(receiver)->StartReading(std::move(pipe));
        }, std::move(pipe)));
    }

private:
    RemoteImplBase* const remote_;
};

template <typename Interface> class QueuedBlobReaderClient final : public Interface {
public:
    explicit QueuedBlobReaderClient(RemoteImplBase* remote) : remote_(remote) {}

    void OnCalculatedSize(uint64_t total, uint64_t expected) override
    {
        remote_->EnqueueCall(base::BindOnce([](uint64_t total, uint64_t expected, void* receiver) {
            static_cast<Interface*>(receiver)->OnCalculatedSize(total, expected);
        }, total, expected));
    }

    void OnComplete(int32_t status, uint64_t length) override
    {
        remote_->EnqueueCall(base::BindOnce([](int32_t status, uint64_t length, void* receiver) {
            static_cast<Interface*>(receiver)->OnComplete(status, length);
        }, status, length));
    }

private:
    RemoteImplBase* const remote_;
};
} // namespace

template <> LiteQueuedProxy<network::mojom::URLLoaderClient>
CreateLiteQueuedProxy<network::mojom::URLLoaderClient>(RemoteImplBase* remote)
{
    return { new QueuedURLLoaderClient(remote), [](network::mojom::URLLoaderClient* proxy) { delete proxy; } };
}

template <> LiteQueuedProxy<network::mojom::DataPipeGetter>
CreateLiteQueuedProxy<network::mojom::DataPipeGetter>(RemoteImplBase* remote)
{
    return { new QueuedDataPipeGetter<network::mojom::DataPipeGetter>(remote), [](network::mojom::DataPipeGetter* proxy) { delete proxy; } };
}

template <> LiteQueuedProxy<network::mojom::blink::DataPipeGetter>
CreateLiteQueuedProxy<network::mojom::blink::DataPipeGetter>(RemoteImplBase* remote)
{
    return { new QueuedDataPipeGetter<network::mojom::blink::DataPipeGetter>(remote), [](network::mojom::blink::DataPipeGetter* proxy) { delete proxy; } };
}

template <> LiteQueuedProxy<network::mojom::ChunkedDataPipeGetter>
CreateLiteQueuedProxy<network::mojom::ChunkedDataPipeGetter>(RemoteImplBase* remote)
{
    return { new QueuedChunkedDataPipeGetter<network::mojom::ChunkedDataPipeGetter>(remote), [](network::mojom::ChunkedDataPipeGetter* proxy) { delete proxy; } };
}

template <> LiteQueuedProxy<network::mojom::blink::ChunkedDataPipeGetter>
CreateLiteQueuedProxy<network::mojom::blink::ChunkedDataPipeGetter>(RemoteImplBase* remote)
{
    return { new QueuedChunkedDataPipeGetter<network::mojom::blink::ChunkedDataPipeGetter>(remote), [](network::mojom::blink::ChunkedDataPipeGetter* proxy) { delete proxy; } };
}

template <> LiteQueuedProxy<blink::mojom::BlobReaderClient>
CreateLiteQueuedProxy<blink::mojom::BlobReaderClient>(RemoteImplBase* remote)
{
    return { new QueuedBlobReaderClient<blink::mojom::BlobReaderClient>(remote), [](blink::mojom::BlobReaderClient* proxy) { delete proxy; } };
}

template <> LiteQueuedProxy<blink::mojom::blink::BlobReaderClient>
CreateLiteQueuedProxy<blink::mojom::blink::BlobReaderClient>(RemoteImplBase* remote)
{
    return { new QueuedBlobReaderClient<blink::mojom::blink::BlobReaderClient>(remote), [](blink::mojom::blink::BlobReaderClient* proxy) { delete proxy; } };
}
} // namespace mojo::internal
