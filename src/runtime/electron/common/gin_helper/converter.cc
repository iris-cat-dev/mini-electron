// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/runtime/electron/common/gin_helper/converter.h"

#include "src/runtime/electron/common/v8_util.h"
#include "third_party/blink/public/common/messaging/cloneable_message.h"
#include "v8.h"
#include <cmath>

#include <windows.h>
#include "base/memory/ptr_util.h"
#include "base/notreached.h"

using v8::ArrayBuffer;
using v8::Boolean;
using v8::External;
using v8::Function;
using v8::Int32;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Maybe;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Uint32;
using v8::Value;

namespace content {
void printCallstack();
}

namespace {

template <typename T, typename U> bool FromMaybe(v8::Maybe<T> maybe, U* out)
{
    if (maybe.IsNothing())
        return false;
    *out = static_cast<U>(maybe.FromJust());
    return true;
}

} // namespace

namespace gin_helper {

v8::Local<v8::Value> Converter<bool>::ToV8(v8::Isolate* isolate, bool val)
{
    return v8::Boolean::New(isolate, val).As<v8::Value>();
}

bool Converter<bool>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, bool* out)
{
    //return FromMaybe(val->BooleanValue(isolate), out);
    *out = val->BooleanValue(isolate);
    return true;
}

v8::Local<v8::Value> Converter<int32_t>::ToV8(v8::Isolate* isolate, int32_t val)
{
    return Integer::New(isolate, val).As<v8::Value>();
}

bool Converter<int32_t>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, int32_t* out)
{
    if (!val->IsInt32())
        return false;
    *out = val.As<Int32>()->Value();
    return true;
}

v8::Local<v8::Value> Converter<uint32_t>::ToV8(v8::Isolate* isolate, uint32_t val)
{
    return Integer::NewFromUnsigned(isolate, val).As<Value>();
}

bool Converter<uint32_t>::FromV8(Isolate* isolate, v8::Local<v8::Value> val, uint32_t* out)
{
    if (!val->IsUint32())
        return false;
    *out = val.As<Uint32>()->Value();
    return true;
}

v8::Local<v8::Value> Converter<int64_t>::ToV8(v8::Isolate* isolate, int64_t val)
{
    return v8::Number::New(isolate, static_cast<double>(val)).As<v8::Value>();
}

bool Converter<int64_t>::FromV8(Isolate* isolate, Local<Value> val, int64_t* out)
{
    if (!val->IsNumber()) {
        content::printCallstack();
        return false;
    }
    // Even though IntegerValue returns int64_t, JavaScript cannot represent
    // the full precision of int64_t, which means some rounding might occur.
    return FromMaybe(val->IntegerValue(isolate->GetCurrentContext()), out);
}

Local<Value> Converter<uint64_t>::ToV8(Isolate* isolate, uint64_t val)
{
    return Number::New(isolate, static_cast<double>(val)).As<Value>();
}

// Local<Value> Converter<unsigned long long>::ToV8(Isolate* isolate, unsigned long long val)
// {
//     return Number::New(isolate, static_cast<double>(val)).As<Value>();
// }

bool Converter<uint64_t>::FromV8(Isolate* isolate, v8::Local<v8::Value> val, uint64_t* out)
{
    if (!val->IsNumber())
        return false;
    return FromMaybe(val->IntegerValue(isolate->GetCurrentContext()), out);
}

Local<Value> Converter<float>::ToV8(Isolate* isolate, float val)
{
    return Number::New(isolate, val).As<Value>();
}

bool Converter<float>::FromV8(Isolate* isolate, v8::Local<v8::Value> val, float* out)
{
    if (!val->IsNumber())
        return false;
    *out = static_cast<float>(val.As<v8::Number>()->Value());
    return true;
}

v8::Local<v8::Value> Converter<double>::ToV8(v8::Isolate* isolate, double val)
{
    return Number::New(isolate, val).As<v8::Value>();
}

bool Converter<double>::FromV8(Isolate* isolate, Local<Value> val, double* out)
{
    if (!val->IsNumber())
        return false;
    *out = val.As<Number>()->Value();
    return true;
}

Local<Value> Converter<base::StringPiece>::ToV8(v8::Isolate* isolate, const base::StringPiece& val)
{
    return v8::String::NewFromUtf8(isolate, val.data(), v8::NewStringType::kNormal, static_cast<uint32_t>(val.length())).ToLocalChecked();
}

