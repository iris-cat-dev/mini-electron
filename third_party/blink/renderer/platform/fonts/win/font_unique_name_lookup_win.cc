// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/fonts/win/font_unique_name_lookup_win.h"

#if defined(MINI_UI_QUICKJS)
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "third_party/blink/public/common/font_unique_name_lookup/icu_fold_case_util.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/ports/SkTypeface_win.h"
#else
#include <memory>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/metrics/histogram_macros.h"
#include "skia/ext/font_utils.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "third_party/blink/public/mojom/dwrite_font_proxy/dwrite_font_proxy.mojom-blink.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/ports/SkTypeface_win.h"
#endif

namespace blink {

#if defined(MINI_UI_QUICKJS)

namespace {

struct UniqueFontEntry {
    std::string folded_name;
    UINT32 family_index;
    UINT32 font_index;
};

void AppendInformationalNames(IDWriteFont* font,
    DWRITE_INFORMATIONAL_STRING_ID name_id,
    UINT32 family_index,
    UINT32 font_index,
    std::vector<UniqueFontEntry>* entries)
{
    Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
    BOOL names_exist = FALSE;
    if (FAILED(font->GetInformationalStrings(name_id, &names, &names_exist)) || !names_exist)
        return;

    const UINT32 name_count = names->GetCount();
    std::wstring name;
    for (UINT32 name_index = 0; name_index < name_count; ++name_index) {
        UINT32 name_length = 0;
        if (FAILED(names->GetStringLength(name_index, &name_length)) || !name_length)
            continue;

        name.resize(name_length + 1);
        if (FAILED(names->GetString(name_index, name.data(), name_length + 1)))
            continue;
        name.resize(name_length);

        std::string folded_name = IcuFoldCase(base::WideToUTF8(name));
        if (!folded_name.empty())
            entries->push_back({ std::move(folded_name), family_index, font_index });
    }
}

} // namespace

class NativeFontUniqueNameLookup {
public:
    bool Initialize()
    {
        if (font_manager_)
            return true;

        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
        HRESULT result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
        if (FAILED(result))
            return false;

        Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
        result = factory->GetSystemFontCollection(&collection, FALSE);
        if (FAILED(result))
            return false;

        sk_sp<SkFontMgr> font_manager = SkFontMgr_New_DirectWrite(factory.Get(), collection.Get());
        if (!font_manager)
            return false;

        std::vector<UniqueFontEntry> entries;
        const UINT32 family_count = collection->GetFontFamilyCount();
        for (UINT32 family_index = 0; family_index < family_count; ++family_index) {
            Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
            if (FAILED(collection->GetFontFamily(family_index, &family)))
                continue;

            const UINT32 font_count = family->GetFontCount();
            for (UINT32 font_index = 0; font_index < font_count; ++font_index) {
                Microsoft::WRL::ComPtr<IDWriteFont> font;
                if (FAILED(family->GetFont(font_index, &font)))
                    continue;

                AppendInformationalNames(font.Get(), DWRITE_INFORMATIONAL_STRING_FULL_NAME,
                    family_index, font_index, &entries);
                AppendInformationalNames(font.Get(), DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME,
                    family_index, font_index, &entries);
            }
        }

        std::sort(entries.begin(), entries.end(), [](const UniqueFontEntry& left, const UniqueFontEntry& right) {
            if (left.folded_name != right.folded_name)
                return left.folded_name < right.folded_name;
            if (left.family_index != right.family_index)
                return left.family_index < right.family_index;
            return left.font_index < right.font_index;
        });
        entries.erase(std::unique(entries.begin(), entries.end(),
                          [](const UniqueFontEntry& left, const UniqueFontEntry& right) {
                              return left.folded_name == right.folded_name;
                          }),
            entries.end());

        entries_ = std::move(entries);
        font_manager_ = std::move(font_manager);
        return true;
    }

