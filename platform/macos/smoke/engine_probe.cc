// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "platform/macos/smoke/engine_probe.h"

#include <memory>

#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"

namespace miniblink::mac {

std::optional<std::string> RunEngineProbe(const char* executable_path) {
  v8::V8::InitializeICUDefaultLocation(executable_path);
  v8::V8::InitializeExternalStartupData(executable_path);

  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  if (!v8::V8::Initialize()) {
    v8::V8::DisposePlatform();
    return std::nullopt;
  }

  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  v8::Isolate* isolate = v8::Isolate::New(params);
  std::optional<std::string> output;

  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    constexpr char kScript[] =
        "(() => {"
        "const locale = new Intl.Locale('zh-CN').maximize().toString();"
        "const date = new Intl.DateTimeFormat('zh-CN', {timeZone: "
        "'Asia/Shanghai', dateStyle: 'full'}).format(new "
        "Date('2026-03-17T00:00:00Z'));"
        "const sorted = ['上海', '北京', '广州'].sort(new "
        "Intl.Collator('zh-CN').compare);"
        "if (locale !== 'zh-Hans-CN' || date !== '2026年3月17日星期二' || "
        "sorted.join(',') !== '北京,广州,上海') throw new Error('ICU data "
        "mismatch');"
        "return JSON.stringify({engine: 'miniblink132', platform: 'macOS', "
        "arch: 'arm64', runLoop: 'CFRunLoop', "
        "unicodeIdentifier: /^[\\p{ID_Start}_][\\p{ID_Continue}_]*$/u.test('_变量1'), "
        "intlLocale: new Intl.NumberFormat('zh-CN').resolvedOptions().locale, "
        "icuLocale: locale, icuDate: date, icuSort: sorted, "
        "result: [1, 2, 3].map(x => x * 7).reduce((a, b) => a + b, 0)});"
        "})()";
    v8::Local<v8::String> source;
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    if (v8::String::NewFromUtf8(isolate, kScript).ToLocal(&source) &&
        v8::Script::Compile(context, source).ToLocal(&script) &&
        script->Run(context).ToLocal(&result)) {
      v8::String::Utf8Value utf8(isolate, result);
      if (*utf8)
        output.emplace(*utf8, utf8.length());
    }
  }

  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return output;
}

}  // namespace miniblink::mac
