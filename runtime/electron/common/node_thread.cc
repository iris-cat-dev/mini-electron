#include "runtime/electron/common/node_thread.h"

#include "runtime/electron/browser/api/app.h"
#include "runtime/electron/browser/api/window_list.h"
#include "runtime/electron/common/atom_command_line.h"
#include "runtime/electron/common/gin_helper/per_isolate_data.h"
#include "runtime/electron/common/node_binding.h"
#include "runtime/electron/node_bindings.h"
#include "runtime/engine/common/thread_call.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/values.h"
#include <algorithm>
#include <cstdio>
#include <optional>

namespace atom {

base::PlatformThreadId g_mainThreadId;
NodeArgc* g_nodeArgc = nullptr;

bool isMainThread()
{
    return g_mainThreadId == base::PlatformThread::CurrentId();
}

static void pumpNode(NodeArgc* state)
{
    if (!state->initType)
        return;
    uv_run(state->uiThreadNodeEnv.uvLoop, UV_RUN_NOWAIT);
    state->m_nodeMultiIsolatePlatform->FlushForegroundTasks(state->m_isolate);
    state->m_isolate->PerformMicrotaskCheckpoint();
    const int wait = uv_backend_timeout(state->uiThreadNodeEnv.uvLoop);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(FROM_HERE,
        base::BindOnce(pumpNode, base::Unretained(state)), base::Milliseconds(wait < 0 ? 10 : std::clamp(wait, 1, 10)));
}

int runNodeMain()
{
    const std::vector<std::string> args = AtomCommandLine::argv();
    const auto initialization = node::InitializeOncePerProcess(
        { args.front(), "--no-experimental-detect-module" },
        node::ProcessInitializationFlags::kEnableStdioInheritance);
    if (!initialization)
        return 1;
    for (const auto& error : initialization->errors())
        std::fprintf(stderr, "mini-electron: %s\n", error.c_str());
    if (initialization->early_return())
        return initialization->exit_code();

    NodeArgc state;
    g_nodeArgc = &state;
    state.m_nodeMultiIsolatePlatform = initialization->platform();
    g_mainThreadId = base::PlatformThread::CurrentId();
    base::PlatformThread::SetName("mini-electron-main");
    std::vector<std::string> errors;
    auto setup = node::CommonEnvironmentSetup::Create(initialization->platform(),
        &errors, args, initialization->exec_args());
    if (!setup) {
        for (const auto& error : errors)
            std::fprintf(stderr, "mini-electron: %s\n", error.c_str());
        g_nodeArgc = nullptr;
        node::TearDownOncePerProcess();
        return 1;
    }

    std::optional<int> processExit;
    int exitCode = 1;
    {
        v8::Isolate* isolate = setup->isolate();
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolateScope(isolate);
        v8::HandleScope handles(isolate);
        v8::Context::Scope contextScope(setup->context());
        gin_helper::PerIsolateData wrapperData(isolate, setup->array_buffer_allocator().get());
        NodeBindings bindings(true);
        state.m_nodeBinding = &bindings;
        state.m_isolate = isolate;
        state.uiThreadNodeEnv.env = setup->env();
        state.uiThreadNodeEnv.uvLoop = setup->event_loop();
        state.uiThreadNodeEnv.v8platform = initialization->platform();
        bindings.m_processObjInfo.isBrowserProcess = true;
        bindings.bindFunction(isolate, nodeGetEnvironmentProcessObject(setup->env()));
        patchProcessObject(nodeGetEnvironmentProcessObject(setup->env()));
        bindEngineConsoleLog(setup->context());
        nodeEnvironmentElectronPostEarlyInitialization(setup->env());
        node::SetProcessExitHandler(setup->env(), [&](node::Environment* env, int code) {
            processExit = code;
            state.initType = false;
            WindowList::destroyAllWindows();
            content::ThreadCall::exitUiThreadMessageLoop();
            node::Stop(env);
        });
        const std::string script = base::WideToUTF8(getResourcesPath(L"browser\\init.js"));
        std::string quoted;
        base::JSONWriter::Write(base::Value(script), &quoted);
        const bool loaded = !node::LoadEnvironment(setup->env(),
            "const filename = " + quoted
                + "; require('module').createRequire(filename)(filename);").IsEmpty();
        if (loaded && !processExit) {
            state.initType = true;
            pumpNode(&state);
            content::ThreadCall::runUiThreadMessageLoop(nullptr, nullptr, nullptr);
        } else if (!processExit) {
            std::fputs("mini-electron: main-process bootstrap failed\n", stderr);
        }
        state.initType = false;
        WindowList::destroyAllWindows();
        exitCode = processExit.value_or(loaded ? App::getExitCode() : 1);
        if (!processExit)
            node::EmitProcessExit(setup->env());
        node::Stop(setup->env(), node::StopFlags::kDoNotTerminateIsolate);
        state.uiThreadNodeEnv.env = nullptr;
        state.m_nodeBinding = nullptr;
    }
    setup.reset();
    g_nodeArgc = nullptr;
    node::TearDownOncePerProcess();
    return exitCode;
}

node::Environment* nodeGetEnvironment(NodeArgc* state)
{
    return state ? state->uiThreadNodeEnv.env : nullptr;
}

} // namespace atom
