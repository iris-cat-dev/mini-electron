
#include "runtime/engine/common/thread_call.h"

#include "runtime/engine/browser/shared_timer_win.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/engine/common/live_id_detect.h"
#include "base/run_loop.h"
#include "v8/include/libplatform/libplatform.h"
#ifdef _WIN32
#include "third_party/libuv/include/uv.h"
#endif // _WIN32
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif
#include "base/task/sequenced_task_runner.h"
#include "v8.h"

base::RunLoop* g_mainThreadRunLoop = nullptr;

namespace v8 {
class Isolate;
}

#if defined(__APPLE__)
namespace {
void RunUiClosureAndDelete(void* context)
{
    std::unique_ptr<std::function<void(void)>> closure(
        static_cast<std::function<void(void)>*>(context));
    (*closure)();
}

void RunUiClosure(void* context)
{
    (*static_cast<std::function<void(void)>*>(context))();
}
} // namespace
#endif
namespace content {

ThreadCall* ThreadCall::m_inst = nullptr;
uv_loop_t* ThreadCall::m_uiUvLoop = nullptr;
uv_loop_t* ThreadCall::m_blinkUvLoop = nullptr;
v8::Platform* ThreadCall::m_UiV8Platform = nullptr;

ThreadCall::ThreadCall()
{
}

void ThreadCall::init(const mini_electron_settings* settings)
{
    m_inst = new ThreadCall();
    g_mainThreadRunLoop = new base::RunLoop();
    m_inst->m_uiThreadId = ::GetCurrentThreadId();
    m_inst->m_uiThreadTask = base::SingleThreadTaskRunner::GetCurrentDefault();
}

void ThreadCall::initializeWebKit()
{
#ifdef _WIN32
    m_blinkUvLoop = new uv_loop_t();
    uv_loop_init(m_blinkUvLoop);
#endif
}

void ThreadCall::callBlinkThreadAsyncWithValid(const TraceLocation& caller, mini_electron_web_view webviewHandle, std::function<void(WebViewHost* webview)>&& closure)
{
    int64_t id = (int64_t)webviewHandle;
    std::function<void(WebViewHost * webview)>* closureDummy = new std::function<void(WebViewHost * webview)>(std::move(closure));

    callBlinkThreadAsync(caller, [id, closureDummy] {
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(id);
        if (webview)
            (*closureDummy)(webview);
        delete closureDummy;
    });
}

void ThreadCall::callBlinkThreadAsync(const TraceLocation& caller, std::function<void(void)>&& closure)
{
    RenderThreadImpl::get()->getTaskRunner()->PostTask(caller, base::BindOnce([](std::function<void(void)>&& closure) {
        (closure)();
    }, std::move(closure)));
}

void ThreadCall::callBlinkThreadAsyncWithValidDelayed(
    const TraceLocation& caller, mini_electron_web_view webviewHandle, size_t millisecond, std::function<void(WebViewHost* webview)>&& closure)
{
    int64_t id = (int64_t)webviewHandle;
    std::function<void(WebViewHost* webview)>* closureDummy = new std::function<void(WebViewHost* webview)>(std::move(closure));

    RenderThreadImpl::get()->getTaskRunner()->PostDelayedTask(caller, base::BindOnce([](std::function<void(WebViewHost* webview)>* closure, int64_t id) {
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(id);
        if (webview)
            (*closure)(webview);
        delete closure;
    }, closureDummy, id), base::Milliseconds(millisecond));
}

void ThreadCall::callBlinkThreadDelayed(const TraceLocation& caller, std::function<void(void)>&& closure, size_t millisecond)
{
    RenderThreadImpl::get()->getTaskRunner()->PostDelayedTask(caller, base::BindOnce([](std::function<void(void)>&& closure) {
        (closure)();
    }, std::move(closure)), base::Milliseconds(millisecond));
}

void ThreadCall::callUiThreadDelayed(const TraceLocation& caller, std::function<void(void)>&& closure, size_t millisecond)
{
#if defined(__APPLE__)
    auto* task = new std::function<void(void)>(std::move(closure));
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, millisecond * NSEC_PER_MSEC),
        dispatch_get_main_queue(), task, RunUiClosureAndDelete);
#else
    m_inst->m_uiThreadTask->PostDelayedTask(
        caller, base::BindOnce([](std::function<void(void)>&& closure) { (closure)(); }, std::move(closure)), base::Milliseconds(millisecond));
#endif
}

void ThreadCall::callUiThreadAsync(const TraceLocation& caller, std::function<void(void)>&& closure)
{
#if defined(__APPLE__)
    auto* task = new std::function<void(void)>(std::move(closure));
    dispatch_async_f(dispatch_get_main_queue(), task, RunUiClosureAndDelete);
#else
    m_inst->m_uiThreadTask->PostNonNestableTask(caller, base::BindOnce([](std::function<void(void)>&& closure) { (closure)(); }, std::move(closure)));
#endif
}

typedef void (*CoreMainTask)(void* data);
struct TaskAsyncData {
    CoreMainTask call;
    void* data;
    void* dataEx;
    BOOL evt;
    //void* ret;
    DWORD fromThreadId;
    //DWORD toThreadId;
    DWORD destroyThreadId;
    TraceLocation caller;
};

TaskAsyncData* cretaeAsyncData(const TraceLocation& caller, void* dataEx, DWORD destroyThreadId)
{
    TaskAsyncData* asyncData = new TaskAsyncData();
    asyncData->evt = FALSE;
    asyncData->dataEx = dataEx;
    asyncData->fromThreadId = ::GetCurrentThreadId();
    //asyncData->toThreadId = 0;
    asyncData->destroyThreadId = destroyThreadId;
    asyncData->caller = caller;

    return asyncData;
}

