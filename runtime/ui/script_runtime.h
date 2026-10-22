// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_SCRIPT_RUNTIME_H_
#define MINI_UI_RUNTIME_UI_SCRIPT_RUNTIME_H_

#include <memory>
#include <string>
#include <string_view>

#include "mini_ui/ui.h"
#include "third_party/quickjs/quickjs.h"

namespace mini_ui {

class LocalResources;
class ScriptRuntimeState;

class ScriptRuntime final {
public:
    using PromiseRejectionObserver = void (*)(JSContext*, JSValueConst promise,
        JSValueConst reason, bool handled, void* user_data) noexcept;

    // One runtime can host independent document capabilities in many realms.
    static std::unique_ptr<ScriptRuntime> Create(Error* error = nullptr);
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;
    ScriptRuntime(ScriptRuntime&&) = delete;
    ScriptRuntime& operator=(ScriptRuntime&&) = delete;

    JSRuntime* get() const;
    bool DrainJobs(Error* error = nullptr);
    // Observes both rejection and later handling alongside the runtime's error
    // tracker. Arguments are borrowed for the call; observer/user_data are not
    // owned. Clear before destroying the observer's owner. UI-thread only;
    // callbacks must not destroy the runtime.
    void SetPromiseRejectionObserver(
        PromiseRejectionObserver, void* user_data = nullptr) noexcept;

private:
    explicit ScriptRuntime(std::shared_ptr<ScriptRuntimeState>);

    std::shared_ptr<ScriptRuntimeState> state_;

    friend class ScriptRealm;
};

class ScriptRealm final {
public:
    // The context retains resources until its actual QuickJS finalization,
    // including when jobs or facade handles outlive this C++ realm handle.
    static std::unique_ptr<ScriptRealm> Create(
        ScriptRuntime&, std::shared_ptr<LocalResources>, Error* error = nullptr);
    ~ScriptRealm();

    ScriptRealm(const ScriptRealm&) = delete;
    ScriptRealm& operator=(const ScriptRealm&) = delete;
    ScriptRealm(ScriptRealm&&) = delete;
    ScriptRealm& operator=(ScriptRealm&&) = delete;

    JSContext* get() const;
    // Shares only the pinned filesystem capability, never the realm/context.
    std::shared_ptr<LocalResources> local_resources() const;
    bool Evaluate(std::string_view source, std::string_view source_url,
        std::string* json_result = nullptr, Error* error = nullptr);
    bool EvaluateModule(std::string_view source, std::string_view source_url,
        std::string* json_result = nullptr, Error* error = nullptr);

private:
    ScriptRealm(std::shared_ptr<ScriptRuntimeState>, JSContext*);
    bool EvaluateInternal(std::string_view source, std::string_view source_url,
        bool force_module, std::string* json_result, Error* error);

    std::shared_ptr<ScriptRuntimeState> state_;
    JSContext* context_ = nullptr;
};

} // namespace mini_ui

#endif // MINI_UI_RUNTIME_UI_SCRIPT_RUNTIME_H_
