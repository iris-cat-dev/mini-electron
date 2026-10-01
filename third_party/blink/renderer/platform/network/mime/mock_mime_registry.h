// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_NETWORK_MIME_MOCK_MIME_REGISTRY_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_NETWORK_MIME_MOCK_MIME_REGISTRY_H_

#include "net/base/mime_util.h"
#include "third_party/blink/public/mojom/mime/mime_registry.mojom-blink.h"
#include "third_party/blink/public/platform/file_path_conversion.h"

namespace blink {

// Used for unit tests.
class MockMimeRegistry : public mojom::blink::MimeRegistry {
public:
    MockMimeRegistry() = default;
    ~MockMimeRegistry() override = default;

    bool GetMimeTypeFromExtension(const String& ext, String* out_mime_type) override
    {
        std::string mime_type;
        bool found = net::GetMimeTypeFromExtension(WebStringToFilePath(ext).value(), &mime_type);
        *out_mime_type = String::FromUTF8(mime_type);
        return found;
    }

    void GetMimeTypeFromExtension(const String& ext, GetMimeTypeFromExtensionCallback callback) override
    {
        String mime_type;
        GetMimeTypeFromExtension(ext, &mime_type);
        std::move(callback).Run(mime_type);
    }
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_PLATFORM_NETWORK_MIME_MOCK_MIME_REGISTRY_H_
