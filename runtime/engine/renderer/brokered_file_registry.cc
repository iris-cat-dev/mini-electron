// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/engine/renderer/brokered_file_registry.h"

#include <map>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "third_party/blink/renderer/core/fileapi/file.h"
#include "third_party/blink/renderer/core/fileapi/file_list.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/file_metadata.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"

namespace content {
namespace {

struct BrokeredFile {
    std::string display_name;
    scoped_refptr<blink::RawData> bytes;
    scoped_refptr<blink::BlobDataHandle> blob;
    std::string authorized_path;
    uint64_t authorized_frame_id = 0;
};

std::map<std::string, BrokeredFile>& Files()
{
    static base::NoDestructor<std::map<std::string, BrokeredFile>> files;
    return *files;
}

RendererFileChooserBroker& FileChooserBroker()
{
    static base::NoDestructor<RendererFileChooserBroker> broker;
    return *broker;
}

} // namespace

void RegisterBrokeredFile(const std::string& broker_url,
    const std::string& display_name, std::vector<uint8_t> bytes,
    std::string authorized_path, uint64_t authorized_frame_id)
{
    if (broker_url.rfind("mini-electron-broker://", 0) != 0)
        return;
    auto data = blink::RawData::Create();
    data->MutableData()->AppendSpan(base::span(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    Files()[broker_url] = {
        display_name, std::move(data), {}, std::move(authorized_path),
        authorized_frame_id
    };
}

blink::FileList* CreateBrokeredFileList(
    const WTF::Vector<WTF::String>& broker_urls)
{
    auto* list = blink::MakeGarbageCollected<blink::FileList>();
    blink::FileMetadata metadata;
    metadata.type = blink::FileMetadata::kTypeFile;
    metadata.modification_time = base::Time::Now();
    for (const WTF::String& url : broker_urls) {
        std::string key = url.Utf8();
        auto found = Files().find(key);
        if (found == Files().end())
            return nullptr;
        metadata.length = found->second.bytes->size();
        auto blob = GetBrokeredFileBlob(
            found->second.authorized_frame_id, key, WTF::g_empty_string);
        if (!blob)
            return nullptr;
        list->Append(blink::File::CreateForFileSystemFile(
            blink::KURL(url), metadata, blink::File::kIsUserVisible,
            std::move(blob)));
    }
    return list;
}

const blink::RawData* FindBrokeredFileBytes(
    uint64_t frame_id, const std::string& broker_url)
{
    auto found = Files().find(broker_url);
    if (found == Files().end() || !frame_id
        || found->second.authorized_frame_id != frame_id)
        return nullptr;
    return found->second.bytes.get();
}

scoped_refptr<blink::BlobDataHandle> GetBrokeredFileBlob(
    uint64_t frame_id, const std::string& broker_url,
    const WTF::String& content_type)
{
    const blink::RawData* bytes = FindBrokeredFileBytes(frame_id, broker_url);
    if (!bytes)
        return nullptr;
    BrokeredFile& file = Files().find(broker_url)->second;
    if (!file.blob) {
        auto data = std::make_unique<blink::BlobData>();
        data->SetContentType(content_type);
        data->AppendData(file.bytes);
        file.blob = blink::BlobDataHandle::Create(
            std::move(data), bytes->size());
    }
    return file.blob;
}

void ClearBrokeredFilesForFrame(uint64_t frame_id)
{
    for (auto found = Files().begin(); found != Files().end();) {
        if (found->second.authorized_frame_id == frame_id)
            found = Files().erase(found);
        else
            ++found;
    }
}

void ClearBrokeredFiles()
{
    Files().clear();
}


bool GetAuthorizedPathForBrokeredFile(
    const blink::File& file, std::string* authorized_path)
{
    if (!authorized_path || file.FileSystemURL().IsEmpty())
        return false;
    auto found = Files().find(file.FileSystemURL().GetString().Utf8());
    if (found == Files().end() || found->second.authorized_path.empty())
        return false;
    *authorized_path = found->second.authorized_path;
    return true;
}

void SetRendererFileChooserBroker(RendererFileChooserBroker broker)
{
    FileChooserBroker() = std::move(broker);
}

void RequestRendererFileChooser(uint64_t frame_id,
    base::Value::Dict request, RendererFileChooserReply reply)
{
    if (!frame_id || !FileChooserBroker()) {
        reply({}, !frame_id ? "file chooser has no requesting frame"
                            : "file chooser broker is unavailable");
        return;
    }
    FileChooserBroker()(frame_id, std::move(request), std::move(reply));
}

} // namespace content
