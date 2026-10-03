// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/browser/api/window_list.h"
#if defined(_WIN32)
#include "runtime/electron/browser/api/app.h"
#endif

#include <algorithm>

namespace atom {

// static
WindowList* WindowList::m_instance = nullptr;

// static
WindowList* WindowList::getInstance()
{
    if (!m_instance)
        m_instance = new WindowList;
    return m_instance;
}

// static
void WindowList::addWindow(WindowInterface* window)
{
    if (!window)
        return;

    // Push |window| on the appropriate list instance.
    WindowVector& windows = getInstance()->m_windows;
    windows.push_back(window);
}

// static
void WindowList::removeWindow(WindowInterface* window)
{
    WindowVector& windows = getInstance()->m_windows;
    windows.erase(std::remove(windows.begin(), windows.end(), window), windows.end());
}

WindowInterface* WindowList::find(int id) const
{
    for (WindowVector::const_iterator it = m_windows.begin(); it != m_windows.end(); ++it) {
        if ((*it)->getId() == id)
            return *it;
    }
    return nullptr;
}

// static
void WindowList::WindowCloseCancelled(WindowInterface* window)
{
#if defined(_WIN32)
    if (App::getInstance())
        App::getInstance()->onWindowCloseCancelled();
#endif
}

// static
void WindowList::closeAllWindows()
{
    const WindowVector windows = getInstance()->m_windows;
    for (WindowInterface* window : windows) {
        const auto& live = getInstance()->m_windows;
        if (std::find(live.begin(), live.end(), window) != live.end() && !window->isClosed())
            window->close();
    }
}

void WindowList::destroyAllWindows()
{
    const WindowVector windows = getInstance()->m_windows;
    for (WindowInterface* window : windows) {
        const auto& live = getInstance()->m_windows;
        if (std::find(live.begin(), live.end(), window) != live.end() && !window->isClosed())
            window->destroy();
    }
}

WindowList::WindowList()
{
}

WindowList::~WindowList()
{
}

} // namespace atom
