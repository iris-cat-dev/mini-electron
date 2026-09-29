#include <memory>

#include "v8/include/v8.h"

bool g_isElectronMode = false;

std::shared_ptr<v8::TaskRunner> nodePlatformGetForegroundTaskRunner(v8::Isolate* isolate)
{
    return nullptr;
}

bool nodePlatformIdleTasksEnabled(v8::Isolate* isolate)
{
    return true;
}