v8::Local<v8::Value> Converter<const char*>::ToV8(v8::Isolate* isolate, const char* val)
{
    return Converter<base::StringPiece>::ToV8(isolate, val);
}

v8::Local<v8::Value> Converter<std::string>::ToV8(v8::Isolate* isolate, const std::string& val)
{
    return Converter<base::StringPiece>::ToV8(isolate, val);
}

bool Converter<std::string>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, std::string* out)
{
    if (val->IsNumber()) {
        double d = val.As<Number>()->Value();
        char temp[64] = { 0 };
        sprintf(temp, "%f\n", d);
        *out = temp;
        return true;
    }

    if (val->IsSymbol() || val->IsSymbolObject())
        return false;

    if (!val->IsString()) {
        v8::Local<v8::String> ty = val->TypeOf(isolate);
        int length = ty->Utf8Length(isolate);
        std::vector<char> temp;
        temp.resize(length);
        ty->WriteUtf8(isolate, temp.data(), length, NULL, v8::String::NO_NULL_TERMINATION);
        temp.push_back('\n');
        temp.push_back('\0');
        OutputDebugStringA("Converter<std::string>::FromV8:\n");
        OutputDebugStringA(temp.data());

        return false;
    }
    v8::Local<v8::String> str = v8::Local<v8::String>::Cast(val);
    int length = str->Utf8Length(isolate);
    out->resize(length);
    str->WriteUtf8(isolate, &(*out)[0], length, NULL, v8::String::NO_NULL_TERMINATION);
    return true;
}

v8::Local<v8::Value> Converter<std::u16string>::ToV8(v8::Isolate* isolate, const std::u16string& val)
{
    v8::Local<v8::String> ret = v8::String::NewFromTwoByte(isolate, (const uint16_t*)val.c_str(), v8::NewStringType::kNormal, val.length()).ToLocalChecked();
    return ret;
}

bool Converter<std::u16string>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, std::u16string* out)
{
    if (!val->IsString())
        return false;
    v8::Local<v8::String> str = v8::Local<v8::String>::Cast(val);
    int length = str->Length();
    out->resize(length);
    str->Write(isolate, (uint16_t*)(out->data()), 0, length, v8::String::NO_OPTIONS);
    return true;
}

Local<Value> Converter<v8::Local<v8::String>>::ToV8(v8::Isolate* isolate, v8::Local<v8::String> val)
{
    return val.As<Value>();
}

bool Converter<v8::Local<v8::String>>::FromV8(Isolate* isolate, v8::Local<v8::Value> val, v8::Local<v8::String>* out)
{
    if (!val->IsString())
        return false;
    *out = v8::Local<v8::String>::Cast(val);
    return true;
}

bool Converter<v8::Local<v8::Function>>::FromV8(v8::Isolate* isolate, Local<Value> val, v8::Local<v8::Function>* out)
{
    if (!val->IsFunction())
        return false;
    *out = v8::Local<v8::Function>::Cast(val);
    return true;
}

v8::Local<v8::Value> Converter<v8::Local<v8::Object>>::ToV8(v8::Isolate* isolate, v8::Local<v8::Object> val)
{
    return val.As<Value>();
}

v8::Local<v8::Value> Converter<v8::Local<v8::Primitive>>::ToV8(v8::Isolate* isolate, v8::Local<v8::Primitive> val)
{
    return val.As<Value>();
}

bool Converter<v8::Local<v8::Object>>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, v8::Local<v8::Object>* out)
{
    if (!val->IsObject())
        return false;
    *out = Local<Object>::Cast(val);
    return true;
}

Local<Value> Converter<Local<ArrayBuffer>>::ToV8(Isolate* isolate, Local<ArrayBuffer> val)
{
    return val.As<Value>();
}

bool Converter<Local<ArrayBuffer>>::FromV8(Isolate* isolate, Local<Value> val, Local<ArrayBuffer>* out)
{
    if (!val->IsArrayBuffer())
        return false;
    *out = Local<ArrayBuffer>::Cast(val);
    return true;
}

Local<Value> Converter<Local<External>>::ToV8(Isolate* isolate, Local<External> val)
{
    return val.As<Value>();
}

