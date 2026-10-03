#include "runtime/engine/browser/file_chooser_impl.h"

#include <optional>
#include <memory>
#include <string>
#include <utility>

#include "base/time/time.h"
#include "runtime/engine/renderer/brokered_file_registry.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"

namespace content {
namespace {

std::string ChooserMode(blink::mojom::FileChooserParams_Mode mode)
{
    switch (mode) {
    case blink::mojom::FileChooserParams_Mode::kOpenMultiple:
        return "openMultiple";
    case blink::mojom::FileChooserParams_Mode::kUploadFolder:
        return "folder";
    default:
        return "open";
    }
}

blink::mojom::blink::FileChooserResultPtr EmptyResult()
{
    return blink::mojom::blink::FileChooserResult::New();
}

} // namespace

FileChooserImpl::FileChooserImpl(uint64_t frame_id)
    : frame_id_(frame_id)
{
}

void FileChooserImpl::OpenFileChooser(
    blink::mojom::blink::FileChooserParamsPtr params,
    blink::mojom::blink::FileChooser::OpenFileChooserCallback callback)
{
    if (!params || !frame_id_) {
        std::move(callback).Run({});
        return;
    }

    base::Value::Dict request;
    request.Set("mode", ChooserMode(params->mode));
    request.Set("title", params->title.Utf8());
    base::Value::List accept_types;
    for (const auto& accept_type : params->accept_types)
        accept_types.Append(accept_type.Utf8());
    request.Set("acceptTypes", std::move(accept_types));

    auto pending = std::make_shared<
        blink::mojom::blink::FileChooser::OpenFileChooserCallback>(
        std::move(callback));
    RequestRendererFileChooser(frame_id_, std::move(request),
        [folder = params->mode == blink::mojom::blink::FileChooserParams::Mode::kUploadFolder,
            callback = std::move(pending)](base::Value::Dict result,
            std::string error) mutable {
            if (!*callback)
                return;
            auto output = EmptyResult();
            if (folder)
                output->base_directory = base::FilePath(FILE_PATH_LITERAL("/"));
            if (!error.empty()
                || result.FindBool("canceled").value_or(false)) {
                std::move(*callback).Run({});
                return;
            }
            base::Value::List* files = result.FindList("files");
            if (!files) {
                std::move(*callback).Run({});
                return;
            }
            for (const base::Value& value : *files) {
                const base::Value::Dict* file = value.GetIfDict();
                const std::string* broker_url =
                    file ? file->FindString("brokerUrl") : nullptr;
                std::optional<int> size =
                    file ? file->FindInt("size") : std::nullopt;
                if (!broker_url || broker_url->empty() || !size || *size < 0)
                    continue;
                double last_modified =
                    file->FindDouble("lastModified").value_or(0);
                auto system_file =
                    blink::mojom::blink::FileSystemFileInfo::New(
                        blink::KURL(WTF::String::FromUTF8(*broker_url)),
                        base::Time::FromMillisecondsSinceUnixEpoch(
                            last_modified),
                        *size);
                output->files.push_back(
                    blink::mojom::blink::FileChooserFileInfo::NewFileSystem(
                        std::move(system_file)));
            }
            std::move(*callback).Run(std::move(output));
        });
}

void FileChooserImpl::EnumerateChosenDirectory(
    const base::FilePath&,
    blink::mojom::blink::FileChooser::EnumerateChosenDirectoryCallback
        callback)
{
    std::move(callback).Run({});
}

} // namespace content