    sk_sp<SkTypeface> Match(const String& font_unique_name)
    {
        if (!font_manager_ || font_unique_name.empty())
            return nullptr;

        std::string utf8_name = font_unique_name.Utf8(WTF::kStrictUTF8Conversion);
        if (utf8_name.empty())
            return nullptr;
        const std::string folded_name = IcuFoldCase(utf8_name);

        auto match = std::lower_bound(entries_.begin(), entries_.end(), folded_name,
            [](const UniqueFontEntry& entry, const std::string& name) {
                return entry.folded_name < name;
            });
        if (match == entries_.end() || match->folded_name != folded_name)
            return nullptr;

        sk_sp<SkFontStyleSet> style_set = font_manager_->createStyleSet(match->family_index);
        if (!style_set || match->font_index >= static_cast<UINT32>(style_set->count()))
            return nullptr;
        return style_set->createTypeface(match->font_index);
    }

private:
    sk_sp<SkFontMgr> font_manager_;
    std::vector<UniqueFontEntry> entries_;
};

FontUniqueNameLookupWin::FontUniqueNameLookupWin() = default;

FontUniqueNameLookupWin::~FontUniqueNameLookupWin() = default;

bool FontUniqueNameLookupWin::EnsureNativeLookup()
{
    if (!native_lookup_)
        native_lookup_ = std::make_unique<NativeFontUniqueNameLookup>();
    return native_lookup_->Initialize();
}

sk_sp<SkTypeface> FontUniqueNameLookupWin::MatchUniqueName(const String& font_unique_name)
{
    if (!EnsureNativeLookup())
        return nullptr;
    return native_lookup_->Match(font_unique_name);
}

bool FontUniqueNameLookupWin::IsFontUniqueNameLookupReadyForSyncLookup()
{
    return EnsureNativeLookup();
}

void FontUniqueNameLookupWin::PrepareFontUniqueNameLookup(NotifyFontUniqueNameLookupReady callback)
{
    if (EnsureNativeLookup())
        std::move(callback).Run();
}

#else

FontUniqueNameLookupWin::FontUniqueNameLookupWin() = default;

FontUniqueNameLookupWin::~FontUniqueNameLookupWin() = default;

sk_sp<SkTypeface> FontUniqueNameLookupWin::MatchUniqueName(const String& font_unique_name)
{
    return MatchUniqueNameSingleLookup(font_unique_name);
}

sk_sp<SkTypeface> FontUniqueNameLookupWin::MatchUniqueNameSingleLookup(const String& font_unique_name)
{
    base::File font_file;
    uint32_t ttc_index = 0;

    EnsureServiceConnected();

    bool matching_mojo_success = service_->MatchUniqueFont(font_unique_name, &font_file, &ttc_index);
    DCHECK(matching_mojo_success);

    return InstantiateFromFileAndTtcIndex(std::move(font_file), ttc_index);
}

// Used for font matching with single lookup case only.
sk_sp<SkTypeface> FontUniqueNameLookupWin::InstantiateFromFileAndTtcIndex(base::File file_handle, uint32_t ttc_index)
{
    FILE* cfile = base::FileToFILE(std::move(file_handle), "rb");
    if (!cfile) {
        return nullptr;
    }
    auto data = SkData::MakeFromFILE(cfile);
    base::CloseFile(cfile);
    if (!data) {
        return nullptr;
    }
    sk_sp<SkFontMgr> mgr = skia::DefaultFontMgr();
    return mgr->makeFromData(std::move(data), ttc_index);
}

bool FontUniqueNameLookupWin::IsFontUniqueNameLookupReadyForSyncLookup()
{
    if (RuntimeEnabledFeatures::FontSrcLocalMatchingEnabled()) {
        EnsureServiceConnected();
    }

    return true;
}

void FontUniqueNameLookupWin::EnsureServiceConnected()
{
    if (service_)
        return;
    Platform::Current()->GetBrowserInterfaceBroker()->GetInterface(service_.BindNewPipeAndPassReceiver());
}

void FontUniqueNameLookupWin::Init()
{
    if (!base::FeatureList::IsEnabled(features::kPrefetchFontLookupTables))
        return;

    EnsureServiceConnected();
}

#endif

} // namespace blink
