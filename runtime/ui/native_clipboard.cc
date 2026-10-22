// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/native_clipboard.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_type.h"
#include "base/no_destructor.h"
#include "base/pickle.h"
#include "base/sequence_checker.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/unique_receiver_set.h"
#include "runtime/ui/local_resources.h"
#include "third_party/blink/public/mojom/clipboard/clipboard.mojom.h"
#include "third_party/blink/public/mojom/data_transfer/data_transfer.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_constants.h"
#include "ui/base/clipboard/clipboard_format_type.h"
#include "ui/base/clipboard/custom_data_helper.h"
#include "ui/base/clipboard/file_info.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "url/gurl.h"

namespace mini_ui {
namespace {

constexpr uint32_t kUnsupportedBuffer = 1;
constexpr uint32_t kInvalidClipboardData = 2;

static_assert(ui::kMaxWebCustomFormatSize
    == blink::mojom::ClipboardHost::kMaxFormatSize);

// Unsanitized format names are ASCII MIME strings. ScopedClipboardWriter uses
// UTF16ToASCII, and a NUL would give the OS a different name from the wire name.
bool IsValidCustomFormat(const std::u16string& format)
{
    if (format.empty() || format.size() >= ui::kMaxWebCustomFormatSize) {
        return false;
    }
    for (char16_t character : format) {
        if (character == 0 || character > 0x7f)
            return false;
    }
    return true;
}

// Account for Pickle's signed string length and four-byte padding before
// allocating or serializing the DataTransfer custom-data payload.
bool AddPickledStringSize(size_t length, size_t* total)
{
    constexpr size_t limit = blink::mojom::ClipboardHost::kMaxDataSize;
    if (length >= (limit - 8) / sizeof(char16_t))
        return false;
    const size_t bytes = (length * sizeof(char16_t) + 3) & ~size_t { 3 };
    if (*total >= limit - sizeof(int32_t) - bytes)
        return false;
    *total += sizeof(int32_t) + bytes;
    return true;
}

class NativeClipboardHost final : public blink::mojom::ClipboardHost {
public:
    using CloseReceiver = base::RepeatingCallback<void(uint32_t, const std::string&)>;

    NativeClipboardHost(std::shared_ptr<LocalResources> resources,
        CloseReceiver close_receiver)
        : resources_(std::move(resources))
        , close_receiver_(std::move(close_receiver))
    {
    }

    ~NativeClipboardHost() override
    {
        AbortWrite();
    }

    void GetSequenceNumber(ui::ClipboardBuffer buffer,
        GetSequenceNumberCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        std::move(callback).Run(ui::Clipboard::GetForCurrentThread()->GetSequenceNumber(buffer));
    }

    void IsFormatAvailable(blink::mojom::ClipboardFormat format,
        ui::ClipboardBuffer buffer, IsFormatAvailableCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        const ui::ClipboardFormatType* native_format = nullptr;
        switch (format) {
        case blink::mojom::ClipboardFormat::kPlaintext:
            native_format = &ui::ClipboardFormatType::PlainTextType();
            break;
        case blink::mojom::ClipboardFormat::kHtml:
            native_format = &ui::ClipboardFormatType::HtmlType();
            break;
        case blink::mojom::ClipboardFormat::kSmartPaste:
            native_format = &ui::ClipboardFormatType::WebKitSmartPasteType();
            break;
        case blink::mojom::ClipboardFormat::kBookmark:
            native_format = &ui::ClipboardFormatType::UrlType();
            break;
        }
        std::move(callback).Run(native_format
            && ui::Clipboard::GetForCurrentThread()->IsFormatAvailable(*native_format, buffer, nullptr));
    }

    void ReadAvailableTypes(ui::ClipboardBuffer buffer,
        ReadAvailableTypesCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        std::vector<std::u16string> types;
        ui::Clipboard::GetForCurrentThread()->ReadAvailableTypes(buffer, nullptr, &types);
        std::move(callback).Run(types);
    }

