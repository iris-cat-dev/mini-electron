// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_SCRIPT_JS_MODULE_SCRIPT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_SCRIPT_JS_MODULE_SCRIPT_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/script/module_script.h"
#include "third_party/blink/renderer/platform/bindings/name_client.h"
#include "third_party/blink/renderer/platform/bindings/parkable_string.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/wtf/text/text_position.h"

namespace blink {

class KURL;
class Modulator;
class ModuleScriptCreationParams;

// JSModuleScript is a model object for the "JavaScript module script" spec
// concept. https://html.spec.whatwg.org/C/#javascript-module-script
class CORE_EXPORT JSModuleScript final : public ModuleScript, public NameClient {
public:
    // https://html.spec.whatwg.org/C/#creating-a-javascript-module-script
    static JSModuleScript* Create(
        const ModuleScriptCreationParams& params, Modulator*, const ScriptFetchOptions&, const TextPosition& start_position = TextPosition::MinimumPosition());

    // Mostly corresponds to Create() but accepts ModuleRecord as the argument
    // and allows null ModuleRecord.
    static JSModuleScript* CreateForTest(Modulator*, v8::Local<v8::Module>, const KURL& base_url, const ScriptFetchOptions& = ScriptFetchOptions());

    // Do not call this constructor. Use Create() instead. This is public only for
    // MakeGarbageCollected.
#if defined(MINI_UI_QUICKJS)
    JSModuleScript(
        Modulator* settings_object, v8::Local<v8::Module>,
        const KURL& source_url, const KURL& base_url,
        const ScriptFetchOptions&, const TextPosition& start_position);
#else
    JSModuleScript(Modulator* settings_object, v8::Local<v8::Module> record, const KURL& source_url, const KURL& base_url, const ScriptFetchOptions&,
        size_t source_text_length, const TextPosition& start_position, ModuleRecordProduceCacheData*);
    void ProduceCache() override;
#endif
    ~JSModuleScript() override = default;

    void Trace(Visitor*) const override;
    const char* NameInHeapSnapshot() const override
    {
        return "JSModuleScript";
    }

private:
    friend class ModuleScriptTest;

#if defined(MINI_UI_QUICKJS)
    static JSModuleScript* CreateInternal(
        Modulator*, v8::Local<v8::Module>, const KURL& source_url,
        const KURL& base_url, const ScriptFetchOptions&,
        const TextPosition&);
#else
    static JSModuleScript* CreateInternal(size_t source_text_length, Modulator*, v8::Local<v8::Module>, const KURL& source_url, const KURL& base_url,
        const ScriptFetchOptions&, const TextPosition&, ModuleRecordProduceCacheData* produce_cache_data);

    // For V8CodeCache statistics.
    const size_t source_text_length_;

    // Cleared once ProduceCache() is called, so a module shared by multiple
    // module graphs produces its V8 cache only once.
    Member<ModuleRecordProduceCacheData> produce_cache_data_;
#endif
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_CORE_SCRIPT_JS_MODULE_SCRIPT_H_