bool Converter<Local<External>>::FromV8(Isolate* isolate, v8::Local<Value> val, Local<External>* out)
{
    if (!val->IsExternal())
        return false;
    *out = Local<External>::Cast(val);
    return true;
}

Local<Value> Converter<Local<Value>>::ToV8(Isolate* isolate, Local<Value> val)
{
    return val;
}

bool Converter<Local<Value>>::FromV8(Isolate* isolate, Local<Value> val, Local<Value>* out)
{
    *out = val;
    return true;
}

Local<Value> Converter<Local<v8::Array>>::ToV8(Isolate* isolate, Local<v8::Array> val)
{
    return val.As<Value>();
}

bool Converter<v8::Local<v8::Array>>::FromV8(Isolate* isolate, v8::Local<v8::Value> val, v8::Local<v8::Array>* out)
{
    if (!val->IsArray())
        return false;
    *out = Local<v8::Array>::Cast(val);
    return true;
}

int v8ValueToType(v8::Local<v8::Value> v)
{
    int type = 0;
    bool b = false;
    b = v->IsUndefined();
    if (b)
        return 0;
    b = v->IsNull();
    if (b)
        return 1;
    b = v->IsTrue();
    if (b)
        return 2;
    b = v->IsFalse();
    if (b)
        return 3;
    b = v->IsName();
    if (b)
        return 4;
    b = v->IsString();
    if (b)
        return 5;
    b = v->IsSymbol();
    if (b)
        return 6;
    b = v->IsFunction();
    if (b)
        return 7;
    b = v->IsArray();
    if (b)
        return 8;
    b = v->IsObject();
    if (b)
        return 9;
    b = v->IsBoolean();
    if (b)
        return 10;
    b = v->IsNumber();
    if (b)
        return 11;
    b = v->IsExternal();
    if (b)
        return 12;
    b = v->IsInt32();
    if (b)
        return 13;
    b = v->IsUint32();
    if (b)
        return 14;
    b = v->IsDate();
    if (b)
        return 15;
    b = v->IsArgumentsObject();
    if (b)
        return 16;
    b = v->IsBooleanObject();
    if (b)
        return 17;
    b = v->IsNumberObject();
    if (b)
        return 18;
    b = v->IsStringObject();
    if (b)
        return 19;
    b = v->IsSymbolObject();
    if (b)
        return 20;
    b = v->IsNativeError();
    if (b)
        return 21;
    b = v->IsRegExp();
    if (b)
        return 22;
    b = v->IsGeneratorFunction();
    if (b)
        return 23;
    b = v->IsGeneratorObject();
    if (b)
        return 24;
    b = v->IsPromise();
    if (b)
        return 25;
    b = v->IsMap();
    if (b)
        return 26;
    b = v->IsSet();
    if (b)
        return 27;
    b = v->IsMapIterator();
    if (b)
        return 28;
    b = v->IsSetIterator();
    if (b)
        return 29;
    b = v->IsWeakMap();
    if (b)
        return 30;
    b = v->IsWeakSet();
    if (b)
        return 31;
    b = v->IsArrayBuffer();
    if (b)
        return 32;
    b = v->IsArrayBufferView();
    if (b)
        return 33;
    b = v->IsTypedArray();
    if (b)
        return 34;
    b = v->IsUint8Array();
    if (b)
        return 35;
    b = v->IsUint8ClampedArray();
    if (b)
        return 36;
    b = v->IsInt8Array();
    if (b)
        return 37;
    b = v->IsUint16Array();
    if (b)
        return 38;
    b = v->IsInt16Array();
    if (b)
        return 39;
    b = v->IsUint32Array();
    if (b)
        return 40;
    b = v->IsInt32Array();
    if (b)
        return 41;
    b = v->IsFloat32Array();
    if (b)
        return 42;
    b = v->IsFloat64Array();
    if (b)
        return 43;
    b = v->IsDataView();
    if (b)
        return 44;
    b = v->IsSharedArrayBuffer();
    if (b)
        return 45;
    return -1;
}