    void ReadText(ui::ClipboardBuffer buffer, ReadTextCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        std::u16string text;
        ui::Clipboard::GetForCurrentThread()->ReadText(buffer, nullptr, &text);
        std::move(callback).Run(text);
    }

    void ReadHtml(ui::ClipboardBuffer buffer, ReadHtmlCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        std::u16string markup;
        std::string source_url;
        uint32_t fragment_start = 0;
        uint32_t fragment_end = 0;
        // ClipboardWin supplies UTF16 fragment offsets, not CF_HTML byte offsets.
        ui::Clipboard::GetForCurrentThread()->ReadHTML(buffer, nullptr, &markup,
            &source_url, &fragment_start, &fragment_end);
        std::move(callback).Run(markup, GURL(source_url), fragment_start, fragment_end);
    }

    void ReadSvg(ui::ClipboardBuffer buffer, ReadSvgCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        std::u16string markup;
        ui::Clipboard::GetForCurrentThread()->ReadSvg(buffer, nullptr, &markup);
        std::move(callback).Run(markup);
    }

    void ReadRtf(ui::ClipboardBuffer buffer, ReadRtfCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        std::string rtf;
        ui::Clipboard::GetForCurrentThread()->ReadRTF(buffer, nullptr, &rtf);
        std::move(callback).Run(rtf);
    }

    void ReadPng(ui::ClipboardBuffer buffer, ReadPngCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        ui::Clipboard::GetForCurrentThread()->ReadPng(buffer, nullptr,
            base::BindOnce(&NativeClipboardHost::OnPngRead,
                weak_factory_.GetWeakPtr(), std::move(callback)));
    }

    void ReadFiles(ui::ClipboardBuffer buffer, ReadFilesCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        std::vector<ui::FileInfo> filenames;
        ui::Clipboard::GetForCurrentThread()->ReadFilenames(buffer, nullptr, &filenames);
        auto result = blink::mojom::ClipboardFiles::New();
        // These are native path-backed files, not an invented isolated filesystem
        // registration. Keep the ordinary ClipboardFiles field on the wire.
        result->file_system_id = std::string();
        for (const auto& file : filenames) {
            base::File::Info information;
            if (!file.path.IsAbsolute() || !resources_
                || !resources_->GetFileInfo(file.path, &information)) {
                continue;
            }
            // This is discovery, not final authorization. The deferred File
            // backing must retain this document's grant and a checked read handle.
            result->files.push_back(blink::mojom::DataTransferFile::New(file.path,
                file.display_name.empty() ? file.path.BaseName() : file.display_name));
        }
        std::move(callback).Run(std::move(result));
    }

    void ReadDataTransferCustomData(ui::ClipboardBuffer buffer,
        const std::u16string& type, ReadDataTransferCustomDataCallback callback) override
    {
        if (!RequireBuffer(buffer))
            return;
        std::u16string data;
        ui::Clipboard::GetForCurrentThread()->ReadDataTransferCustomData(buffer, type, nullptr, &data);
        std::move(callback).Run(data);
    }

    void ReadAvailableCustomAndStandardFormats(
        ReadAvailableCustomAndStandardFormatsCallback callback) override
    {
        std::move(callback).Run(ui::Clipboard::GetForCurrentThread()->ReadAvailableStandardAndCustomFormatNames(
            ui::ClipboardBuffer::kCopyPaste, nullptr));
    }

    void ReadUnsanitizedCustomFormat(const std::u16string& format,
        ReadUnsanitizedCustomFormatCallback callback) override
    {
        if (!IsValidCustomFormat(format)) {
            Reject(kInvalidClipboardData, "Invalid custom clipboard format name");
            return;
        }
        auto* clipboard = ui::Clipboard::GetForCurrentThread();
        const auto formats = clipboard->ExtractCustomPlatformNames(ui::ClipboardBuffer::kCopyPaste, nullptr);
        // Reads carry the "web " prefix; the writer/map uses the bare MIME type.
        const auto found = formats.find(base::UTF16ToUTF8(format));
        if (found == formats.end() || found->second.empty()
            || found->second.size() >= ui::kMaxWebCustomFormatSize
            || found->second.find('\0') != std::string::npos) {
            std::move(callback).Run(mojo_base::BigBuffer());
            return;
        }
        std::string data;
        if (!clipboard->ReadDataWithMaxSize(
                ui::ClipboardFormatType::CustomPlatformType(found->second),
                nullptr, kMaxDataSize, &data)) {
            Reject(kInvalidClipboardData, "Custom clipboard payload exceeds size limit");
            return;
        }
        std::move(callback).Run(mojo_base::BigBuffer(
            base::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size())));
    }

