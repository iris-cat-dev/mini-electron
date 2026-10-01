
#include "runtime/engine/browser/popup_widget_host_impl.h"
#include "runtime/engine/common/common.h"

namespace content {

PopupWidgetHostImpl::PopupWidgetHostImpl()
{
}

PopupWidgetHostImpl::~PopupWidgetHostImpl()
{
}

void PopupWidgetHostImpl::RequestClosePopup()
{
    printFuncName(__FUNCTION__, true, true);
}

//using ShowPopupCallback = base::OnceCallback<void()>;
void PopupWidgetHostImpl::ShowPopup(const ::gfx::Rect& initial_rect, const ::gfx::Rect& anchor_rect, ShowPopupCallback callback)
{
    printFuncName(__FUNCTION__, true, true);
}

//using SetPopupBoundsCallback = base::OnceCallback<void()>;
void PopupWidgetHostImpl::SetPopupBounds(const ::gfx::Rect& bounds, SetPopupBoundsCallback callback)
{
    printFuncName(__FUNCTION__, true, true);
}

}