// Copyright (c) 2017 weolar, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "src/runtime/electron/node_bindings.h"
#include "src/runtime/electron/common/api/event_emitter.h"
#include "src/runtime/electron/common/gin_helper/dictionary.h"
#include "src/runtime/electron/common/gin_helper/object_template_builder.h"
#include "src/runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "src/runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "src/runtime/electron/common/gin_helper/wrappable.h"
#include "src/runtime/electron/common/node_register_help.h"

#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/display/win/screen_win.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"

namespace {

base::Value::Dict CreatePointValue(const gfx::Point& point)
{
    base::Value::Dict value;
    value.Set("x", point.x());
    value.Set("y", point.y());
    return value;
}

base::Value::Dict CreateSizeValue(const gfx::Size& size)
{
    base::Value::Dict value;
    value.Set("width", size.width());
    value.Set("height", size.height());
    return value;
}

base::Value::Dict CreateRectValue(const gfx::Rect& rect)
{
    base::Value::Dict value;
    value.Set("x", rect.x());
    value.Set("y", rect.y());
    value.Set("width", rect.width());
    value.Set("height", rect.height());
    return value;
}

const char* TouchSupportToString(display::Display::TouchSupport touch_support)
{
    switch (touch_support) {
    case display::Display::TouchSupport::AVAILABLE:
        return "available";
    case display::Display::TouchSupport::UNAVAILABLE:
        return "unavailable";
    case display::Display::TouchSupport::UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

base::Value::Dict CreateDisplayValue(const display::Display& display)
{
    base::Value::Dict value;
    value.Set("id", static_cast<int>(display.id()));
    value.Set("rotation", display.RotationAsDegree());
    value.Set("scaleFactor", display.device_scale_factor());
    value.Set("touchSupport", TouchSupportToString(display.touch_support()));
    value.Set("bounds", CreateRectValue(display.bounds()));
    value.Set("size", CreateSizeValue(display.size()));
    value.Set("workArea", CreateRectValue(display.work_area()));
    value.Set("workAreaSize", CreateSizeValue(display.work_area_size()));
    return value;
}

bool GetPointArgument(const v8::FunctionCallbackInfo<v8::Value>& args, gfx::Point* point)
{
    if (args.Length() < 1 || !args[0]->IsObject())
        return false;

    gin_helper::Dictionary value(args.GetIsolate(), args[0].As<v8::Object>());
    int x;
    int y;
    if (!value.Get("x", &x) || !value.Get("y", &y))
        return false;
    point->SetPoint(x, y);
    return true;
}

bool GetRectArgument(const v8::FunctionCallbackInfo<v8::Value>& args, gfx::Rect* rect)
{
    if (args.Length() < 1 || !args[0]->IsObject())
        return false;

    gin_helper::Dictionary value(args.GetIsolate(), args[0].As<v8::Object>());
    int x;
    int y;
    int width;
    int height;
    if (!value.Get("x", &x) || !value.Get("y", &y)
        || !value.Get("width", &width) || !value.Get("height", &height)) {
        return false;
    }
    rect->SetRect(x, y, width, height);
    return true;
}

THREAD_LOCAL_CONSTRUCTOR(Screen)

class Screen : public mate::EventEmitter<Screen> {
public:
    explicit Screen(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
    {
        gin_helper::Wrappable<Screen>::InitWith(isolate, wrapper);
    }

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target)
    {
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        v8::Local<v8::FunctionTemplate> prototype = v8::FunctionTemplate::New(isolate, newFunction);

        prototype->SetClassName(v8::String::NewFromUtf8(isolate, "Screen").ToLocalChecked());
        gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
        builder.SetMethod("getCursorScreenPoint", &Screen::getCursorScreenPointApi);
        builder.SetMethod("getPrimaryDisplay", &Screen::getPrimaryDisplayApi);
        builder.SetMethod("getAllDisplays", &Screen::getAllDisplaysApi);
        builder.SetMethod("getDisplayNearestPoint", &Screen::getDisplayNearestPointApi);
        builder.SetMethod("getDisplayMatching", &Screen::getDisplayMatchingApi);
        builder.SetMethod("addListener", &Screen::addListenerApi);

        getScreenConstructor().Reset(isolate, prototype->GetFunction(context).ToLocalChecked());
        target->Set(context, v8::String::NewFromUtf8(isolate, "Screen").ToLocalChecked(),
            prototype->GetFunction(context).ToLocalChecked());
    }

    void getCursorScreenPointApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        SetReturnValue(args, CreatePointValue(nativeScreen()->GetCursorScreenPoint()));
    }

    void getPrimaryDisplayApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        SetReturnValue(args, CreateDisplayValue(nativeScreen()->GetPrimaryDisplay()));
    }

    void getAllDisplaysApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        const std::vector<display::Display>& native_displays = nativeScreen()->GetAllDisplays();
        base::Value::List displays;
        displays.reserve(native_displays.size());
        for (const display::Display& display : native_displays)
            displays.Append(CreateDisplayValue(display));

        args.GetReturnValue().Set(
            gin_helper::Converter<base::Value::List>::ToV8(args.GetIsolate(), displays));
    }

    void getDisplayNearestPointApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        gfx::Point point;
        if (!GetPointArgument(args, &point))
            return;
        SetReturnValue(args, CreateDisplayValue(nativeScreen()->GetDisplayNearestPoint(point)));
    }

    void getDisplayMatchingApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        gfx::Rect rect;
        if (!GetRectArgument(args, &rect))
            return;
        SetReturnValue(args, CreateDisplayValue(nativeScreen()->GetDisplayMatching(rect)));
    }

    void addListenerApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        OutputDebugStringA("Screen.addListener\n");
    }

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        if (args.IsConstructCall()) {
            new Screen(args.GetIsolate(), args.This());
            args.GetReturnValue().Set(args.This());
        }
    }

    static gin_helper::WrapperInfo kWrapperInfo;

private:
    static display::Screen* nativeScreen()
    {
        if (!display::Screen::GetScreen()) {
            static base::NoDestructor<display::win::ScreenWin> screen;
            display::Screen::SetScreenInstance(screen.get(), FROM_HERE);
        }
        return display::Screen::GetScreen();
    }

    static void SetReturnValue(
        const v8::FunctionCallbackInfo<v8::Value>& args, base::Value::Dict value)
    {
        args.GetReturnValue().Set(
            gin_helper::Converter<base::Value::Dict>::ToV8(args.GetIsolate(), std::move(value)));
    }
};

gin_helper::WrapperInfo Screen::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin
};

void initializeCommonScreenApi(v8::Local<v8::Object> exports,
    v8::Local<v8::Value> target,
    v8::Local<v8::Context> context,
    void* priv)
{
    Screen::init(context->GetIsolate(), exports);
}

} // namespace

static const char CommonScreenNative[] = "console.log('CommonScreenNative');;";
static NodeNative nativeCommonScreenNative {
    "Screen", CommonScreenNative, sizeof(CommonScreenNative) - 1
};

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(
    electron_common_screen, initializeCommonScreenApi, &nativeCommonScreenNative)