    void WriteText(const std::u16string& text) override
    {
        if (!RequireStringSize(text.size()))
            return;
        Writer().WriteText(text);
    }

    void WriteHtml(const std::u16string& markup, const GURL& url) override
    {
        if (!RequireStringSize(markup.size()))
            return;
        Writer().WriteHTML(markup, url.spec());
    }

    void WriteSvg(const std::u16string& markup) override
    {
        if (!RequireStringSize(markup.size()))
            return;
        Writer().WriteSvg(markup);
    }

    void WriteSmartPasteMarker() override
    {
        Writer().WriteWebSmartPaste();
    }

    void WriteDataTransferCustomData(
        const base::flat_map<std::u16string, std::u16string>& data) override
    {
        size_t pickle_size = sizeof(base::Pickle::Header) + sizeof(uint32_t);
        for (const auto& entry : data) {
            if (!AddPickledStringSize(entry.first.size(), &pickle_size)
                || !AddPickledStringSize(entry.second.size(), &pickle_size)) {
                Reject(kInvalidClipboardData, "DataTransfer clipboard payload exceeds size limit");
                return;
            }
        }
        base::Pickle pickle;
        ui::WriteCustomDataToPickle(data, &pickle);
        Writer().WritePickledData(pickle, ui::ClipboardFormatType::DataTransferCustomType());
    }

    void WriteBookmark(const std::string& url, const std::u16string& title) override
    {
        if (url.size() >= kMaxDataSize) {
            Reject(kInvalidClipboardData, "Clipboard bookmark URL exceeds size limit");
            return;
        }
        if (!RequireStringSize(title.size()))
            return;
        Writer().WriteBookmark(title, url);
    }

    void WriteImage(const SkBitmap& image) override
    {
        if (image.drawsNothing())
            return;
        // BitmapN32 decoding already copies shared pixels into owned storage.
        // Do not let the writer's N32 CHECK become our input-validation policy.
        if (image.colorType() != kN32_SkColorType || !image.getPixels()
            || image.computeByteSize() >= kMaxDataSize) {
            Reject(kInvalidClipboardData, "Invalid clipboard bitmap");
            return;
        }
        Writer().WriteImage(image);
    }

    void WriteUnsanitizedCustomFormat(const std::u16string& format,
        mojo_base::BigBuffer data) override
    {
        if (!IsValidCustomFormat(format) || data.size() >= kMaxDataSize
            || data.storage_type() == mojo_base::BigBuffer::StorageType::kInvalidBuffer) {
            Reject(kInvalidClipboardData, "Invalid custom clipboard format or payload");
            return;
        }
        if (custom_formats_.contains(format))
            return;
        if (custom_formats_.size() >= static_cast<size_t>(ui::kMaxRegisteredClipboardFormats)) {
            Reject(kInvalidClipboardData, "Too many custom clipboard formats");
            return;
        }
        custom_formats_.insert(format);
        // Raw data is opaque. ScopedClipboardWriter/ClipboardWin copy it into
        // native clipboard storage at commit without interpreting shared bytes.
        Writer().WriteData(format, std::move(data));
    }

