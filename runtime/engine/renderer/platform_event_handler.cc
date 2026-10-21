
#include "runtime/engine/renderer/platform_event_handler.h"
#include <algorithm>
#include <cstdint>
#include <iterator>


#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/engine/browser/render_widget_host_impl.h"
#include "runtime/engine/common/thread_call.h"
#include "third_party/blink/renderer/platform/scheduler/worker/compositor_thread_scheduler_impl.h"

#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/web/web_widget.h"
#include "third_party/blink/public/mojom/input/input_handler.mojom-blink.h"
#include "third_party/blink/public/mojom/page/widget.mojom-blink.h"
#if defined(__APPLE__)
#include "third_party/blink/public/web/web_frame_widget.h"
#include "ui/display/screen_info.h"
#endif
#include "ui/latency/latency_info.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include <windowsx.h>

namespace content {

ULONG verticalScrollLines()
{
    ULONG scrollLines = 3;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &scrollLines, 0))
        scrollLines = 3;
    return scrollLines;
}

ULONG horizontalScrollChars()
{
    ULONG scrollChars = 1;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &scrollChars, 0))
        scrollChars = 1;
    return scrollChars;
}

static bool isWithinDoubleClickBounds(const gfx::Point& first, const gfx::Point& second)
{
#if defined(_WIN32)
    const int halfWidth = std::max(1, ::GetSystemMetrics(SM_CXDOUBLECLK) / 2);
    const int halfHeight = std::max(1, ::GetSystemMetrics(SM_CYDOUBLECLK) / 2);
    return std::abs(first.x() - second.x()) <= halfWidth
        && std::abs(first.y() - second.y()) <= halfHeight;
#else
    return std::abs(first.x() - second.x())
        + std::abs(first.y() - second.y()) < 15;
#endif
}

