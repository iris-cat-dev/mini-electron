// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_BROWSER_API_NATIVE_THEME_H_
#define RUNTIME_ELECTRON_BROWSER_API_NATIVE_THEME_H_

#include <cstdint>

#include "runtime/electron/common/api/event_emitter.h"
#include "v8.h"
#if defined(_WIN32)
#include <windows.h>
#endif

namespace atom {

class NativeTheme : public mate::EventEmitter<NativeTheme> {
public:
    NativeTheme(v8::Isolate* isolate, v8::Local<v8::Object> wrapper);
    ~NativeTheme() override;

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target);
    bool shouldUseDarkColors() const;

    static gin_helper::WrapperInfo kWrapperInfo;

private:
    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
    void systemThemeChanged();
#if defined(_WIN32)
    static LRESULT CALLBACK WindowProc(
        HWND, UINT, WPARAM, LPARAM);
    HWND window_ = nullptr;
#elif defined(__APPLE__)
    uint64_t observer_identifier_ = 0;
#endif
    bool dark_ = false;
};

} // namespace atom

#endif // RUNTIME_ELECTRON_BROWSER_API_NATIVE_THEME_H_
