// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_BLINK_BACKEND_H_
#define MINI_UI_RUNTIME_UI_BLINK_BACKEND_H_

#include <memory>
#include <string>
#include <string_view>

#include "mini_ui/ui.h"

namespace mini_ui {

class LocalResources;

class BlinkBackend final {
public:
    // Views share the process runtime/isolate, never a document context or root.
    static std::unique_ptr<BlinkBackend> Create(
        const ViewOptions&, std::shared_ptr<LocalResources>, Error* error = nullptr);
    ~BlinkBackend();

    BlinkBackend(const BlinkBackend&) = delete;
    BlinkBackend& operator=(const BlinkBackend&) = delete;
    BlinkBackend(BlinkBackend&&) = delete;
    BlinkBackend& operator=(BlinkBackend&&) = delete;

    bool LoadFile(std::string_view relative_path, Error* error = nullptr);
    bool Evaluate(std::string_view source, std::string_view source_url,
        std::string* json_result = nullptr, Error* error = nullptr);
    bool Resize(int width, int height, float device_scale, Error* error = nullptr);
    bool Tick(double monotonic_seconds, Error* error = nullptr);
    // CPU raster includes any active native form popup composited above the page.
    FrameView frame() const;
    void Pointer(const PointerEvent&);
    void Key(const KeyEvent&);

private:
    class Impl;
    explicit BlinkBackend(std::unique_ptr<Impl>);

    std::unique_ptr<Impl> impl_;
};

} // namespace mini_ui

#endif // MINI_UI_RUNTIME_UI_BLINK_BACKEND_H_
