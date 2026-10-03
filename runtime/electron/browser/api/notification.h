// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_BROWSER_API_NOTIFICATION_H_
#define RUNTIME_ELECTRON_BROWSER_API_NOTIFICATION_H_

#include <cstdint>
#include <string>

#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "v8.h"

namespace atom {

class NativeImage;

class Notification : public mate::EventEmitter<Notification> {
public:
    Notification(v8::Isolate* isolate, v8::Local<v8::Object> wrapper,
        const gin_helper::Dictionary& options);
    ~Notification() override;

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target);
    static bool isSupported();

    void show();
    void close();

    static gin_helper::WrapperInfo kWrapperInfo;

private:
    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
    void closePlatform(bool emit_event);
    void didShow();
    void didClick();
    void didClose();
    void didFail(const std::string& error);

    std::u16string title_;
    std::u16string body_;
    bool silent_ = false;
    NativeImage* icon_ = nullptr;
    v8::Global<v8::Object> icon_handle_;
    uint64_t platform_identifier_ = 0;
    bool shown_ = false;
    bool closed_ = false;
};

} // namespace atom

#endif // RUNTIME_ELECTRON_BROWSER_API_NOTIFICATION_H_
