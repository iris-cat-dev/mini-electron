// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <string>

#include "runtime/electron/node_bindings.h"
#include "runtime/electron/common/platform_util.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"

#include "base/win/scoped_com_initializer.h"
#include "base/win/shortcut.h"
#include "base/files/file_path.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_version.h"
#include "third_party/libuv/include/uv.h"

void initializeFeaturesApi(v8::Local<v8::Object> exports, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, void* priv)
{
    v8::Isolate* isolate = context->GetIsolate();
    v8::Local<v8::Object> obj = v8::Object::New(isolate);
    gin_helper::Dictionary dict(context->GetIsolate(), obj);
    dict.Set("isDesktopCapturerEnabled", false);
    dict.SetMethod("isViewApiEnabled", false);

    exports->Set(context, v8::String::NewFromUtf8(isolate, "Shell").ToLocalChecked(), obj);
}

static const char CommonFeaturesNative[] = "console.log('CommonFeaturesNative');;";
static NodeNative nativeCommonFeaturesNative { "Features", CommonFeaturesNative, sizeof(CommonFeaturesNative) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_common_features, initializeFeaturesApi, &nativeCommonFeaturesNative)
