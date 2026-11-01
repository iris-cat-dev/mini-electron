#ifndef MINI_ELECTRON_WINDOWS_NATIVE_CURSOR_H_
#define MINI_ELECTRON_WINDOWS_NATIVE_CURSOR_H_

#include <windows.h>
#include <cstdint>
#include "ui/base/cursor/cursor.h"

namespace mini_electron::win {
// UI-sequence cache. Shared stock handles are never destroyed; image handles are owned.
class NativeCursor {
public:
    NativeCursor() = default;
    ~NativeCursor();
    NativeCursor(const NativeCursor&) = delete;
    NativeCursor& operator=(const NativeCursor&) = delete;
    void update(const ui::Cursor& cursor, UINT dpi);
    void apply(const ui::Cursor& cursor, UINT dpi);
private:
    void replace(HCURSOR handle, bool owned);
    HCURSOR m_handle = nullptr;
    bool m_owned = false;
    bool m_initialized = false;
    ui::mojom::CursorType m_type = ui::mojom::CursorType::kNull;
    uint32_t m_generation = 0;
    gfx::Point m_hotspot;
    float m_imageScale = 1.f;
    UINT m_dpi = 0;
};
}
#endif
