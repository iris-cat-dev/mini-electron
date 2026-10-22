// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_UI_H_
#define MINI_UI_UI_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#if defined(_WIN32) && defined(MINI_UI_SHARED)
#if defined(MINI_UI_IMPLEMENTATION)
#define MINI_UI_API __declspec(dllexport)
#else
#define MINI_UI_API __declspec(dllimport)
#endif
#else
#define MINI_UI_API
#endif

namespace mini_ui {

enum class ErrorCode {
    none,
    invalid_argument,
    resource_denied,
    not_found,
    io,
    script,
    render,
    internal,
};

struct Error {
    ErrorCode code = ErrorCode::none;
    std::string message;

    explicit operator bool() const noexcept { return code != ErrorCode::none; }
};

struct ViewOptions {
    std::filesystem::path resource_root;
    int width = 800;
    int height = 600;
    float device_scale = 1.0f;
    // Optional borrowed HWND on Windows, created on this view's thread. Enables
    // native DOM pointer lock; nullptr leaves offscreen views unsupported.
    // The host keeps pumping window messages and must destroy the view before
    // destroying the window. While locked the backend consumes native mouse
    // messages (including WM_INPUT) and owns capture/raw registration; the host
    // must not replace that registration until unlock. Ordinary absolute
    // Pointer() input is unchanged outside the lock.
    void* native_window = nullptr;
};

// Premultiplied BGRA pixels owned by the view. Valid until the next mutating
// view operation or destruction. Stride is measured in bytes.
struct FrameView {
    const std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
    std::uint64_t generation = 0;
};

enum Modifier : unsigned {
    shift_key = 1u << 0,
    control_key = 1u << 1,
    alt_key = 1u << 2,
    meta_key = 1u << 3,
    key_pad = 1u << 4,
    caps_lock = 1u << 9,
    num_lock = 1u << 10,
};

struct PointerEvent {
    enum Type { move, down, up, wheel, leave };
    Type type = move;
    double x = 0;
    double y = 0;
    int button = 0;
    unsigned modifiers = 0;
    double delta_x = 0;
    double delta_y = 0;
};

struct KeyEvent {
    enum class Type { down, up, text };
    Type type = Type::down;
    unsigned virtual_key = 0;
    unsigned scan_code = 0;
    unsigned modifiers = 0;
    bool repeat = false;
    std::u16string text;
};

// A view and all of its operations belong to the thread that creates it. The
// host owns the window and event loop and supplies monotonically increasing
// seconds to Tick(). Resources may only be read below resource_root.
class MINI_UI_API View final {
public:
    static std::unique_ptr<View> Create(const ViewOptions&, Error* = nullptr);
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;
    View(View&&) = delete;
    View& operator=(View&&) = delete;

    bool LoadFile(std::string_view relative_path, Error* = nullptr);
    bool Evaluate(std::string_view source, std::string_view source_url,
        std::string* json_result = nullptr, Error* = nullptr);
    bool Resize(int width, int height, float device_scale, Error* = nullptr);
    bool Tick(double monotonic_seconds, Error* = nullptr);
    FrameView frame() const;
    void Pointer(const PointerEvent&);
    void Key(const KeyEvent&);

private:
    struct Impl;
    explicit View(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

} // namespace mini_ui

#endif // MINI_UI_UI_H_
