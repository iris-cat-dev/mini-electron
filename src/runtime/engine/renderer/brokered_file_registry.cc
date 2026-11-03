// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "src/runtime/engine/renderer/brokered_file_registry.h"

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
#include "src/runtime/engine/renderer/blob_registry_impl.h"
#include "third_party/blink/renderer/platform/network/mime/mime_type_registry.h"

namespace content {
namespace {

struct BrokeredFile {
    std::string display_name;
    scoped_refptr<blink::RawData> bytes;
    scoped_refptr<blink::BlobDataHandle> blob;
    std::string authorized_path;
    uint64_t authorized_frame_id = 0;
    bool is_directory = false;
    bool is_clipboard = false;
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

RendererClipboardFileBroker& ClipboardFileBroker()
{
    static base::NoDestructor<RendererClipboardFileBroker> broker;
    return *broker;
}

thread_local ClipboardInputState clipboard_input;
thread_local uint64_t clipboard_read_frame = 0;

} // namespace

void RegisterBrokeredFile(const std::string& broker_url,
    const std::string& display_name, std::vector<uint8_t> bytes,
    std::string authorized_path, uint64_t authorized_frame_id,
    bool is_directory)
{
    if (broker_url.rfind("mini-electron-broker://", 0) != 0)
        return;
    auto data = blink::RawData::Create();
    data->MutableData()->AppendSpan(base::span(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    Files()[broker_url] = {
        display_name, std::move(data), {}, std::move(authorized_path),
        authorized_frame_id, is_directory
    };
}

void RegisterClipboardFile(const std::string& broker_url,
    const std::string& display_name, std::vector<uint8_t> bytes,
    std::string authorized_path, uint64_t frame_id)
{
    const WTF::String name = WTF::String::FromUTF8(display_name);
    const auto dot = name.ReverseFind('.');
    WTF::String type = dot == WTF::kNotFound ? WTF::g_empty_string
        : blink::MIMETypeRegistry::GetWellKnownMIMETypeForExtension(name.Substring(dot + 1));
    if (type.IsNull())
        type = WTF::g_empty_string;
    auto blob = CreateOwnedBytesBlob(std::move(bytes), type);
    Files()[broker_url] = {display_name, {}, std::move(blob),
        std::move(authorized_path), frame_id, false, true};
}

void SetClipboardFileAuthority(blink::File& file, blink::ExecutionContext& context,
    uint64_t frame_id, const std::string& broker_url)
{
    auto found = Files().find(broker_url);
    if (found != Files().end() && found->second.is_clipboard
        && found->second.authorized_frame_id == frame_id)
        file.SetClipboardAuthority(&context, frame_id,
            WTF::String::FromUTF8(found->second.authorized_path));
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
        if (found == Files().end() || found->second.is_directory)
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
    auto found = Files().find(broker_url);
    if (found == Files().end() || !frame_id
        || found->second.authorized_frame_id != frame_id)
        return nullptr;
    BrokeredFile& file = found->second;
    if (file.blob)
        return file.blob;
    const auto* bytes = file.bytes.get();
    if (!file.blob) {
        auto data = std::make_unique<blink::BlobData>();
        data->SetContentType(content_type);
        data->AppendData(file.bytes);
        file.blob = blink::BlobDataHandle::Create(
            std::move(data), bytes->size());
    }
    return file.blob;
}
void UnregisterBrokeredFile(const std::string& broker_url)
{
    Files().erase(broker_url);
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
    uint64_t frame_id, const blink::File& file, std::string* authorized_path)
{
    if (!frame_id || !authorized_path || file.FileSystemURL().IsEmpty())
        return false;
    const WTF::String clipboard_path = file.ClipboardAuthorizedPath(frame_id);
    if (!clipboard_path.empty()) {
        *authorized_path = clipboard_path.Utf8();
        return true;
    }
    auto found = Files().find(file.FileSystemURL().GetString().Utf8());
    if (found == Files().end() || found->second.authorized_path.empty()
        || found->second.authorized_frame_id != frame_id)
        return false;
    *authorized_path = found->second.authorized_path;
    return true;
}

bool GetBrokeredFileDirectory(
    const blink::File& file, bool* is_directory)
{
    if (!is_directory || file.FileSystemURL().IsEmpty())
        return false;
    auto found = Files().find(file.FileSystemURL().GetString().Utf8());
    if (found == Files().end())
        return false;
    *is_directory = found->second.is_directory;
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

void SetRendererClipboardFileBroker(RendererClipboardFileBroker broker)
{
    ClipboardFileBroker() = std::move(broker);
}

bool ReadRendererClipboardFiles(uint64_t frame_id, base::Value::Dict* result)
{
    if (!RendererClipboardFileReadAllowed(frame_id) || !result || !ClipboardFileBroker())
        return false;
    clipboard_input.attempted_read = true;
    return ClipboardFileBroker()(frame_id, "readFiles", result);
}

bool RendererClipboardFileReadAllowed(uint64_t frame_id)
{
    return frame_id && clipboard_input.frame_id == frame_id
        && clipboard_read_frame == frame_id;
}

ScopedRendererClipboardInput::ScopedRendererClipboardInput(uint64_t frame_id, bool paste)
    : paste_(paste), previous_(clipboard_input)
{
    if (paste_)
        clipboard_input = {frame_id, false};
}

ScopedRendererClipboardInput::~ScopedRendererClipboardInput()
{
    if (!paste_)
        return;
    if (!clipboard_input.attempted_read && ClipboardFileBroker()) {
        base::Value::Dict ignored;
        ClipboardFileBroker()(clipboard_input.frame_id, "finishFiles", &ignored);
    }
    for (auto found = Files().begin(); found != Files().end();) {
        if (found->second.is_clipboard
            && found->second.authorized_frame_id == clipboard_input.frame_id)
            found = Files().erase(found);
        else
            ++found;
    }
    clipboard_input = previous_;
}

ScopedRendererClipboardFileRead::ScopedRendererClipboardFileRead(uint64_t frame_id, bool allowed)
    : reset_(&clipboard_read_frame, allowed ? frame_id : 0)
{
}

} // namespace content