v8::Local<v8::Value> Converter<base::Value::Dict>::ToV8(v8::Isolate* isolate, const base::Value::Dict& val)
{
    v8::Local<v8::Object> result = v8::Object::New(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    for (const auto item : val) {
        v8::Local<v8::Value> value = ConvertToV8(isolate, item.second);
        if (value.IsEmpty() || result->CreateDataProperty(
                context, Converter<std::string>::ToV8(isolate, item.first).As<v8::String>(), value).IsNothing())
            return v8::Local<v8::Value>();
    }
    return result;
}

bool Converter<base::Value::Dict>::FromV8(Isolate* isolate, v8::Local<v8::Value> val, base::Value::Dict* out)
{
    base::Value converted;
    if (!Converter<base::Value>::FromV8(isolate, val, &converted) || !converted.is_dict())
        return false;
    *out = std::move(converted).TakeDict();
    return true;
}


v8::Local<v8::Value> Converter<base::Value::List>::ToV8(v8::Isolate* isolate, const base::Value::List& val)
{
    v8::Local<v8::Array> result = v8::Array::New(isolate, val.size());
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    for (size_t index = 0; index < val.size(); ++index) {
        v8::Local<v8::Value> value = ConvertToV8(isolate, val[index]);
        if (value.IsEmpty() || result->CreateDataProperty(context, index, value).IsNothing())
            return v8::Local<v8::Value>();
    }
    return result;
}

v8::Local<v8::Value> Converter<base::Value>::ToV8(
    v8::Isolate* isolate, base::Value&& val)
{
    switch (val.type()) {
    case base::Value::Type::NONE:
        return v8::Null(isolate);
    case base::Value::Type::BOOLEAN:
        return Converter<bool>::ToV8(isolate, val.GetBool());
    case base::Value::Type::INTEGER:
        return Converter<int32_t>::ToV8(isolate, val.GetInt());
    case base::Value::Type::DOUBLE:
        return Converter<double>::ToV8(isolate, val.GetDouble());
    case base::Value::Type::STRING:
        return Converter<std::string>::ToV8(isolate, val.GetString());
    case base::Value::Type::BINARY: {
        blink::CloneableMessage message;
        message.owned_encoded_message = std::move(val).TakeBlob();
        message.encoded_message = message.owned_encoded_message;
        return Converter<blink::CloneableMessage>::ToV8(isolate, message);
    }
    case base::Value::Type::LIST: {
        base::Value::List list = std::move(val).TakeList();
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        v8::Local<v8::Array> result =
            v8::Array::New(isolate, static_cast<int>(list.size()));
        for (size_t index = 0; index < list.size(); ++index) {
            auto value = Converter<base::Value>::ToV8(isolate, std::move(list[index]));
            if (value.IsEmpty() || result->CreateDataProperty(context, static_cast<uint32_t>(index), value).IsNothing())
                return v8::Local<v8::Value>();
        }
        return result;
    }
    case base::Value::Type::DICT: {
        base::Value::Dict dict = std::move(val).TakeDict();
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        v8::Local<v8::Object> result = v8::Object::New(isolate);
        for (auto it = dict.begin(); it != dict.end(); ++it) {
            auto value = Converter<base::Value>::ToV8(isolate, std::move(it->second));
            if (value.IsEmpty() || result->CreateDataProperty(
                    context, Converter<std::string>::ToV8(isolate, it->first).As<v8::String>(), value).IsNothing())
                return v8::Local<v8::Value>();
        }
        return result;
    }
    }
    NOTREACHED();
}

namespace {

bool ValueFromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, base::Value* out, unsigned depth)
{
    // base::Value is a tree: cyclic or excessively deep JavaScript graphs
    // cannot be represented; bound recursion before exhausting the native stack.
    if (val.IsEmpty() || depth > 100)
        return false;
    if (val->IsBoolean())
        *out = base::Value(val.As<v8::Boolean>()->Value());
    else if (val->IsInt32())
        *out = base::Value(val.As<v8::Int32>()->Value());
    else if (val->IsNumber()) {
        double number = val.As<v8::Number>()->Value();
        if (!std::isfinite(number))
            return false;
        *out = base::Value(number);
    }
    else if (val->IsString()) {
        v8::String::Utf8Value utf8(isolate, val);
        if (!*utf8)
            return false;
        *out = base::Value(std::string(*utf8, utf8.length()));
    } else if (val->IsNullOrUndefined())
        *out = base::Value();
    else if (val->IsArrayBuffer() || val->IsArrayBufferView()) {
        blink::CloneableMessage message;
        if (!ConvertFromV8(isolate, val, &message))
            return false;
        *out = base::Value(message.encoded_message);
    } else if (val->IsArray()) {
        auto array = val.As<v8::Array>();
        base::Value::List values;
        values.reserve(array->Length());
        for (uint32_t index = 0; index < array->Length(); ++index) {
            v8::Local<v8::Value> item;
            base::Value converted;
            if (!array->Get(isolate->GetCurrentContext(), index).ToLocal(&item)
                || !ValueFromV8(isolate, item, &converted, depth + 1))
                return false;
            values.Append(std::move(converted));
        }
        *out = base::Value(std::move(values));
    } else if (val->IsObject() && !val->IsFunction()) {
        auto object = val.As<v8::Object>();
        auto context = isolate->GetCurrentContext();
        v8::Local<v8::Array> names;
        if (!object->GetOwnPropertyNames(context, v8::SKIP_SYMBOLS, v8::KeyConversionMode::kConvertToString).ToLocal(&names))
            return false;
        base::Value::Dict values;
        for (uint32_t index = 0; index < names->Length(); ++index) {
            v8::Local<v8::Value> key;
            v8::Local<v8::Value> item;
            std::string name;
            base::Value converted;
            if (!names->Get(context, index).ToLocal(&key)
                || !Converter<std::string>::FromV8(isolate, key, &name)
                || !object->Get(context, key).ToLocal(&item)
                || !ValueFromV8(isolate, item, &converted, depth + 1))
                return false;
            values.Set(name, std::move(converted));
        }
        *out = base::Value(std::move(values));
    } else
        return false;
    return true;
}

} // namespace

