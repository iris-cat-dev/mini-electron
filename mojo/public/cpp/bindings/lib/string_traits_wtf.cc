// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/public/cpp/bindings/string_traits_wtf.h"

#include <string_view>

#include "base/strings/utf_string_conversion_utils.h"
#include "mojo/public/cpp/bindings/string_data_view.h"

namespace mojo {

// static
void StringTraits<WTF::String>::SetToNull(WTF::String* output)
{
    if (output->IsNull())
        return;

    WTF::String result;
    output->swap(result);
}

// static
WTF::StringUTF8Adaptor StringTraits<WTF::String>::GetUTF8(const WTF::String& input)
{
    return WTF::StringUTF8Adaptor(WTF::StringView(input));
}

// static
bool StringTraits<WTF::String>::Read(StringDataView input, WTF::String* output)
{
    WTF::String result = WTF::String::FromUTF8(std::string_view(input.storage(), input.size()));
    if (result.IsNull())
        return false;
    output->swap(result);
    return true;
}

// static
bool StringTraits<WTF::String>::IsValidUTF8(const WTF::String& value)
{
    // WTF's 8-bit representation is Latin-1, not a buffer of UTF-8 bytes.
    if (value.IsNull() || value.Is8Bit())
        return true;

    const auto data = value.Span16();
    for (size_t i = 0; i < data.size(); ++i) {
        base_icu::UChar32 code_point;
        if (!base::ReadUnicodeCharacter(data.data(), data.size(), &i, &code_point) || !base::IsValidCharacter(code_point))
            return false;
    }
    return true;
}

} // namespace mojo
