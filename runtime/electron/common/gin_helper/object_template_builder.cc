// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "runtime/electron/common/gin_helper/object_template_builder.h"

#include <stdint.h>

#include "runtime/electron/common/gin_helper/interceptor.h"
#include "runtime/electron/common/gin_helper/per_isolate_data.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"

namespace gin_helper {

namespace {

WrappableBase* WrappableFromV8(v8::Isolate* isolate, v8::Local<v8::Value> val)
{
    if (!val->IsObject())
        return NULL;
    v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(val);
    WrapperInfo* info = WrapperInfo::From(obj);

    // If this fails, the object is not managed by Gin.
    if (!info)
        return NULL;

    // We don't further validate the type of the object, but assume it's derived
    // from WrappableBase. We look up the pointer in a global registry, to make
    // sure it's actually pointed to a valid life object.
    return static_cast<WrappableBase*>(obj->GetAlignedPointerFromInternalField(gin_helper::InternalFields::kEncodedValueIndex));
}

NamedPropertyInterceptor* NamedInterceptorFromV8(v8::Isolate* isolate, v8::Local<v8::Value> val)
{
    WrappableBase* base = WrappableFromV8(isolate, val);
    if (!base)
        return NULL;
    return PerIsolateData::From(isolate)->GetNamedPropertyInterceptor(base);
}

IndexedPropertyInterceptor* IndexedInterceptorFromV8(v8::Isolate* isolate, v8::Local<v8::Value> val)
{
    WrappableBase* base = WrappableFromV8(isolate, val);
    if (!base)
        return NULL;
    return PerIsolateData::From(isolate)->GetIndexedPropertyInterceptor(base);
}

v8::Intercepted NamedPropertyGetter(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    NamedPropertyInterceptor* interceptor = NamedInterceptorFromV8(isolate, info.Holder());
    std::string name;
    if (!interceptor || !property->IsString() || !ConvertFromV8(isolate, property, &name))
        return v8::Intercepted::kNo;
    auto value = interceptor->GetNamedProperty(isolate, name);
    if (value.IsEmpty())
        return v8::Intercepted::kNo;
    info.GetReturnValue().Set(value);
    return v8::Intercepted::kYes;
}

v8::Intercepted NamedPropertySetter(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    NamedPropertyInterceptor* interceptor = NamedInterceptorFromV8(isolate, info.Holder());
    std::string name;
    if (!interceptor || !property->IsString() || !ConvertFromV8(isolate, property, &name))
        return v8::Intercepted::kNo;
    return interceptor->SetNamedProperty(isolate, name, value) ? v8::Intercepted::kYes : v8::Intercepted::kNo;
}

v8::Intercepted NamedPropertyQuery(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Integer>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    NamedPropertyInterceptor* interceptor = NamedInterceptorFromV8(isolate, info.Holder());
    std::string name;
    if (!interceptor || !property->IsString() || !ConvertFromV8(isolate, property, &name)
        || interceptor->GetNamedProperty(isolate, name).IsEmpty())
        return v8::Intercepted::kNo;
    info.GetReturnValue().Set(0);
    return v8::Intercepted::kYes;
}

void NamedPropertyEnumerator(const v8::PropertyCallbackInfo<v8::Array>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    NamedPropertyInterceptor* interceptor = NamedInterceptorFromV8(isolate, info.Holder());
    if (!interceptor)
        return;
    v8::Local<v8::Value> properties;
    if (!TryConvertToV8(isolate, interceptor->EnumerateNamedProperties(isolate), &properties))
        return;
    info.GetReturnValue().Set(v8::Local<v8::Array>::Cast(properties));
}

v8::Intercepted IndexedPropertyGetter(uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    IndexedPropertyInterceptor* interceptor = IndexedInterceptorFromV8(isolate, info.Holder());
    if (!interceptor)
        return v8::Intercepted::kNo;
    auto value = interceptor->GetIndexedProperty(isolate, index);
    if (value.IsEmpty())
        return v8::Intercepted::kNo;
    info.GetReturnValue().Set(value);
    return v8::Intercepted::kYes;
}

v8::Intercepted IndexedPropertySetter(uint32_t index, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    IndexedPropertyInterceptor* interceptor = IndexedInterceptorFromV8(isolate, info.Holder());
    return interceptor && interceptor->SetIndexedProperty(isolate, index, value)
        ? v8::Intercepted::kYes : v8::Intercepted::kNo;
}

void IndexedPropertyEnumerator(const v8::PropertyCallbackInfo<v8::Array>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    IndexedPropertyInterceptor* interceptor = IndexedInterceptorFromV8(isolate, info.Holder());
    if (!interceptor)
        return;
    v8::Local<v8::Value> properties;
    if (!TryConvertToV8(isolate, interceptor->EnumerateIndexedProperties(isolate), &properties))
        return;
    info.GetReturnValue().Set(v8::Local<v8::Array>::Cast(properties));
}

} // namespace

ObjectTemplateBuilder::ObjectTemplateBuilder(v8::Isolate* isolate)
    : isolate_(isolate)
    , template_(v8::ObjectTemplate::New(isolate))
{
    template_->SetInternalFieldCount(gin_helper::InternalFields::kNumberOfInternalFields);
}

ObjectTemplateBuilder::ObjectTemplateBuilder(v8::Isolate* isolate, v8::Local<v8::ObjectTemplate> objectTemplate)
    : isolate_(isolate)
    , template_(objectTemplate)
{
    template_->SetInternalFieldCount(gin_helper::InternalFields::kNumberOfInternalFields);
}

ObjectTemplateBuilder::ObjectTemplateBuilder(const ObjectTemplateBuilder& other) = default;

ObjectTemplateBuilder::~ObjectTemplateBuilder()
{
}

ObjectTemplateBuilder& ObjectTemplateBuilder::AddNamedPropertyInterceptor()
{
    template_->SetHandler(v8::NamedPropertyHandlerConfiguration(
        &NamedPropertyGetter, &NamedPropertySetter, &NamedPropertyQuery, nullptr, &NamedPropertyEnumerator));
    return *this;
}

ObjectTemplateBuilder& ObjectTemplateBuilder::AddIndexedPropertyInterceptor()
{
    template_->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        &IndexedPropertyGetter, &IndexedPropertySetter, nullptr, nullptr, &IndexedPropertyEnumerator));
    return *this;
}

ObjectTemplateBuilder& ObjectTemplateBuilder::SetImpl(const base::StringPiece& name, v8::Local<v8::Data> val)
{
    template_->Set(StringToSymbol(isolate_, name), val);
    return *this;
}

ObjectTemplateBuilder& ObjectTemplateBuilder::SetPropertyImpl(
    const base::StringPiece& name, v8::Local<v8::FunctionTemplate> getter, v8::Local<v8::FunctionTemplate> setter)
{
    template_->SetAccessorProperty(StringToSymbol(isolate_, name), getter, setter);
    return *this;
}

v8::Local<v8::ObjectTemplate> ObjectTemplateBuilder::Build()
{
    v8::Local<v8::ObjectTemplate> result = template_;
    template_.Clear();
    return result;
}

} // namespace gin
