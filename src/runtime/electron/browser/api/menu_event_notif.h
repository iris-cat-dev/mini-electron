
#ifndef browser_api_MenuUitl_h
#define browser_api_MenuUitl_h

#include <windows.h>
#include "v8/include/v8-forward.h"

namespace atom {

class WindowInterface;
class MenuItem;

class MenuEventNotif {
public:
    static void onMenuCommon(UINT uMsg, WPARAM wParam, LPARAM lParam);
    static bool onAccelerator(HMENU menu, UINT virtualKeyCode);
    static bool getNativeMenu(v8::Local<v8::Value> value, HMENU* menu);
    static void onWindowDidCreated(WindowInterface* window);
};

}

#endif