static void postDragMessageImpl(HWND hWnd, void* param)
{
    ::ReleaseCapture();
    ::PostMessageW(hWnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
}

PlatformEventHandler::PlatformEventHandler(int64_t webviewid, blink::WebWidget* webWidget, blink::WebViewImpl* webViewImpl)
//     : m_checkMouseLeaveTimer(this, &PlatformEventHandler::checkMouseLeave)
//     , m_touchEmulator(webWidget)
{
    m_isDispatchDirectlyToWidget = !webViewImpl;
    m_isDraggableRegionNcHitTest = false;
    m_postMouseLeave = false;
    m_isAlert = false;
    m_webviewid = webviewid;
    m_isDraggableNodeMousedown = false;
    m_isLeftMousedown = false;
    m_lastTimeMouseDown = 0;
    m_lastMouseDownButton = 0;
    m_activeMouseButton = 0;
    m_lastMouseDownClickCount = 0;
    m_activeMouseClickCount = 0;
    m_isValidLastTouchDownPoint = false;
    m_webWidget = webWidget;
    m_webViewImpl = webViewImpl;
    m_enableTouchSimulate = false;
    m_enableSystemTouch = false;
}

PlatformEventHandler::~PlatformEventHandler()
{
    m_checkMouseLeaveTimer.reset();
}

void PlatformEventHandler::destroy()
{
    m_lock.Acquire();
    m_webWidget = nullptr;
    m_webViewImpl = nullptr;
    m_blinkWidgetInputHandler.reset();
    m_hostReceiver.reset();
    m_lock.Release();

    if (m_isDispatchDirectlyToWidget) {
        delete this;
    } else {
        scoped_refptr<base::SingleThreadTaskRunner> runner = blink::ThreadScheduler::CompositorThreadScheduler()->InputTaskRunner();
        runner->PostTask(FROM_HERE, base::BindOnce([](PlatformEventHandler* self) { delete self; }, base::Unretained(this)));
    }
}

bool PlatformEventHandler::isDraggableRegionNcHitTest(HWND hWnd, const gfx::Point& pos, const blink::WebVector<blink::WebDraggableRegion>& draggableRegion)
{
    // ���߳�����£�ֱ����mb�ڲ�������ק�����߳���Ⱦʱ��������ui�̴߳���������blink�߳̿��ˣ��ᵼ����קҲ�ܿ�
    //if (!/*blink::RuntimeEnabledFeatures::updataInOtherThreadEnabled()*/true)
    //    return m_isDraggableNodeMousedown;
    //return false;

    bool draggable = false;
    for (size_t i = 0; i < draggableRegion.size(); ++i) {
        blink::WebDraggableRegion r = draggableRegion[i];
        if (r.bounds.Contains(pos)) {
            if (!r.draggable)
                return false;
            draggable = true;
        }
    }
    return draggable;
}

void PlatformEventHandler::fireWheelEventOnCompositorThread(int x, int y,
    float wheelTicksX, float wheelTicksY, float deltaX, float deltaY,
    int modifiers, ui::ScrollGranularity deltaUnits, bool precise)
{
    if (!m_blinkWidgetInputHandler.is_bound())
        return;
    blink::WebMouseWheelEvent mouseWheelEvent;

    mouseWheelEvent.SetType(blink::mojom::EventType::kMouseWheel);
    mouseWheelEvent.SetPositionInWidget(gfx::PointF(x, y));
    mouseWheelEvent.SetPositionInScreen(gfx::PointF(x, y));
    mouseWheelEvent.SetTimeStamp(base::TimeTicks::Now());

    mouseWheelEvent.delta_x = deltaX;
    mouseWheelEvent.delta_y = deltaY;
    mouseWheelEvent.wheel_ticks_x = wheelTicksX;
    mouseWheelEvent.wheel_ticks_y = wheelTicksY;

    mouseWheelEvent.phase = blink::WebMouseWheelEvent::kPhaseNone;
    mouseWheelEvent.dispatch_type = blink::WebMouseWheelEvent::DispatchType::kBlocking;
    mouseWheelEvent.event_action = blink::WebMouseWheelEvent::EventAction::kPageZoom;
    mouseWheelEvent.delta_units = deltaUnits;
    mouseWheelEvent.SetModifiers(modifiers);
    if (precise)
        mouseWheelEvent.event_action = blink::WebMouseWheelEvent::GetPlatformSpecificDefaultEventAction(mouseWheelEvent);

    std::unique_ptr<blink::WebCoalescedInputEvent> mouseWheelCoalescedEvent
        = std::make_unique<blink::WebCoalescedInputEvent>(mouseWheelEvent, ui::LatencyInfo());
    m_blinkWidgetInputHandler->DispatchNonBlockingEvent(std::move(mouseWheelCoalescedEvent));

    blink::WebGestureEvent webGestureEvent;
    if (precise)
        webGestureEvent.SetModifiers(modifiers);
    webGestureEvent.SetType(blink::mojom::EventType::kGestureScrollBegin);
    webGestureEvent.SetSourceDevice(blink::mojom::GestureDevice::kTouchpad);
    webGestureEvent.SetPositionInWidget(gfx::PointF(x, y));
    webGestureEvent.SetPositionInScreen(gfx::PointF(x, y));
    webGestureEvent.SetTimeStamp(base::TimeTicks::Now());
    webGestureEvent.data.scroll_begin.delta_x_hint = deltaX;
    webGestureEvent.data.scroll_begin.delta_y_hint = deltaY;
    webGestureEvent.data.scroll_begin.delta_hint_units = mouseWheelEvent.delta_units;
    webGestureEvent.data.scroll_begin.target_viewport = false;
    webGestureEvent.data.scroll_begin.inertial_phase = blink::mojom::InertialPhaseState::kNonMomentum;
    webGestureEvent.data.scroll_begin.synthetic = true;
    webGestureEvent.data.scroll_begin.pointer_count = 1;
    m_blinkWidgetInputHandler->DispatchNonBlockingEvent(std::make_unique<blink::WebCoalescedInputEvent>(webGestureEvent, ui::LatencyInfo()));

    webGestureEvent.SetType(blink::mojom::EventType::kGestureScrollUpdate);
    webGestureEvent.data.scroll_update.delta_x = deltaX;
    webGestureEvent.data.scroll_update.delta_y = deltaY;
//     webGestureEvent.data.scroll_update.velocity_x = deltaX;
//     webGestureEvent.data.scroll_update.velocity_y = deltaY;
    webGestureEvent.data.scroll_update.inertial_phase = blink::mojom::InertialPhaseState::kNonMomentum;
    webGestureEvent.data.scroll_update.delta_units = mouseWheelEvent.delta_units;
    m_blinkWidgetInputHandler->DispatchNonBlockingEvent(std::make_unique<blink::WebCoalescedInputEvent>(webGestureEvent, ui::LatencyInfo()));

    webGestureEvent.SetType(blink::mojom::EventType::kGestureScrollEnd);
    webGestureEvent.data.scroll_end.inertial_phase = blink::mojom::InertialPhaseState::kNonMomentum;
    webGestureEvent.data.scroll_end.synthetic = true;
    webGestureEvent.data.scroll_end.generated_by_fling_controller = true;
    webGestureEvent.data.scroll_end.delta_units = mouseWheelEvent.delta_units;
    m_blinkWidgetInputHandler->DispatchNonBlockingEvent(std::make_unique<blink::WebCoalescedInputEvent>(webGestureEvent, ui::LatencyInfo()));
}

void PlatformEventHandler::firePreciseWheelEvent(int x, int y, float deltaX, float deltaY, int modifiers)
{
    if (deltaX == 0 && deltaY == 0)
        return;
    scoped_refptr<base::SingleThreadTaskRunner> runner = blink::ThreadScheduler::CompositorThreadScheduler()->InputTaskRunner();
    if (m_isDispatchDirectlyToWidget)
        runner = RenderThreadImpl::get()->getTaskRunner();
    runner->PostTask(FROM_HERE, base::BindOnce(
        [](PlatformEventHandler* self, int x, int y, float deltaX, float deltaY, int modifiers) {
            base::AutoLock lock(self->m_lock);
            self->fireWheelEventOnCompositorThread(x, y, 0, 0, deltaX, deltaY,
                modifiers, ui::ScrollGranularity::kScrollByPrecisePixel, true);
        },
        base::Unretained(this), x, y, deltaX, deltaY, modifiers));
}

LRESULT PlatformEventHandler::fireWheelEvent(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ::ScreenToClient(hWnd, &point);

    int modifiers = 0;
    if (wParam & MK_SHIFT)
        modifiers |= blink::WebInputEvent::kShiftKey;
    if (wParam & MK_CONTROL)
        modifiers |= blink::WebInputEvent::kControlKey;

    const float wheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam));
    fireWheelEvent(point.x, point.y,
        message == WM_MOUSEHWHEEL ? wheelDelta : 0,
        message == WM_MOUSEWHEEL ? wheelDelta : 0, modifiers);
    return 0;
}

