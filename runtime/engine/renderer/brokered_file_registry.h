// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ENGINE_RENDERER_BROKERED_FILE_REGISTRY_H_
#define RUNTIME_ENGINE_RENDERER_BROKERED_FILE_REGISTRY_H_

#include <cstdint>
#include <string>
#include <functional>
#include <vector>

#include "base/values.h"
#include "base/memory/scoped_refptr.h"

#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {
class File;
class FileList;
class RawData;
class BlobDataHandle;
}

namespace content {

// Blink-thread-only registry for parent-authorized upload bytes. Broker URLs are
// opaque capabilities and never resolve to a host filesystem path.
void RegisterBrokeredFile(const std::string& broker_url,
    const std::string& display_name, std::vector<uint8_t> bytes,
    std::string authorized_path = {}, uint64_t authorized_frame_id = 0);
blink::FileList* CreateBrokeredFileList(
    const WTF::Vector<WTF::String>& broker_urls);
const blink::RawData* FindBrokeredFileBytes(
    uint64_t frame_id, const std::string& broker_url);
scoped_refptr<blink::BlobDataHandle> GetBrokeredFileBlob(
    uint64_t frame_id, const std::string& broker_url,
    const WTF::String& content_type);
void ClearBrokeredFilesForFrame(uint64_t frame_id);
void ClearBrokeredFiles();
bool GetAuthorizedPathForBrokeredFile(
    const blink::File& file, std::string* authorized_path);

using RendererFileChooserReply =
    std::function<void(base::Value::Dict, std::string)>;
using RendererFileChooserBroker = std::function<void(
    uint64_t, base::Value::Dict, RendererFileChooserReply)>;
void SetRendererFileChooserBroker(RendererFileChooserBroker broker);
void RequestRendererFileChooser(uint64_t frame_id,
    base::Value::Dict request, RendererFileChooserReply reply);

} // namespace content

#endif // RUNTIME_ENGINE_RENDERER_BROKERED_FILE_REGISTRY_H_
