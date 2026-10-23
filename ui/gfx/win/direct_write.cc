// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gfx/win/direct_write.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/debug/alias.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/ports/SkTypeface_win.h"

#pragma comment(lib, "dwrite.lib")

namespace gfx::win {

namespace {

// DirectWrite and Skia retain this process-wide factory for their lifetimes.
IDWriteFactory* g_direct_write_factory = nullptr;

void SetDirectWriteFactory(IDWriteFactory* factory)
{
    CHECK(!g_direct_write_factory);
    factory->AddRef();
    g_direct_write_factory = factory;
}

} // namespace

void CreateDWriteFactory(IDWriteFactory** factory)
{
    CHECK(factory);
    *factory = nullptr;

    Microsoft::WRL::ComPtr<IUnknown> factory_unknown;
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &factory_unknown);
    if (FAILED(hr)) {
        base::debug::Alias(&hr);
        NOTREACHED();
    }

    factory_unknown.CopyTo(factory);
}

void InitializeDirectWrite()
{
    CHECK(!g_direct_write_factory);

    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    CreateDWriteFactory(&factory);
    CHECK(factory);
    SetDirectWriteFactory(factory.Get());

    sk_sp<SkFontMgr> direct_write_font_manager =
        SkFontMgr_New_DirectWrite(factory.Get());
    CHECK(direct_write_font_manager);
    skia::OverrideDefaultSkFontMgr(std::move(direct_write_font_manager));
}

IDWriteFactory* GetDirectWriteFactory()
{
    if (!g_direct_write_factory)
        InitializeDirectWrite();
    return g_direct_write_factory;
}

std::optional<std::string> RetrieveLocalizedString(
    IDWriteLocalizedStrings* names,
    const std::string& locale)
{
    CHECK(names);

    UINT32 index = 0;
    BOOL exists = FALSE;
    if (!locale.empty()) {
        const std::wstring locale_wide = base::UTF8ToWide(locale);
        if (FAILED(names->FindLocaleName(locale_wide.c_str(), &index, &exists)) ||
            !exists) {
            return std::nullopt;
        }
    }

    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length)))
        return std::nullopt;

    std::wstring buffer(length + 1, L'\0');
    if (FAILED(names->GetString(index, buffer.data(),
                                static_cast<UINT32>(buffer.size())))) {
        return std::nullopt;
    }

    buffer.resize(length);
    return base::WideToUTF8(buffer);
}

std::optional<std::string> RetrieveLocalizedFontName(
    std::string_view font_name,
    const std::string& locale)
{
    Microsoft::WRL::ComPtr<IDWriteFontCollection> font_collection;
    if (FAILED(GetDirectWriteFactory()->GetSystemFontCollection(
            &font_collection))) {
        return std::nullopt;
    }

    const std::wstring font_name_wide = base::UTF8ToWide(font_name);
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(font_collection->FindFamilyName(font_name_wide.c_str(), &index,
                                                &exists)) ||
        !exists) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IDWriteFontFamily> font_family;
    Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> family_names;
    if (FAILED(font_collection->GetFontFamily(index, &font_family)) ||
        FAILED(font_family->GetFamilyNames(&family_names))) {
        return std::nullopt;
    }

    return RetrieveLocalizedString(family_names.Get(), locale);
}

} // namespace gfx::win