void PlatformEventHandler::fireWheelEvent(int x, int y, float wheelDeltaX,
    float wheelDeltaY, int modifiers)
{
    if (wheelDeltaX == 0 && wheelDeltaY == 0)
        return;
#if defined(_WIN32)
    // Win32 horizontal ticks point right; Blink's content-motion delta is opposite.
    wheelDeltaX = -wheelDeltaX;
#endif


    if (wheelDeltaX == 0 && wheelDeltaY != 0
        && (modifiers & blink::WebInputEvent::kShiftKey)) {
        wheelDeltaX = wheelDeltaY;
        wheelDeltaY = 0;
    }

    const float wheelTicksX = wheelDeltaX / static_cast<float>(WHEEL_DELTA);
    const float wheelTicksY = wheelDeltaY / static_cast<float>(WHEEL_DELTA);
#if defined(__APPLE__)
    // Use logical pixels on macOS so Retina does not halve the wheel step.
    const float pixelsPerLine =
        (80.0f / 3.0f) * m_webViewImpl->MainFrameWidget()->GetOriginalScreenInfo().device_scale_factor;
#else
    static constexpr float pixelsPerLine = 100.0f / 3.0f;
#endif

    const ULONG scrollLines = verticalScrollLines();
    const ui::ScrollGranularity deltaUnits =
        wheelTicksY != 0 && scrollLines == WHEEL_PAGESCROLL
        ? ui::ScrollGranularity::kScrollByPage
        : ui::ScrollGranularity::kScrollByPixel;
    const float deltaX = wheelTicksX * static_cast<float>(horizontalScrollChars()) * pixelsPerLine;
    const float deltaY = wheelTicksY == 0 ? 0
        : deltaUnits == ui::ScrollGranularity::kScrollByPage ? wheelTicksY
        : wheelTicksY * static_cast<float>(scrollLines) * pixelsPerLine;

    CHECK(this);
    scoped_refptr<base::SingleThreadTaskRunner> runner =
        blink::ThreadScheduler::CompositorThreadScheduler()->InputTaskRunner();
    if (m_isDispatchDirectlyToWidget)
        runner = RenderThreadImpl::get()->getTaskRunner();

    runner->PostTask(FROM_HERE,
        base::BindOnce(
            [](PlatformEventHandler* self, int x, int y, float wheelTicksX,
                float wheelTicksY, float deltaX, float deltaY, int modifiers,
                ui::ScrollGranularity deltaUnits) {
                base::AutoLock lock(self->m_lock);
                self->fireWheelEventOnCompositorThread(x, y, wheelTicksX,
                    wheelTicksY, deltaX, deltaY, modifiers, deltaUnits);
            },
            base::Unretained(this), x, y, wheelTicksX, wheelTicksY, deltaX,
            deltaY, modifiers, deltaUnits));
}

void PlatformEventHandler::buildMousePosInfo(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, bool* handle, gfx::Point* pos, gfx::Point* globalPos)
{
    if (WM_MOUSELEAVE == message) {
        *handle = true;
        m_postMouseLeave = true;

        POINT ptCursor;
        ::GetCursorPos(&ptCursor);
        *globalPos = gfx::Point(ptCursor.x, ptCursor.y);
        ::ScreenToClient(hWnd, &ptCursor);

        if (ptCursor.x < 2)
            ptCursor.x = -1;
        else if (ptCursor.x > 10)
            ptCursor.x += 2;

        if (ptCursor.y < 2)
            ptCursor.y = -1;
        else if (ptCursor.y > 10)
            ptCursor.y += 2;

        *pos = gfx::Point(ptCursor.x, ptCursor.y);

        lParam = MAKELPARAM(ptCursor.x, ptCursor.y);
    } else {
        m_postMouseLeave = false;
        pos->set_x(/*m_offset.x() +*/ ((int)(short)LOWORD(lParam)));
        pos->set_y(/*m_offset.y() +*/ ((int)(short)HIWORD(lParam)));

        POINT widgetPoint = { pos->x(), pos->y() };
#if defined(WIN32)
        ::ClientToScreen(hWnd, &widgetPoint);
#endif
        *globalPos = gfx::Point(widgetPoint.x, widgetPoint.y);
    }
}

