
#ifndef content_browser_FileChooserImpl_h
#define content_browser_FileChooserImpl_h

#include <cstdint>

#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-blink.h"

namespace content {

class FileChooserImpl : public blink::mojom::blink::FileChooser {
public:
    explicit FileChooserImpl(uint64_t frame_id);

    void OpenFileChooser(
        blink::mojom::blink::FileChooserParamsPtr params,
        blink::mojom::blink::FileChooser::OpenFileChooserCallback callback)
        override;
    void EnumerateChosenDirectory(const base::FilePath& directory_path,
        blink::mojom::blink::FileChooser::EnumerateChosenDirectoryCallback
            callback) override;

private:
    uint64_t frame_id_;
};

} // namespace content

#endif
