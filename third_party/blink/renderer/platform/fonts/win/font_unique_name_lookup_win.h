// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_WIN_FONT_UNIQUE_NAME_LOOKUP_WIN_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_WIN_FONT_UNIQUE_NAME_LOOKUP_WIN_H_

#if !defined(MINI_UI_QUICKJS)
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/common/font_unique_name_lookup/font_table_matcher.h"
#include "third_party/blink/public/mojom/dwrite_font_proxy/dwrite_font_proxy.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/deque.h"
#endif
#include "third_party/blink/renderer/platform/fonts/font_unique_name_lookup.h"

namespace blink {

#if defined(MINI_UI_QUICKJS)
class NativeFontUniqueNameLookup;
#endif
#if defined(MINI_UI_QUICKJS)
// Resolves installed full and PostScript font names directly through the
// process' native DirectWrite font collection.
#else
// Performs the IPC towards the browser process for font unique name
// matching. Direct individual sync Mojo IPC calls are made to lookup fonts,
// and the class reponds synchronously.
#endif
class FontUniqueNameLookupWin : public FontUniqueNameLookup {
public:
    FontUniqueNameLookupWin();
    FontUniqueNameLookupWin(const FontUniqueNameLookupWin&) = delete;
    FontUniqueNameLookupWin& operator=(const FontUniqueNameLookupWin&) = delete;
    ~FontUniqueNameLookupWin() override;
    sk_sp<SkTypeface> MatchUniqueName(const String& font_unique_name) override;

    bool IsFontUniqueNameLookupReadyForSyncLookup() override;

#if defined(MINI_UI_QUICKJS)
    void PrepareFontUniqueNameLookup(NotifyFontUniqueNameLookupReady callback) override;
#else
    void Init() override;
#endif

private:
#if defined(MINI_UI_QUICKJS)
    bool EnsureNativeLookup();

    std::unique_ptr<NativeFontUniqueNameLookup> native_lookup_;
#else
    void EnsureServiceConnected();

    sk_sp<SkTypeface> MatchUniqueNameSingleLookup(const String& font_unique_name);

    sk_sp<SkTypeface> InstantiateFromFileAndTtcIndex(base::File file_handle, uint32_t ttc_index);

    mojo::Remote<mojom::blink::DWriteFontProxy> service_;
#endif
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_WIN_FONT_UNIQUE_NAME_LOOKUP_WIN_H_