// �������뷨�£���������ww��������������Ϣ��
// WM_IME_STARTCOMPOSITION
// WM_IME_COMPOSITION : 77
// WM_IME_COMPOSITION : 77
// WM_IME_COMPOSITION : 5916
// WM_IME_CHAR : 5916
// WM_IME_CHAR : 7f51
// WM_IME_ENDCOMPOSITION
// ˵����������'w'��WM_IME_COMPOSITION��Ϣ��Ȼ��������WM_IME_CHAR������Ϣ��UTF16�������������WM_IME_ENDCOMPOSITION
void PlatformEventHandler::fireImeComposition(ImeCompositioHandleType type, WCHAR c)
{
    gfx::Range range((size_t)(0xffffffff), (size_t)(0xffffffff));

    m_lock.Acquire();
    if (!(m_blinkWidgetInputHandler.is_bound() && m_blinkWidgetInputHandler.is_connected())) {
        m_lock.Release();
        return;
    }

    if (kImeCompositioHandleTypeStart == type) {
        m_imeTextCache.clear();
    } else if (kImeCompositioHandleTypeCom == type && c != 0) {
        ui::ImeTextSpan textSpan(
            ui::ImeTextSpan::Type::kComposition,
            /*size_t start_offset =*/ 0,
            /*size_t end_offset =*/ 1,
            ui::ImeTextSpan::Thickness::kThin,
            ui::ImeTextSpan::UnderlineStyle::kSolid,
            SK_ColorTRANSPARENT,
            SK_ColorTRANSPARENT,
            std::vector<std::string>(),
            SK_ColorTRANSPARENT);
        WTF::Vector<::ui::ImeTextSpan> imeTextSpans;
        imeTextSpans.push_back(textSpan);

        int32_t selectionStart = 1;
        int32_t selectionEnd = 1;

        WTF::String imeText(base::span<const UChar>((const UChar*)(&c), (size_t)1));
        m_blinkWidgetInputHandler->ImeSetComposition(imeText, imeTextSpans, range, selectionStart, selectionEnd, base::BindOnce([] {}));
    } else if (kImeCompositioHandleTypeEnd == type && !m_imeTextCache.empty()) {
        WTF::Vector<::ui::ImeTextSpan> imeTextSpans2;

        WTF::String imeText(base::span<const UChar>((const UChar*)m_imeTextCache.data(), m_imeTextCache.size()));
        m_blinkWidgetInputHandler->ImeCommitText(imeText, imeTextSpans2, range, 0, base::BindOnce([] {}));
        m_imeTextCache.clear();
    } else if (kImeCompositioHandleTypeChar == type) {
        m_imeTextCache.push_back(c);
    }

    m_lock.Release();
}

void PlatformEventHandler::fireImeText(const std::u16string& text, bool committed)
{
    gfx::Range range((size_t)(0xffffffff), (size_t)(0xffffffff));

    m_lock.Acquire();
    if (!(m_blinkWidgetInputHandler.is_bound() && m_blinkWidgetInputHandler.is_connected())) {
        m_lock.Release();
        return;
    }

    WTF::String imeText(base::span<const UChar>(
        reinterpret_cast<const UChar*>(text.data()), text.size()));
    if (committed) {
        if (!text.empty()) {
            WTF::Vector<::ui::ImeTextSpan> imeTextSpans;
            m_blinkWidgetInputHandler->ImeCommitText(
                imeText, imeTextSpans, range, 0, base::BindOnce([] {}));
        }
        m_imeTextCache.clear();
    } else {
        WTF::Vector<::ui::ImeTextSpan> imeTextSpans;
        int32_t selectionEnd = static_cast<int32_t>(text.size());
        if (!text.empty()) {
            imeTextSpans.push_back(ui::ImeTextSpan(
                ui::ImeTextSpan::Type::kComposition,
                0,
                static_cast<size_t>(selectionEnd),
                ui::ImeTextSpan::Thickness::kThin,
                ui::ImeTextSpan::UnderlineStyle::kSolid,
                SK_ColorTRANSPARENT,
                SK_ColorTRANSPARENT,
                std::vector<std::string>(),
                SK_ColorTRANSPARENT));
        }
        m_blinkWidgetInputHandler->ImeSetComposition(
            imeText, imeTextSpans, range, selectionEnd, selectionEnd,
            base::BindOnce([] {}));
        m_imeTextCache.assign(text.begin(), text.end());
    }

    m_lock.Release();
}

void PlatformEventHandler::fireInputEventToCompositingThread(const blink::WebInputEvent& evt)
{
    if (evt.GetType() == blink::WebInputEvent::Type::kKeyDown)
        OutputDebugStringA("kKeyDown\n");

    scoped_refptr<base::SingleThreadTaskRunner> runner = blink::ThreadScheduler::CompositorThreadScheduler()->InputTaskRunner();
    std::unique_ptr<blink::WebInputEvent> newEvt = evt.Clone();

    if (m_isDispatchDirectlyToWidget || runner->BelongsToCurrentThread()) {
        std::unique_ptr<blink::WebCoalescedInputEvent> event = std::make_unique<blink::WebCoalescedInputEvent>(std::move(evt), ui::LatencyInfo());
        m_lock.Acquire();
        if (m_blinkWidgetInputHandler.is_bound() && m_blinkWidgetInputHandler.is_connected())
            m_blinkWidgetInputHandler->DispatchNonBlockingEvent(std::move(event)); // TODO: û���ǿ���̱����������
        m_lock.Release();
    } else {
        runner->PostTask(FROM_HERE,
            base::BindOnce(
                [](PlatformEventHandler* self, std::unique_ptr<blink::WebInputEvent> evt) {
                    std::unique_ptr<blink::WebCoalescedInputEvent> event = std::make_unique<blink::WebCoalescedInputEvent>(std::move(evt), ui::LatencyInfo());
                    self->m_lock.Acquire();
                    if (self->m_blinkWidgetInputHandler.is_bound() && self->m_blinkWidgetInputHandler.is_connected())
                        self->m_blinkWidgetInputHandler->DispatchNonBlockingEvent(std::move(event)); // TODO: û���ǿ���̱����������
                    self->m_lock.Release();
                },
                base::Unretained(this), std::move(newEvt)));
    }
}

