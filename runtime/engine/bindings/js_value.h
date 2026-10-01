
#ifndef MINI_ELECTRON_ENGINE_BINDINGS_JS_VALUE_H_
#define MINI_ELECTRON_ENGINE_BINDINGS_JS_VALUE_H_

#include "runtime/engine/public/engine_api.h"
#include "runtime/engine/common/live_id_detect.h"
#include "v8.h"

typedef void* mini_electron_web_frame_handle;

namespace mini_electron::engine {

class JsValueBridge {
public:
    static JsValueBridge* create();
    void ref();
    void deref();

private:
    JsValueBridge();
    ~JsValueBridge();

public:
    static JsValueBridge* v8ValueToEngineValue(v8::Isolate* isolate, v8::Local<v8::Context> context, v8::Local<v8::Value> value);

    int64_t getId() const
    {
        return m_id;
    }
    mini_electron_js_type getType() const
    {
        return m_type;
    }

    double getDoubleVal() const
    {
        return m_doubleVal;
    }
    std::string getStrVal() const
    {
        return m_strVal;
    }
    BOOL getBoolVal() const
    {
        return m_boolVal;
    }
    mini_electron_web_frame_handle getWebFrameHandle() const
    {
        return m_webFrameHandle;
    }

private:
    int m_ref = 1;
    int64_t m_id = -1;
    mini_electron_js_type m_type = kMiniElectronJsTypeUndefined;

    double m_doubleVal = 0;
    std::string m_strVal;
    BOOL m_boolVal = FALSE;
    mini_electron_web_frame_handle m_webFrameHandle = nullptr;
};

}

#endif // MINI_ELECTRON_ENGINE_BINDINGS_JS_VALUE_H_