void ThreadCall::callThreadSync(const TraceLocation& caller, std::function<void(void)>&& closure, scoped_refptr<base::SingleThreadTaskRunner> runner)
{
    TaskAsyncData* asyncData = cretaeAsyncData(caller, &closure, ::GetCurrentThreadId());

    runner->PostTask(caller, base::BindOnce([](
        std::function<void(void)>&& closure, TaskAsyncData* asyncData) {
            (closure)();
        asyncData->evt = TRUE;
    }, std::move(closure), base::Unretained(asyncData)));

    if (!waitForCallThreadAsync(asyncData)) {
        runner->PostTask(caller, base::BindOnce([](
            std::function<void(void)>&& closure, TaskAsyncData* asyncData) {
                (closure)();
                asyncData->evt = TRUE;
            }, std::move(closure), base::Unretained(asyncData)));
        waitForCallThreadAsync(asyncData);
    }
    delete asyncData;
}

void ThreadCall::callUiThreadSync(const TraceLocation& caller, std::function<void(void)>&& closure)
{
    if (isUiThread()) {
        closure();
        return;
    }
#if defined(__APPLE__)
    dispatch_sync_f(dispatch_get_main_queue(), &closure, RunUiClosure);
#else
    callThreadSync(caller, std::move(closure), m_inst->m_uiThreadTask);
#endif
}

void ThreadCall::callBlinkThreadSync(const TraceLocation& caller, std::function<void(void)>&& closure)
{
    if (isBlinkThread()) {
        closure();
        return;
    }
    callThreadSync(caller, std::move(closure), RenderThreadImpl::get()->getTaskRunner());
}

bool ThreadCall::waitForCallThreadAsync(TaskAsyncData* asyncData)
{
    bool ok = false;
    bool firstPost = false;

    int count = 0;
    while (!asyncData->evt) {
        ::Sleep(100);

#ifdef _WIN32
        // ��npapi�����ʱ��createwebview�����ȣ�Ȼ���������ֿ��ܻᷢ��Ϣ��npapi���ڣ��������
        MSG msg;
        if (::PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) != FALSE) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (!firstPost)
            ::PostThreadMessageW(::GetCurrentThreadId(), WM_NULL, 0, 0);
        firstPost = true;
#else
        count++;
        if (count % 41 == 0) {
            char output[100] = { 0 };
            sprintf(output, "waitForCallThreadAsync: %d\n", count);
            OutputDebugStringA(output);
        }

        if (count > 30) // linux����ʱ���Ī�������ʧ��
            return false;
#endif // _WIN32
    }

    return true;
}

bool ThreadCall::isBlinkThread()
{
    return RenderThreadImpl::get()->isCurrentThread();
}

bool ThreadCall::isUiThread()
{
    if (!m_inst)
        return true;
    return m_inst->m_uiThreadId == ::GetCurrentThreadId();
}

void ThreadCall::wake()
{
}

void ThreadCall::onThreadIdle(uv_loop_t* loop, v8::Platform* platform, v8::Isolate* isolate)
{
#ifdef _WIN32
    bool more = (0 != uv_run(loop, UV_RUN_NOWAIT));
    if (platform) {
        v8::platform::PumpMessageLoop(platform, isolate);
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostNonNestableDelayedTask(FROM_HERE,
        base::BindOnce(onThreadIdle, loop, platform, base::Unretained(isolate)), base::Microseconds(500));
#endif
}

void ThreadCall::runUiThreadMessageLoop(uv_loop_t* loop, v8::Platform* platform, v8::Isolate* isolate)
{
#if defined(OS_WIN)
    content::stopSharedTimer();
#endif
    if (loop) {
        m_uiUvLoop = loop;
        m_UiV8Platform = platform;
        m_inst->m_uiThreadTask->PostNonNestableDelayedTask(
            FROM_HERE, base::BindOnce(onThreadIdle, loop, platform, base::Unretained(isolate)), base::Microseconds(500)); // ��ʱ����ѯ���Ժ�ĳɸ���libuv��ʱ����iocp����
    }

    g_mainThreadRunLoop->Run();
}

void ThreadCall::runBlinkThreadNode(uv_loop_t* loop, v8::Isolate* isolate)
{
    RenderThreadImpl::get()->getTaskRunner()->PostNonNestableDelayedTask(FROM_HERE, 
        base::BindOnce(onThreadIdle, loop, nullptr, base::Unretained(isolate)), base::Microseconds(500));
}

bool ThreadCall::isInitUiThread()
{
    return !!g_mainThreadRunLoop;
}

void ThreadCall::exitUiThreadMessageLoop()
{
    g_mainThreadRunLoop->Quit();
}

void ThreadCall::setThreadIdle(mini_electron_thread_callback callback, void* param1, void* param2)
{
//     common::ThreadCallballInfo* info = nullptr;
//     if (common::ThreadCall::isBlinkThread()) {
//         info = &common::ThreadCall::s_blinkThreadIdleInfo;
//     } else if (common::ThreadCall::isUiThread()) {
//         info = &common::ThreadCall::s_uiThreadIdleInfo;
//     } else
//         return;
// 
//     info->cb = callback;
//     info->param1 = param1;
//     info->param2 = param2;
}

void ThreadCall::setBlinkThreadInited(mini_electron_thread_callback callback, void* param1, void* param2)
{
//     common::ThreadCall::s_blinkThreadInitedInfo.cb = callback;
//     common::ThreadCall::s_blinkThreadInitedInfo.param1 = param1;
//     common::ThreadCall::s_blinkThreadInitedInfo.param2 = param2;
}

}

extern "C" bool ThreadCallIsUiThread()
{
    return content::ThreadCall::isUiThread();
}