static void postDragMessage(HWND hWnd)
{
    HWND hRootWnd = hWnd;
    while (true) {
        hWnd = ::GetParent(hWnd);
        if (!hWnd)
            break;
        hRootWnd = hWnd;
    }

    if (!hRootWnd)
        return;

    if (!/*blink::RuntimeEnabledFeatures::updataInOtherThreadEnabled()*/ true) {
        postDragMessageImpl(hRootWnd, nullptr);
        return;
    }

    //     if (wke::g_wkeUiThreadPostTaskCallback)
    //         wke::g_wkeUiThreadPostTaskCallback(hRootWnd, postDragMessageImpl, nullptr);
}

void PlatformEventHandler::checkMouseLeave()
{
    if (!m_hWnd || !::IsWindow(m_hWnd)) {
        m_checkMouseLeaveTimer->Stop();
        return;
    }

    POINT pt = { 0 };
    ::GetCursorPos(&pt);

    RECT rc;
    ::GetClientRect(m_hWnd, &rc);
    ::ScreenToClient(m_hWnd, &pt);

    if ((!::PtInRect(&rc, pt) || !::IsWindowVisible(m_hWnd))) {
        if (!m_isLeftMousedown) {
            MouseEvtInfo info = { true, false };
            LPARAM lParam = MAKELONG(pt.x, pt.y);

            //             if (m_enableTouchSimulate)
            //                 fireTouchEvent(m_hWnd, WM_MOUSELEAVE, lParam, 0);

            fireMouseEvent(m_hWnd, WM_MOUSELEAVE, 0, lParam, info, nullptr);
        }
        m_checkMouseLeaveTimer->Stop();
    }
}

// �����������ʱ���ղ���up��Ϣ������Ҫģ��һ��
bool PlatformEventHandler::fireMouseUpEventIfNeeded(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, const MouseEvtInfo& info, BOOL* bHandle)
{
    if ((WM_MOUSEMOVE != message && WM_MOUSELEAVE != message) || !m_isLeftMousedown)
        return false;

    if ((wParam & MK_LBUTTON))
        return false;

    fireMouseEvent(hWnd, WM_LBUTTONUP, 0, lParam, info, nullptr);
    if (bHandle)
        *bHandle = true;
    return true;
}

static void setFocusOnBlinkThread(int64_t webviewid, HWND hWnd, blink::WebViewImpl* webViewImpl)
{
    ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewid, [hWnd, webViewImpl](WebViewHost* self) {
        ::SetFocus(hWnd);
        webViewImpl->MainFrameWidget()->SetFocus(true);
        webViewImpl->SetIsActive(true);
    });
}

