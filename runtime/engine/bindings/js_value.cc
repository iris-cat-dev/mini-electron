
#include "runtime/engine/bindings/js_value.h"

#include "third_party/blink/renderer/bindings/core/v8/v8_html_frame_element.h"
#include "third_party/blink/renderer/core/html/html_frame_element.h"
#include "third_party/blink/public/web/web_local_frame.h"

namespace blink {
class Frame;
}

namespace content {
intptr_t getFrameIdByWebLocalFrame(blink::WebLocalFrame* frame);
}

namespace mini_electron::engine {

JsValueBridge::JsValueBridge()
{
    m_id = common::LiveIdDetect::get()->constructed(this);
}

JsValueBridge::~JsValueBridge()
{
    common::LiveIdDetect::get()->deconstructed(m_id);
}

JsValueBridge* JsValueBridge::create()
{
    return new JsValueBridge();
}

void JsValueBridge::ref()
{
    m_ref++;
}

void JsValueBridge::deref()
{
    m_ref--;
    if (0 == m_ref)
        delete this;
}

static mini_electron_web_frame_handle getFrameId(v8::Isolate* isolate, v8::Local<v8::Value> value)
{
    if (!blink::V8HTMLFrameElement::HasInstance(isolate, value))
        return nullptr;

    blink::HTMLFrameElement* frameElement = blink::V8HTMLFrameElement::ToWrappableUnsafe(isolate, value.As<v8::Object>());
    if (!frameElement)
        return nullptr;

    blink::Frame* frame = frameElement->ContentFrame();
    if (!frame)
        return nullptr;
    blink::WebFrame* webframe = blink::WebFrame::FromCoreFrame(frame);
    intptr_t id = content::getFrameIdByWebLocalFrame(webframe->ToWebLocalFrame());
    return (mini_electron_web_frame_handle)id;
}

JsValueBridge* JsValueBridge::v8ValueToEngineValue(v8::Isolate* isolate, v8::Local<v8::Context> context, v8::Local<v8::Value> value)
{
    JsValueBridge* result = new JsValueBridge();

    if (value.IsEmpty())
        result->m_type = kMiniElectronJsTypeNull;
    else if (value->IsUndefined()) {
        result->m_type = kMiniElectronJsTypeUndefined;
    } else if (value->IsBoolean()) {
        result->m_type = kMiniElectronJsTypeBool;
        result->m_boolVal = value->ToBoolean(isolate)->Value();
    } else if (value->IsNumber()) {
        result->m_type = kMiniElectronJsTypeNumber;
        result->m_doubleVal = value->ToNumber(context).ToLocalChecked()->Value();
    } else if (value->IsString()) {
        result->m_type = kMiniElectronJsTypeString;
        v8::Local<v8::String> stringValue = value->ToString(context).ToLocalChecked();
        v8::String::Utf8Value utf8value(isolate, stringValue);
        result->m_strVal.append(*utf8value, utf8value.length());
    } else if (value->IsArray()) {
        result->m_type = kMiniElectronJsTypeString;
        result->m_strVal = "[Array]";
    } else if (value->IsFunction()) {
        result->m_type = kMiniElectronJsTypeString;
        result->m_strVal = "[Function]";
    } else if (value->IsObject()) {
        mini_electron_web_frame_handle id = getFrameId(isolate, value);
        if (0 != id) {
            result->m_type = kMiniElectronJsTypeFrame;
            result->m_webFrameHandle = id;
        } else {
            result->m_type = kMiniElectronJsTypeString;
            result->m_strVal = "[Object]";
        }
    }

    return result;
}

}
