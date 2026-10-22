// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/bindings/core/v8/v8_initializer.h"

#include "base/json/json_reader.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/re2/src/re2/re2.h"
#include "v8/include/v8.h"

namespace blink {

bool FilterETWSessionByURLCallback(v8::Local<v8::Context> context, const std::string& json_payload)
{
    std::optional<base::Value> optional_value = base::JSONReader::Read(json_payload);
    if (!optional_value || !optional_value->is_dict())
        return false;
    const base::Value::List* filtered_urls = optional_value->GetDict().FindList("filtered_urls");
    if (!filtered_urls)
        return false;
    for (const base::Value& filtered_url : *filtered_urls) {
        if (!filtered_url.is_string())
            return false;
        ExecutionContext* execution_context = ToExecutionContext(context);
        if (execution_context) {
            std::string url(execution_context->Url().GetString().Utf8());
            const RE2 regex(filtered_url.GetString());
            if (RE2::FullMatch(url, regex))
                return true;
        }
    }
    return false; // No regex matching found.
}

} // namespace blink