bool Converter<base::Value>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, base::Value* out)
{
    return ValueFromV8(isolate, val, out, 0);
}

bool Converter<base::Value::List>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, base::Value::List* out)
{
    base::Value converted;
    if (!ValueFromV8(isolate, val, &converted, 0) || !converted.is_list())
        return false;
    *out = std::move(converted).TakeList();
    return true;
}

v8::Local<v8::String> StringToSymbol(v8::Isolate* isolate, const base::StringPiece& val)
{
    return v8::String::NewFromUtf8(isolate, val.data(), v8::NewStringType::kInternalized, static_cast<uint32_t>(val.length())).ToLocalChecked();
}

std::string V8ToString(v8::Local<v8::Value> value)
{
    if (value.IsEmpty())
        return std::string();
    std::string result;
    if (!ConvertFromV8(NULL, value, &result))
        return std::string();
    return result;
}

v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, const std::vector<intptr_t>& input)
{
    std::vector<Local<Value>> elements;
    for (size_t i = 0; i < input.size(); ++i) {
        // intptr_t is long on LP64 POSIX but long long on Win64.
        elements.push_back(Converter<int64_t>::ToV8(isolate, static_cast<int64_t>(input[i])));
    }
    v8::Local<v8::Array> ret = v8::Array::New(isolate, elements.data(), input.size());
    return ret;
}

v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, const base::Value::List& input)
{
    return Converter<base::Value::List>::ToV8(isolate, input);
}

v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, const base::Value::Dict& input)
{
    return Converter<base::Value::Dict>::ToV8(isolate, input);
}

v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, const base::Value& input)
{
    switch (input.type()) {
    case base::Value::Type::NONE:
        return v8::Null(isolate);
    case base::Value::Type::BOOLEAN:
        return Converter<bool>::ToV8(isolate, input.GetBool());
    case base::Value::Type::INTEGER:
        return Converter<int32_t>::ToV8(isolate, input.GetInt());
    case base::Value::Type::DOUBLE:
        return Converter<double>::ToV8(isolate, input.GetDouble());
    case base::Value::Type::STRING:
        return Converter<std::string>::ToV8(isolate, input.GetString());
    case base::Value::Type::LIST:
        return Converter<base::Value::List>::ToV8(isolate, input.GetList());
    case base::Value::Type::DICT:
        return Converter<base::Value::Dict>::ToV8(isolate, input.GetDict());
    case base::Value::Type::BINARY: {
        blink::CloneableMessage message;
        message.encoded_message = input.GetBlob();
        return Converter<blink::CloneableMessage>::ToV8(isolate, message);
    }
    }
    NOTREACHED();
}

