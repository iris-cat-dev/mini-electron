// Copyright (c) 2019 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_COMMON_GIN_CONVERTERS_CALLBACK_CONVERTER_H_
#define ELECTRON_SHELL_COMMON_GIN_CONVERTERS_CALLBACK_CONVERTER_H_

#include <utility>

//#include "base/callback_helpers.h"
#include "runtime/electron/common/gin_helper/callback.h"

namespace gin_helper {

template <typename Sig> struct Converter<base::RepeatingCallback<Sig>> {
    static v8::Local<v8::Value> ToV8(v8::Isolate* isolate, const base::RepeatingCallback<Sig>& val)
    {
        // We don't use CreateFunctionTemplate here because it creates a new
        // FunctionTemplate everytime, which is cached by V8 and causes leaks.
        auto translater = base::BindRepeating(&gin_helper::NativeFunctionInvoker<Sig>::Go, val);
        return gin_helper::CreateFunctionFromTranslater(isolate, translater, false);
    }
    static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, base::RepeatingCallback<Sig>* out)
    {
        if (!val->IsFunction())
            return false;

        *out = base::BindRepeating(&gin_helper::V8FunctionInvoker<Sig>::Go, isolate, gin_helper::SafeV8Function(isolate, val));
        return true;
    }
};

template <typename ReturnType, typename... ArgTypes> struct Converter<base::OnceCallback<ReturnType(ArgTypes...)>> {
    using Sig = ReturnType(ArgTypes...);
    static v8::Local<v8::Value> ToV8(v8::Isolate* isolate, base::OnceCallback<Sig> in)
    {
        auto callback = base::BindRepeating(
            [](base::OnceCallback<Sig>& once, ArgTypes... args) -> ReturnType {
                return std::move(once).Run(std::forward<ArgTypes>(args)...);
            },
            base::OwnedRef(std::move(in)));
        auto translater = base::BindRepeating(&gin_helper::NativeFunctionInvoker<Sig>::Go, callback);
        return gin_helper::CreateFunctionFromTranslater(isolate, translater, true);
    }
    static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, base::OnceCallback<Sig>* out)
    {
        if (!val->IsFunction())
            return false;
        *out = base::BindOnce(&gin_helper::V8FunctionInvoker<Sig>::Go, isolate, gin_helper::SafeV8Function(isolate, val));
        return true;
    }
};

} // namespace gin

#endif // ELECTRON_SHELL_COMMON_GIN_CONVERTERS_CALLBACK_CONVERTER_H_