    void CommitWrite() override
    {
        // Destruction is the one atomic multi-format OS transaction. An empty
        // receiver has no transaction and must not clear another app's clipboard.
        writer_.reset();
        custom_formats_.clear();
    }

private:
    bool RequireBuffer(ui::ClipboardBuffer buffer)
    {
        if (ui::Clipboard::IsSupportedClipboardBuffer(buffer))
            return true;
        // There is no nullable sequence token and no operation status in this
        // schema. A transport error is honest for Windows' unsupported selection.
        Reject(kUnsupportedBuffer, "Clipboard buffer is unsupported on this platform");
        return false;
    }

    bool RequireStringSize(size_t length)
    {
        if (length < kMaxDataSize / sizeof(char16_t))
            return true;
        Reject(kInvalidClipboardData, "Clipboard string exceeds size limit");
        return false;
    }

    void Reject(uint32_t reason, const std::string& description)
    {
        // Removing the currently dispatching unique receiver destroys this
        // object. Keep the callback alive locally; callers return immediately.
        auto close_receiver = close_receiver_;
        AbortWrite();
        close_receiver.Run(reason, description);
    }

    ui::ScopedClipboardWriter& Writer()
    {
        if (!writer_)
            writer_ = std::make_unique<ui::ScopedClipboardWriter>(ui::ClipboardBuffer::kCopyPaste);
        return *writer_;
    }

    void AbortWrite()
    {
        if (writer_) {
            writer_->Reset();
            writer_.reset();
        }
        custom_formats_.clear();
    }

    void OnPngRead(ReadPngCallback callback, const std::vector<uint8_t>& png)
    {
        std::move(callback).Run(mojo_base::BigBuffer(base::span<const uint8_t>(png)));
    }

    const std::shared_ptr<LocalResources> resources_;
    const CloseReceiver close_receiver_;
    std::unique_ptr<ui::ScopedClipboardWriter> writer_;
    base::flat_set<std::u16string> custom_formats_;
    base::WeakPtrFactory<NativeClipboardHost> weak_factory_ { this };
};

class LocalBrowserInterfaceBroker final : public blink::mojom::BrowserInterfaceBroker {
public:
    explicit LocalBrowserInterfaceBroker(std::shared_ptr<LocalResources> resources)
        : resources_(std::move(resources))
    {
    }

    void GetInterface(mojo::GenericPendingReceiver receiver) override
    {
        auto clipboard = receiver.As<blink::mojom::ClipboardHost>();
        if (!clipboard.is_valid())
            return; // Unknown interfaces close as unavailable.
        clipboards_.Add(std::make_unique<NativeClipboardHost>(resources_,
                            base::BindRepeating(&LocalBrowserInterfaceBroker::CloseCurrentClipboard,
                                base::Unretained(this))),
            std::move(clipboard));
    }

private:
    void CloseCurrentClipboard(uint32_t reason, const std::string& description)
    {
        clipboards_.RemoveWithReason(clipboards_.current_receiver(), reason, description);
    }

    // The native thread's UniqueReceiverSet destroys this broker on pipe
    // disconnect or service shutdown. The clipboard set dies first, resetting
    // each receiver's pending writer before releasing the document grant.
    const std::shared_ptr<LocalResources> resources_;
    mojo::UniqueReceiverSet<blink::mojom::ClipboardHost> clipboards_;
};

// ClipboardWin notifies a process-global, sequence-bound ClipboardMonitor.
// Documents therefore share this native sequence, never a broker, capability
// group, or staged writer. No request depends on a renderer UI task.
class NativeClipboardThread final : public base::Thread {
public:
    NativeClipboardThread()
        : base::Thread("mini-ui native clipboard")
    {
        init_com_with_mta(false);
    }

    ~NativeClipboardThread() override
    {
        Stop();
    }

    void Bind(mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> receiver,
        std::shared_ptr<LocalResources> resources)
    {
        if (!brokers_)
            return;
        brokers_->Add(std::make_unique<LocalBrowserInterfaceBroker>(std::move(resources)),
            std::move(receiver));
    }

