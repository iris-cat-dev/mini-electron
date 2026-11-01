#include "src/platform/windows/native_cursor.h"
#include "src/platform/windows/resources/cursor_resources.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "third_party/skia/include/core/SkImage.h"
#include <algorithm>
#include <cmath>

namespace mini_electron::win {
namespace {
HCURSOR standardCursor(ui::mojom::CursorType type)
{
    using T = ui::mojom::CursorType;
    LPCWSTR stock = IDC_ARROW;
    int resource = 0;
    switch (type) {
    case T::kNull: case T::kPointer: case T::kContextMenu: break;
    case T::kCross: stock = IDC_CROSS; break;
    case T::kHand: stock = IDC_HAND; break;
    case T::kIBeam: stock = IDC_IBEAM; break;
    case T::kWait: stock = IDC_WAIT; break;
    case T::kHelp: stock = IDC_HELP; break;
    case T::kProgress: stock = IDC_APPSTARTING; break;
    case T::kEastResize: case T::kWestResize: case T::kEastWestResize: stock = IDC_SIZEWE; break;
    case T::kNorthResize: case T::kSouthResize: case T::kNorthSouthResize: stock = IDC_SIZENS; break;
    case T::kNorthEastResize: case T::kSouthWestResize: case T::kNorthEastSouthWestResize: stock = IDC_SIZENESW; break;
    case T::kNorthWestResize: case T::kSouthEastResize: case T::kNorthWestSouthEastResize: stock = IDC_SIZENWSE; break;
    case T::kMove: case T::kDndMove: stock = IDC_SIZEALL; break;
    case T::kNoDrop: case T::kNotAllowed: case T::kDndNone:
    case T::kEastWestNoResize: case T::kNorthSouthNoResize:
    case T::kNorthEastSouthWestNoResize: case T::kNorthWestSouthEastNoResize: stock = IDC_NO; break;
    case T::kColumnResize: resource = MINI_CURSOR_COL_RESIZE; break;
    case T::kRowResize: resource = MINI_CURSOR_ROW_RESIZE; break;
    case T::kMiddlePanning: resource = MINI_CURSOR_PAN_MIDDLE; break;
    case T::kMiddlePanningVertical: resource = MINI_CURSOR_PAN_MIDDLE_VERTICAL; break;
    case T::kMiddlePanningHorizontal: resource = MINI_CURSOR_PAN_MIDDLE_HORIZONTAL; break;
    case T::kEastPanning: resource = MINI_CURSOR_PAN_EAST; break;
    case T::kNorthPanning: resource = MINI_CURSOR_PAN_NORTH; break;
    case T::kNorthEastPanning: resource = MINI_CURSOR_PAN_NORTH_EAST; break;
    case T::kNorthWestPanning: resource = MINI_CURSOR_PAN_NORTH_WEST; break;
    case T::kSouthPanning: resource = MINI_CURSOR_PAN_SOUTH; break;
    case T::kSouthEastPanning: resource = MINI_CURSOR_PAN_SOUTH_EAST; break;
    case T::kSouthWestPanning: resource = MINI_CURSOR_PAN_SOUTH_WEST; break;
    case T::kWestPanning: resource = MINI_CURSOR_PAN_WEST; break;
    case T::kVerticalText: resource = MINI_CURSOR_VERTICAL_TEXT; break;
    case T::kCell: resource = MINI_CURSOR_CELL; break;
    case T::kAlias: case T::kDndLink: resource = MINI_CURSOR_ALIASB; break;
    case T::kCopy: case T::kDndCopy: resource = MINI_CURSOR_COPY; break;
    case T::kZoomIn: resource = MINI_CURSOR_ZOOM_IN; break;
    case T::kZoomOut: resource = MINI_CURSOR_ZOOM_OUT; break;
    case T::kGrab: resource = MINI_CURSOR_HAND_GRAB; break;
    case T::kGrabbing: resource = MINI_CURSOR_HAND_GRABBING; break;
    case T::kNone: return nullptr;
    case T::kCustom: return nullptr;
    }
    return resource ? ::LoadCursorW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource))
                    : ::LoadCursorW(nullptr, stock);
}

