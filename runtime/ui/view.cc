// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "mini_ui/ui.h"

#include <utility>

#include "runtime/ui/blink_backend.h"
#include "runtime/ui/local_resources.h"

namespace mini_ui {

struct View::Impl {
    explicit Impl(std::unique_ptr<BlinkBackend> value)
        : backend(std::move(value))
    {
    }

    std::unique_ptr<BlinkBackend> backend;
};

std::unique_ptr<View> View::Create(const ViewOptions& options, Error* error)
{
    auto resources = LocalResources::Open(options.resource_root, error);
    if (!resources)
        return nullptr;
    auto backend = BlinkBackend::Create(options,
        std::shared_ptr<LocalResources>(std::move(resources)), error);
    if (!backend)
        return nullptr;
    auto impl = std::make_unique<Impl>(std::move(backend));
    return std::unique_ptr<View>(new View(std::move(impl)));
}

View::View(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

View::~View() = default;

bool View::LoadFile(std::string_view path, Error* error)
{
    return impl_->backend->LoadFile(path, error);
}

bool View::Evaluate(std::string_view source, std::string_view source_url,
    std::string* json_result, Error* error)
{
    return impl_->backend->Evaluate(source, source_url, json_result, error);
}

bool View::Resize(int width, int height, float device_scale, Error* error)
{
    return impl_->backend->Resize(width, height, device_scale, error);
}

bool View::Tick(double monotonic_seconds, Error* error)
{
    return impl_->backend->Tick(monotonic_seconds, error);
}

FrameView View::frame() const
{
    return impl_->backend->frame();
}

void View::Pointer(const PointerEvent& event)
{
    impl_->backend->Pointer(event);
}

void View::Key(const KeyEvent& event)
{
    impl_->backend->Key(event);
}

} // namespace mini_ui