    void CloseDocumentsAndClipboard()
    {
        if (!brokers_)
            return;
        // Mojo objects and receiver-specific writers/grants must be destroyed
        // here, not on the UI thread that owns and joins the base::Thread.
        brokers_.reset();
        ui::Clipboard::OnPreShutdownForCurrentThread();
        ui::Clipboard::DestroyClipboardForCurrentThread();
    }

private:
    void Init() override
    {
        brokers_ = std::make_unique<mojo::UniqueReceiverSet<blink::mojom::BrowserInterfaceBroker>>();
    }

    void CleanUp() override
    {
        // Also handles a thread that started but never accepted a binding.
        // base::Thread releases its STA initializer only after CleanUp returns.
        CloseDocumentsAndClipboard();
    }

    std::unique_ptr<mojo::UniqueReceiverSet<blink::mojom::BrowserInterfaceBroker>> brokers_;
};

class NativeClipboardService final {
public:
    NativeClipboardService() = default;

    void Shutdown()
    {
        if (!thread_)
            return;
        // Stop posts its quit helper to this same FIFO task runner. The close
        // task runs first, while the native message pump and STA are still
        // alive; Stop joins only after native CleanUp and COM release complete.
        // No caller task pump, disconnect delivery, or separate event is needed.
        CHECK(runner_->PostTask(FROM_HERE,
            base::BindOnce(&NativeClipboardThread::CloseDocumentsAndClipboard,
                base::Unretained(thread_.get()))));
        thread_->Stop();
        runner_.reset();
        thread_.reset();
    }

    void Bind(mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> receiver,
        std::shared_ptr<LocalResources> resources)
    {
        if (!thread_) {
            auto thread = std::make_unique<NativeClipboardThread>();
            base::Thread::Options options;
            options.message_pump_type = base::MessagePumpType::UI;
            if (!thread->StartWithOptions(std::move(options)))
                return; // Startup failure closes the pending receiver normally.
            runner_ = thread->task_runner();
            thread_ = std::move(thread);
        }
        // Posting failure destroys the closure and closes its pending receiver.
        // Explicit Shutdown retains ownership through native close and Stop/join.
        runner_->PostTask(FROM_HERE,
            base::BindOnce(&NativeClipboardThread::Bind, base::Unretained(thread_.get()),
                std::move(receiver), std::move(resources)));
    }

private:
    std::unique_ptr<NativeClipboardThread> thread_;
    scoped_refptr<base::SingleThreadTaskRunner> runner_;
};

class NativeClipboardState final {
public:
    void Bind(mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> receiver,
        std::shared_ptr<LocalResources> resources)
    {
        DCHECK_CALLED_ON_VALID_SEQUENCE(owner_sequence_checker_);
        if (shutdown_)
            return;
        if (!service_)
            service_ = std::make_unique<NativeClipboardService>();
        service_->Bind(std::move(receiver), std::move(resources));
    }

    void Shutdown()
    {
        DCHECK_CALLED_ON_VALID_SEQUENCE(owner_sequence_checker_);
        if (shutdown_)
            return;
        shutdown_ = true;
        // Partial BlinkProcess initialization may never have reached Bind.
        // Do not construct a service, start a thread, or touch Mojo in that case.
        if (!service_)
            return;
        service_->Shutdown();
        service_.reset();
    }

private:
    std::unique_ptr<NativeClipboardService> service_;
    bool shutdown_ = false;
    SEQUENCE_CHECKER(owner_sequence_checker_);
};

NativeClipboardState& ClipboardState()
{
    // Only the empty native lifecycle state persists through process exit.
    // The explicit call, before Mojo teardown, resets its optional service and
    // releases every native thread/runner/group/grant. No static destructor race.
    static base::NoDestructor<NativeClipboardState> state;
    return *state;
}

} // namespace

void BindLocalBrowserInterfaceBroker(
    mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> receiver,
    std::shared_ptr<LocalResources> resources)
{
    if (!receiver.is_valid())
        return;
    ClipboardState().Bind(std::move(receiver), std::move(resources));
}

void ShutdownNativeClipboard()
{
    ClipboardState().Shutdown();
}

} // namespace mini_ui