v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, const blink::CloneableMessage& in)
{
    return Converter<blink::CloneableMessage>::ToV8(isolate, in);
}

v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, const std::vector<blink::CloneableMessage>& input)
{
    return Converter<std::vector<blink::CloneableMessage>>::ToV8(isolate, input);
}

v8::Local<v8::Value> Converter<std::vector<blink::CloneableMessage>>::ToV8(v8::Isolate* isolate, const std::vector<blink::CloneableMessage>& val)
{
    std::vector<v8::Local<v8::Value>> retTemp;
    retTemp.reserve(val.size());
    for (size_t i = 0; i < val.size(); ++i) {
        v8::Local<v8::Value> it = Converter<blink::CloneableMessage>::ToV8(isolate, val[i]);
        if (it.IsEmpty())
            return v8::Local<v8::Value>();
        retTemp.push_back(it);
    }

    return v8::Array::New(isolate, retTemp.data(), retTemp.size());
}

bool Converter<std::vector<blink::CloneableMessage>>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, std::vector<blink::CloneableMessage>* out)
{
    if (!val->IsArray())
        return false;
    auto array = val.As<v8::Array>();
    std::vector<blink::CloneableMessage> values;
    values.reserve(array->Length());
    for (uint32_t index = 0; index < array->Length(); ++index) {
        v8::Local<v8::Value> item;
        blink::CloneableMessage message;
        if (!array->Get(isolate->GetCurrentContext(), index).ToLocal(&item)
            || !Converter<blink::CloneableMessage>::FromV8(isolate, item, &message))
            return false;
        values.push_back(std::move(message));
    }
    *out = std::move(values);
    return true;
}


v8::Local<v8::Value> Converter<std::unique_ptr<std::vector<blink::CloneableMessage>>>::ToV8(
    v8::Isolate* isolate, 
    const std::unique_ptr<std::vector<blink::CloneableMessage>>& val)
{
    if (!val)
        return v8::Null(isolate);
    return Converter<std::vector<blink::CloneableMessage>>::ToV8(isolate, *val);
}

bool Converter<std::unique_ptr<std::vector<blink::CloneableMessage>>>::FromV8(
    v8::Isolate* isolate, 
    v8::Local<v8::Value> val, 
    std::unique_ptr<std::vector<blink::CloneableMessage>>* out)
{
    auto values = std::make_unique<std::vector<blink::CloneableMessage>>();
    if (!Converter<std::vector<blink::CloneableMessage>>::FromV8(isolate, val, values.get()))
        return false;
    *out = std::move(values);
    return true;
}

struct TranslaterHolder {
    explicit TranslaterHolder(v8::Isolate* isolate)
        : handle(isolate, v8::External::New(isolate, this))
    {
        handle.SetWeak(this, &GC, v8::WeakCallbackType::kParameter /*kFinalizer*/);
    }
    ~TranslaterHolder()
    {
        if (!handle.IsEmpty()) {
            handle.ClearWeak();
            handle.Reset();
        }
    }

    static void GC(const v8::WeakCallbackInfo<TranslaterHolder>& data)
    {
        delete data.GetParameter();
    }

    v8::Global<v8::External> handle;
    std::function<void(const v8::FunctionCallbackInfo<v8::Value>&)> translater;
};

void CallTranslater(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    TranslaterHolder* holder = (TranslaterHolder*)(v8::External::Cast(*info.Data())->Value());
    holder->translater(info);
}

v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, std::function<void(const v8::FunctionCallbackInfo<v8::Value>& info)>&& cb)
{
    TranslaterHolder* holder = new TranslaterHolder(isolate);
    holder->translater = std::move(cb);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();

    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(isolate);
    v8::Local<v8::Value> data = holder->handle.Get(isolate);
    tmpl->SetCallHandler(CallTranslater, data);
    v8::Local<v8::Function> func = tmpl->GetFunction(context).FromMaybe(v8::Local<v8::Function>());
    return func;
}

v8::Local<v8::Value> Converter<blink::CloneableMessage>::ToV8(v8::Isolate* isolate, const blink::CloneableMessage& in)
{
    return atom::deserializeV8Value(isolate, in);
}

bool Converter<blink::CloneableMessage>::FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, blink::CloneableMessage* out) 
{
    return atom::serializeV8Value(isolate, val, out);
}

} // namespace gin_helper