// ������ִ���ںϳ��߳�
LRESULT PlatformEventHandler::fireMouseEvent(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, const MouseEvtInfo& info, BOOL* bHandle)
{
    m_hWnd = hWnd;
    bool handle = false;

    m_isDraggableRegionNcHitTest = false;
    if (true == m_isAlert)
        return 0;

    bool isValideWindow = ::IsWindow(hWnd) && !info.isWillDestroy;

    if (fireMouseUpEventIfNeeded(hWnd, message, wParam, lParam, info, bHandle))
        return 0;


    gfx::Point pos;
    gfx::Point globalPos;

    buildMousePosInfo(hWnd, message, wParam, lParam, &handle, &pos, &globalPos);

#if defined(WIN32)
    if (WM_MOUSELEAVE != message) {
        if (!m_checkMouseLeaveTimer)
            m_checkMouseLeaveTimer = std::make_unique<base::RepeatingTimer>();
        m_checkMouseLeaveTimer->Stop();
        m_checkMouseLeaveTimer->Start(FROM_HERE, base::Microseconds(200), base::BindRepeating(&PlatformEventHandler::checkMouseLeave, base::Unretained(this)));
    }
#endif

    base::TimeTicks time = base::TimeTicks::Now();

    blink::WebMouseEvent webMouseEvent;
    // Use the originating event's snapshot, not global key state on this thread.
    webMouseEvent.SetModifiers(info.modifiers);
    webMouseEvent.SetPositionInWidget(gfx::PointF(pos.x(), pos.y()));
    webMouseEvent.SetPositionInScreen(gfx::PointF(globalPos.x(), globalPos.y()));
    webMouseEvent.SetTimeStamp(time);
    webMouseEvent.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
    webMouseEvent.movement_x = pos.x() - m_lastPosMouseMove.x();
    webMouseEvent.movement_y = pos.y() - m_lastPosMouseMove.y();
    m_lastPosMouseMove = pos;

    webMouseEvent.click_count = 0;

    if (WM_LBUTTONDOWN == message || WM_MBUTTONDOWN == message
        || WM_RBUTTONDOWN == message || WM_LBUTTONDBLCLK == message
        || WM_MBUTTONDBLCLK == message || WM_RBUTTONDBLCLK == message) {
        handle = true;

        const UINT button = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK
            ? WM_LBUTTONDOWN
            : message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK
                ? WM_MBUTTONDOWN
                : WM_RBUTTONDOWN;
        const int64_t interval = static_cast<int64_t>(::GetDoubleClickTime()) * 1000;
        const int64_t ticks = time.ToInternalValue() - m_lastTimeMouseDown;
        const bool continuesClickSequence = m_lastTimeMouseDown != 0
            && button == m_lastMouseDownButton && ticks >= 0 && ticks <= interval
            && isWithinDoubleClickBounds(m_lastPosMouseDown, pos);
        int clickCount = continuesClickSequence ? m_lastMouseDownClickCount + 1 : 1;
        if ((message == WM_LBUTTONDBLCLK || message == WM_MBUTTONDBLCLK
                || message == WM_RBUTTONDBLCLK)
            && clickCount < 2) {
            clickCount = 2;
        }
        webMouseEvent.click_count = clickCount;
        m_lastTimeMouseDown = time.ToInternalValue();
        m_lastPosMouseDown = pos;
        m_lastMouseDownButton = button;
        m_lastMouseDownClickCount = clickCount;
        m_activeMouseButton = button;
        m_activeMouseClickCount = clickCount;

        if (hWnd && info.isNeedSetFocus) {
            if (m_webViewImpl && isValideWindow && ::GetFocus() != hWnd) {
                setFocusOnBlinkThread(m_webviewid, hWnd, m_webViewImpl);
            }
            //             if (wke::g_enableNativeSetCapture && isValideWindow)
            //                 ::SetCapture(hWnd);
        }
        switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            m_isLeftMousedown = true;
            webMouseEvent.button = blink::WebPointerProperties::Button::kLeft;
            webMouseEvent.SetModifiers(webMouseEvent.GetModifiers() | blink::WebMouseEvent::kLeftButtonDown);
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            webMouseEvent.button = blink::WebPointerProperties::Button::kMiddle;
            webMouseEvent.SetModifiers(webMouseEvent.GetModifiers() | blink::WebMouseEvent::kMiddleButtonDown);
            break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            webMouseEvent.button = blink::WebPointerProperties::Button::kRight;
            webMouseEvent.SetModifiers(webMouseEvent.GetModifiers() | blink::WebMouseEvent::kRightButtonDown);
            break;
        }
        m_isDraggableRegionNcHitTest = false;
        webMouseEvent.SetType(blink::mojom::EventType::kMouseDown);
        fireInputEventToCompositingThread(webMouseEvent);

        m_isDraggableNodeMousedown = false;
    } else if (WM_LBUTTONUP == message || WM_MBUTTONUP == message || WM_RBUTTONUP == message) {
        handle = true;
        const UINT button = message == WM_LBUTTONUP
            ? WM_LBUTTONDOWN
            : message == WM_MBUTTONUP ? WM_MBUTTONDOWN : WM_RBUTTONDOWN;
        webMouseEvent.click_count = button == m_activeMouseButton
            ? m_activeMouseClickCount
            : 1;

        switch (message) {
        case WM_LBUTTONUP:
            m_isLeftMousedown = false;
            webMouseEvent.button = blink::WebPointerProperties::Button::kLeft;
            break;
        case WM_MBUTTONUP:
            webMouseEvent.button = blink::WebPointerProperties::Button::kMiddle;
            break;
        case WM_RBUTTONUP:
            webMouseEvent.button = blink::WebPointerProperties::Button::kRight;
            break;
        }
        if (button == m_activeMouseButton) {
            m_activeMouseButton = 0;
            m_activeMouseClickCount = 0;
        }
        if (isValideWindow)
            ::ReleaseCapture();
        if (info.renderWidgetHost && info.renderWidgetHost->m_frameWidget) {
            info.renderWidgetHost->m_frameWidget->DragSourceSystemDragEnded();
        }
        webMouseEvent.SetType(blink::mojom::EventType::kMouseUp);
        fireInputEventToCompositingThread(webMouseEvent);
    } else if (WM_MOUSEMOVE == message || WM_MOUSELEAVE == message) {
        handle = true;
        if (wParam & MK_LBUTTON) {
            webMouseEvent.button = blink::mojom::Button::kLeft;
            webMouseEvent.SetModifiers(webMouseEvent.GetModifiers() | blink::WebInputEvent::kLeftButtonDown);
        } else if (wParam & MK_MBUTTON) {
            webMouseEvent.button = blink::mojom::Button::kMiddle;
            webMouseEvent.SetModifiers(webMouseEvent.GetModifiers() | blink::WebInputEvent::kMiddleButtonDown);
        } else if (wParam & MK_RBUTTON) {
            webMouseEvent.button = blink::mojom::Button::kRight;
            webMouseEvent.SetModifiers(webMouseEvent.GetModifiers() | blink::WebInputEvent::kRightButtonDown);
        } else {
            CHECK(!m_isLeftMousedown);
            webMouseEvent.button = blink::mojom::Button::kEraser;
        }

        bool b = false;
        switch (message) {
        case WM_MOUSEMOVE:
            // Blink derives DOM enter/over transitions from the actual move.
            webMouseEvent.SetType(blink::mojom::EventType::kMouseMove);

            fireInputEventToCompositingThread(webMouseEvent);
            break;
        case WM_MOUSELEAVE:
            webMouseEvent.SetType(blink::mojom::EventType::kMouseLeave);

            if (info.renderWidgetHost && info.renderWidgetHost->m_frameWidget) {
                info.renderWidgetHost->m_frameWidget->DragSourceSystemDragEnded(
                    //                     gfx::PointF(pos.x(), pos.y()),
                    //                     gfx::PointF(globalPos.x(), globalPos.y()),
                    //                     ::ui::mojom::blink::DragOperation::kNone,
                    //                     blink::mojom::blink::FrameWidget::DragSourceEndedAtCallback()
                );
            }
            fireInputEventToCompositingThread(webMouseEvent);
            break;
        }
    }

    if (bHandle)
        *bHandle = handle;
    return 0;
}