HCURSOR imageCursor(const ui::Cursor& cursor, UINT dpi)
{
    const SkBitmap& bitmap = cursor.custom_bitmap();
    const double scale = dpi / (96.0 * cursor.image_scale_factor());
    if (bitmap.drawsNothing() || !std::isfinite(scale) || scale <= 0)
        return nullptr;
    const double scaledWidth = std::round(bitmap.width() * scale);
    const double scaledHeight = std::round(bitmap.height() * scale);
    if (scaledWidth < 1 || scaledHeight < 1 || scaledWidth > 32767 || scaledHeight > 32767)
        return nullptr;
    const int width = static_cast<int>(scaledWidth);
    const int height = static_cast<int>(scaledHeight);
    SkBitmap scaled;
    const SkBitmap* source = &bitmap;
    if (width != bitmap.width() || height != bitmap.height()) {
        if (!scaled.tryAllocN32Pixels(width, height))
            return nullptr;
        scaled.eraseColor(SK_ColorTRANSPARENT);
        SkCanvas canvas(scaled);
        canvas.scale(static_cast<float>(width) / bitmap.width(), static_cast<float>(height) / bitmap.height());
        canvas.drawImage(bitmap.asImage(), 0, 0, SkSamplingOptions(SkFilterMode::kLinear));
        source = &scaled;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP color = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!color)
        return nullptr;
    const bool copied = source->readPixels(SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType), pixels, static_cast<size_t>(width) * 4, 0, 0);
    HBITMAP mask = copied ? ::CreateBitmap(width, height, 1, 1, nullptr) : nullptr;
    HCURSOR handle = nullptr;
    if (mask) {
        // Alpha supplies transparency; the monochrome AND mask must be clear.
        HDC dc = ::CreateCompatibleDC(nullptr);
        if (dc) {
            HGDIOBJ previous = ::SelectObject(dc, mask);
            ::PatBlt(dc, 0, 0, width, height, BLACKNESS);
            ::SelectObject(dc, previous);
            ::DeleteDC(dc);
            ICONINFO icon = {};
            icon.xHotspot = static_cast<DWORD>(std::clamp(std::round(cursor.custom_hotspot().x() * scale), 0.0, width - 1.0));
            icon.yHotspot = static_cast<DWORD>(std::clamp(std::round(cursor.custom_hotspot().y() * scale), 0.0, height - 1.0));
            icon.hbmColor = color;
            icon.hbmMask = mask;
            handle = static_cast<HCURSOR>(::CreateIconIndirect(&icon));
        }
        ::DeleteObject(mask);
    }
    ::DeleteObject(color);
    return handle;
}
}

NativeCursor::~NativeCursor() { replace(nullptr, false); }

void NativeCursor::replace(HCURSOR handle, bool owned)
{
    if (m_owned && m_handle) {
        if (::GetCursor() == m_handle)
            ::SetCursor(handle ? handle : ::LoadCursorW(nullptr, IDC_ARROW));
        ::DestroyCursor(m_handle);
    }
    m_handle = handle;
    m_owned = owned;
}

void NativeCursor::update(const ui::Cursor& cursor, UINT dpi)
{
    using T = ui::mojom::CursorType;
    const bool custom = cursor.type() == T::kCustom;
    const uint32_t generation = custom ? cursor.custom_bitmap().getGenerationID() : 0;
    const gfx::Point hotspot = custom ? cursor.custom_hotspot() : gfx::Point();
    const float imageScale = custom ? cursor.image_scale_factor() : 1.f;
    if (m_initialized && m_type == cursor.type() && (!custom ||
        (m_generation == generation && m_hotspot == hotspot && m_imageScale == imageScale && m_dpi == dpi)))
        return;
    HCURSOR handle = custom ? imageCursor(cursor, dpi) : standardCursor(cursor.type());
    // A failed image/resource load is not the intentional hidden cursor.
    if (!handle && cursor.type() != T::kNone)
        handle = ::LoadCursorW(nullptr, IDC_ARROW);
    replace(handle, custom && handle && handle != ::LoadCursorW(nullptr, IDC_ARROW));
    m_initialized = true;
    m_type = cursor.type();
    m_generation = generation;
    m_hotspot = hotspot;
    m_imageScale = imageScale;
    m_dpi = dpi;
}

void NativeCursor::apply(const ui::Cursor& cursor, UINT dpi)
{
    update(cursor, dpi);
    ::SetCursor(m_handle);
}
}
