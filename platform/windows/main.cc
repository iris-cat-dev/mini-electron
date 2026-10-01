#include "runtime/electron/common/init_gdi_plus.h"
#include "runtime/electron/common/atom_command_line.h"
#include "runtime/electron/common/node_thread.h"
#include "runtime/electron/common/node_register_help.h"
#include "base/command_line.h"
#include "base/process/launch.h"

#include <cstdlib>
#include <ole2.h>
#include <windows.h>

extern int wmain(int argc, wchar_t* argv[]);

namespace {

bool isEnvironmentVariableSet(const char* name)
{
    return std::getenv(name) != nullptr;
}

int runAsNode()
{
    // A GUI-subsystem executable is not automatically attached to its parent's
    // console. Preserve inherited pipes, or attach stdout/stderr to the parent
    // console when they are otherwise unavailable, as Electron does.
    if (!isEnvironmentVariableSet("ELECTRON_NO_ATTACH_CONSOLE"))
        base::RouteStdioToConsole(false);

    // The wide CRT argv is already parsed according to Windows quoting rules.
    // Reuse Node's Windows entrypoint so its UTF-8 conversion, platform check,
    // argv semantics, and exit code remain identical to the standalone binary.
    return ::wmain(__argc, __wargv);
}

} // namespace

bool g_isElectronMode = false;

namespace atom {

#define NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_REG_IN_MAIN(fn) \
    fn(electron_browser_web_contents)                            \
    fn(electron_browser_app)                                     \
    fn(electron_browser_electron)                                \
    fn(electron_browser_browserwindow)                           \
    fn(electron_browser_menu)                                    \
    fn(electron_browser_dialog)                                  \
    fn(electron_browser_protocol)                                \
    fn(electron_browser_tray)                                    \
    fn(electron_renderer_ipc)                                    \
    fn(electron_common_v8_util)                                  \
    fn(electron_common_shell)                                    \
    fn(electron_common_original_fs)                              \
    fn(electron_common_screen)                                   \
    fn(electron_renerer_webframe)                                \
    fn(electron_renderer_contextbridge)                          \
    fn(electron_common_intl_collator)                            \
    fn(electron_common_asar)                                     \
    fn(electron_common_nativeImage)                              \
    fn(electron_common_clipboard)                                \
    fn(electron_common_features)                                 \
    fn(electron_browser_browserview)                             \
    fn(electron_browser_session)                                 \
    fn(electron_browser_webrequest)                              \
    fn(electron_browser_downloaditem)                            \
    fn(electron_browser_utility_process)                         \
    fn(electron_browser_parent_port)                             \
    fn(electron_browser_web_frame_main)                          \
    fn(electron_browser_commandline)                             \
    fn(electron_browser_message_port)                            \
    fn(electron_browser_safe_storage)                            \
    fn(electron_browser_powermonitor)

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_REG_IN_MAIN(NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_DECLARE_IN_MAIN)

static void registerNodeModules()
{
    NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_REG_IN_MAIN(NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_DEFINDE_IN_MAIN)
}

} // namespace atom

namespace content {
void initV8Data();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // Both modes use the same V8 snapshot; Node mode must not initialize the host.
    if (isEnvironmentVariableSet("ELECTRON_RUN_AS_NODE")) {
        atom::_register_electron_common_asar();
        content::initV8Data();
        return runAsNode();
    }
    g_isElectronMode = true;
    ::OleInitialize(nullptr);
    atom::initGDIPlusClsids();
    atom::AtomCommandLine::initAW();
    base::CommandLine::Init(0, nullptr);
    atom::registerNodeModules();
    content::initV8Data();

    [[maybe_unused]] atom::NodeArgc* node = atom::runNodeThread();
    return 0;
}