#if defined(_WIN32)
static ui::DomCode domCodeFromWindowsEvent(WPARAM virtualKey, LPARAM keyData,
    HKL keyboardLayout)
{
    const uintptr_t nativeData = static_cast<uintptr_t>(keyData);
    UINT scanCode = static_cast<UINT>((nativeData >> 16) & 0xff);
    if (nativeData & (uintptr_t { 1 } << 24))
        scanCode |= 0xe000;
    if (!scanCode && virtualKey) {
        scanCode = ::MapVirtualKeyExW(static_cast<UINT>(virtualKey),
            MAPVK_VK_TO_VSC_EX, keyboardLayout);
    }

    const ui::DomCode code =
        ui::KeycodeConverter::NativeKeycodeToDomCode(static_cast<int>(scanCode));
    if (code != ui::DomCode::NONE)
        return code;
    return ui::UsLayoutKeyboardCodeToDomCode(
        static_cast<ui::KeyboardCode>(virtualKey));
}


static ui::DomKey domKeyFromWindowsEvent(WPARAM virtualKey, ui::DomCode domCode,
    unsigned int flags, HKL keyboardLayout)
{
    const bool altGraph = (flags & MINI_ELECTRON_ALT_GRAPH) != 0;
    if (domCode == ui::DomCode::ALT_RIGHT && altGraph)
        return ui::DomKey::ALT_GRAPH;
    ui::DomKey domKey;
    ui::KeyboardCode keyCode;
    if (ui::DomCodeToNonPrintableDomKey(domCode, &domKey, &keyCode))
        return domKey;

    BYTE keyboardState[256] = {};
    ::GetKeyboardState(keyboardState);
    auto setPressed = [&keyboardState](int key, bool pressed) {
        keyboardState[key] = static_cast<BYTE>(
            (keyboardState[key] & 0x01) | (pressed ? 0x80 : 0));
    };
    setPressed(VK_SHIFT, (flags & MINI_ELECTRON_SHIFT) != 0);
    setPressed(VK_CONTROL, (flags & MINI_ELECTRON_CONTROL) != 0);
    setPressed(VK_MENU, (flags & MINI_ELECTRON_ALT) != 0);
    keyboardState[VK_CAPITAL] = static_cast<BYTE>(
        (keyboardState[VK_CAPITAL] & 0x80)
        | ((flags & MINI_ELECTRON_CAPS_LOCK) ? 0x01 : 0));
    keyboardState[VK_NUMLOCK] = static_cast<BYTE>(
        (keyboardState[VK_NUMLOCK] & 0x80)
        | ((flags & MINI_ELECTRON_NUM_LOCK) ? 0x01 : 0));

    setPressed(VK_RMENU, altGraph);
    if (!altGraph) {
        setPressed(VK_CONTROL, false);
        setPressed(VK_LCONTROL, false);
        setPressed(VK_RCONTROL, false);
        setPressed(VK_MENU, false);
        setPressed(VK_LMENU, false);
        setPressed(VK_RMENU, false);
    }

    WCHAR characters[4] = {};
    const UINT scanCode = static_cast<UINT>(
        ui::KeycodeConverter::DomCodeToNativeKeycode(domCode)) & 0xff;
    const int characterCount = ::ToUnicodeEx(static_cast<UINT>(virtualKey),
        scanCode, keyboardState, characters,
        static_cast<int>(std::size(characters)),
        4 /* Do not mutate the keyboard's dead-key state. */, keyboardLayout);
    if (characterCount > 0) {
        uint32_t character = characters[0];
        if (characterCount > 1 && characters[0] >= 0xd800
            && characters[0] <= 0xdbff && characters[1] >= 0xdc00
            && characters[1] <= 0xdfff) {
            character = 0x10000
                + ((static_cast<uint32_t>(characters[0]) - 0xd800) << 10)
                + (static_cast<uint32_t>(characters[1]) - 0xdc00);
        }
        return ui::DomKey::FromCharacter(character);
    }
    if (characterCount < 0 && characters[0])
        return ui::DomKey::DeadKeyFromCombiningCharacter(characters[0]);
    const ui::DomCode semanticCode = ui::UsLayoutKeyboardCodeToDomCode(
        static_cast<ui::KeyboardCode>(virtualKey));
    if (ui::DomCodeToNonPrintableDomKey(semanticCode, &domKey, &keyCode))
        return domKey;

    int fallbackFlags = ui::EF_NONE;
    if (flags & MINI_ELECTRON_SHIFT)
        fallbackFlags |= ui::EF_SHIFT_DOWN;
    if (altGraph)
        fallbackFlags |= ui::EF_ALTGR_DOWN;
    if (flags & MINI_ELECTRON_CAPS_LOCK)
        fallbackFlags |= ui::EF_CAPS_LOCK_ON;
    if (flags & MINI_ELECTRON_NUM_LOCK)
        fallbackFlags |= ui::EF_NUM_LOCK_ON;
    if (ui::DomCodeToUsLayoutDomKey(domCode, fallbackFlags, &domKey, &keyCode))
        return domKey;
    return ui::DomKey::UNIDENTIFIED;
}
#endif

blink::WebKeyboardEvent PlatformEventHandler::buildKeyboardEvent(
    blink::WebInputEvent::Type type, UINT message, WPARAM wParam, LPARAM lParam,
    unsigned int flags
#if defined(_WIN32)
    , HKL keyboardLayout
#endif
)
{
    const bool isCharacter = type == blink::WebInputEvent::Type::kChar;
    const WPARAM virtualKey = isCharacter ? 0 : wParam;
#if defined(_WIN32)
    if (!keyboardLayout)
        keyboardLayout = ::GetKeyboardLayout(0);
    const ui::DomCode domCode =
        domCodeFromWindowsEvent(virtualKey, lParam, keyboardLayout);
    const ui::DomKey domKey = isCharacter
        ? ui::DomKey::FromCharacter(static_cast<uint32_t>(wParam))
        : domKeyFromWindowsEvent(virtualKey, domCode, flags, keyboardLayout);
#else
    const ui::DomCode domCode = ui::UsLayoutKeyboardCodeToDomCode(
        static_cast<ui::KeyboardCode>(virtualKey));
    ui::DomKey domKey = isCharacter
        ? ui::DomKey::FromCharacter(static_cast<uint32_t>(wParam))
        : ui::DomKey::UNIDENTIFIED;
    if (!isCharacter) {
        int keyFlags = ui::EF_NONE;
        if (flags & MINI_ELECTRON_SHIFT)
            keyFlags |= ui::EF_SHIFT_DOWN;
        if (flags & MINI_ELECTRON_CAPS_LOCK)
            keyFlags |= ui::EF_CAPS_LOCK_ON;
        if (flags & MINI_ELECTRON_NUM_LOCK)
            keyFlags |= ui::EF_NUM_LOCK_ON;
        ui::KeyboardCode keyCode;
        if (!ui::DomCodeToUsLayoutDomKey(domCode, keyFlags, &domKey, &keyCode))
            domKey = ui::DomKey::UNIDENTIFIED;
    }
#endif

    blink::WebKeyboardEvent keyEvent;
    keyEvent.SetTimeStamp(base::TimeTicks::Now());
    keyEvent.SetType(type);
    keyEvent.windows_key_code =
        static_cast<int>(static_cast<uint16_t>(wParam));
    keyEvent.native_key_code =
        ui::KeycodeConverter::DomCodeToNativeKeycode(domCode);
    keyEvent.dom_code = static_cast<int>(domCode);
    keyEvent.dom_key = static_cast<uint32_t>(domKey);
    keyEvent.is_system_key = !(flags & MINI_ELECTRON_ALT_GRAPH)
        && (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP
#if defined(_WIN32)
            || message == WM_SYSCHAR
#endif
        );

    int modifiers = 0;
    const bool altGraph = (flags & MINI_ELECTRON_ALT_GRAPH) != 0;
    if ((flags & MINI_ELECTRON_CONTROL) && !altGraph)
        modifiers |= blink::WebInputEvent::kControlKey;
    if (flags & MINI_ELECTRON_SHIFT)
        modifiers |= blink::WebInputEvent::kShiftKey;
    if (flags & MINI_ELECTRON_META)
        modifiers |= blink::WebInputEvent::kMetaKey;
    if ((flags & MINI_ELECTRON_ALT) && !altGraph)
        modifiers |= blink::WebInputEvent::kAltKey;
    if (altGraph)
        modifiers |= blink::WebInputEvent::kAltGrKey;
    // KF_REPEAT is the previous-key-state bit. It is set on every keyup,
    // but key release events are never auto-repeat events.
    if (type != blink::WebInputEvent::Type::kKeyUp
        && (flags & MINI_ELECTRON_REPEAT)) {
        modifiers |= blink::WebInputEvent::kIsAutoRepeat;
    }

    if (flags & MINI_ELECTRON_CAPS_LOCK)
        modifiers |= blink::WebInputEvent::kCapsLockOn;
    if (flags & MINI_ELECTRON_NUM_LOCK)
        modifiers |= blink::WebInputEvent::kNumLockOn;
    const ui::DomKeyLocation location =
        ui::KeycodeConverter::DomCodeToLocation(domCode);
    if (location == ui::DomKeyLocation::NUMPAD)
        modifiers |= blink::WebInputEvent::kIsKeyPad;
    else if (location == ui::DomKeyLocation::LEFT)
        modifiers |= blink::WebInputEvent::kIsLeft;
    else if (location == ui::DomKeyLocation::RIGHT)
        modifiers |= blink::WebInputEvent::kIsRight;
    keyEvent.SetModifiers(modifiers);

    std::fill(keyEvent.text.begin(), keyEvent.text.end(), 0);
    std::fill(keyEvent.unmodified_text.begin(), keyEvent.unmodified_text.end(), 0);
    if (isCharacter) {
        keyEvent.text[0] = static_cast<char16_t>(wParam);
        keyEvent.unmodified_text[0] = static_cast<char16_t>(wParam);
    } else if (domKey.IsCharacter() && domKey.ToCharacter() <= 0xffff) {
        char16_t character = static_cast<char16_t>(domKey.ToCharacter());
        keyEvent.unmodified_text[0] = character;
        if ((flags & MINI_ELECTRON_CONTROL) && !(flags & MINI_ELECTRON_ALT)
            && character >= 0x40 && character <= 0x7f) {
            character &= 0x1f;
        }
        keyEvent.text[0] = character;
    }
    return keyEvent;
}

}