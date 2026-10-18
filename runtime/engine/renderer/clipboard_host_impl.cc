#include "runtime/engine/renderer/clipboard_host_impl.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/unguessable_token.h"
#include "platform/clipboard.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/renderer/platform/image-encoders/image_encoder.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"

bool blink::mojom::blink::ClipboardHost::GetSequenceNumber(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::blink::ClipboardSequenceNumberToken* out_result) {
  return false;
}

bool blink::mojom::blink::ClipboardHost::IsFormatAvailable(
    ::blink::mojom::blink::ClipboardFormat format,
    ::blink::mojom::blink::ClipboardBuffer buffer,
    bool* out_result) {
  return false;
}

bool blink::mojom::blink::ClipboardHost::ReadAvailableTypes(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    WTF::Vector<::WTF::String>* out_types) {
  return false;
}

bool blink::mojom::blink::ClipboardHost::ReadText(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::WTF::String* out_result) {
  return false;
}

bool blink::mojom::blink::ClipboardHost::ReadHtml(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::WTF::String* out_markup,
    ::blink::KURL* out_url,
    uint32_t* out_fragment_start,
    uint32_t* out_fragment_end) {
  return false;
}

bool blink::mojom::blink::ClipboardHost::ReadRtf(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::WTF::String* out_result) {
  return false;
}

bool blink::mojom::blink::ClipboardHost::ReadPng(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::mojo_base::BigBuffer* out_png) {
  return false;
}

bool blink::mojom::blink::ClipboardHost::ReadFiles(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::blink::mojom::blink::ClipboardFilesPtr* out_result) {
  return false;
}

bool blink::mojom::blink::ClipboardHost::ReadAvailableCustomAndStandardFormats(
    WTF::Vector<::WTF::String>* out_format_types) {
  return false;
}

namespace {

bool IsStandardBuffer(blink::mojom::blink::ClipboardBuffer buffer) {
  return buffer == blink::mojom::blink::ClipboardBuffer::kStandard;
}

std::u16string ToUTF16(const WTF::String& value) {
  if (value.empty())
    return {};
  if (!value.Is8Bit()) {
    return std::u16string(
        reinterpret_cast<const char16_t*>(value.Characters16()),
        value.length());
  }
  std::u16string result(value.length(), u'\0');
  for (size_t i = 0; i < value.length(); ++i)
    result[i] = value.Characters8()[i];
  return result;
}

WTF::String FromUTF16(const std::u16string& value) {
  return WTF::String(base::span<const UChar>(
      reinterpret_cast<const UChar*>(value.data()), value.size()));
}

bool IsRendererImageSizeValid(const SkBitmap& image) {
  return !image.drawsNothing()
      && mini_electron::ClipboardImageDimensionsAreValid(
          image.width(), image.height());
}

}  // namespace

namespace content {

bool ClipboardHostImpl::GetSequenceNumber(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::blink::ClipboardSequenceNumberToken* out_result) {
  if (!IsStandardBuffer(buffer))
    return false;
  mini_electron::ClipboardSequence sequence =
      mini_electron::GetClipboardSequence();
  std::optional<base::UnguessableToken> token =
      base::UnguessableToken::Deserialize(sequence.high, sequence.low);
  if (!token)
    return false;
  *out_result = ::blink::ClipboardSequenceNumberToken(*token);
  return true;
}

void ClipboardHostImpl::GetSequenceNumber(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    GetSequenceNumberCallback callback) {
  ::blink::ClipboardSequenceNumberToken result;
  GetSequenceNumber(buffer, &result);
  std::move(callback).Run(result);
}

bool ClipboardHostImpl::IsFormatAvailable(
    ::blink::mojom::blink::ClipboardFormat format,
    ::blink::mojom::blink::ClipboardBuffer buffer,
    bool* out_result) {
  *out_result = false;
  if (!IsStandardBuffer(buffer))
    return false;
  switch (format) {
    case blink::mojom::blink::ClipboardFormat::kPlaintext:
      *out_result = mini_electron::ClipboardHasText();
      return true;
    case blink::mojom::blink::ClipboardFormat::kHtml:
      *out_result = mini_electron::ClipboardHasHtml();
      return true;
    case blink::mojom::blink::ClipboardFormat::kSmartPaste:
    case blink::mojom::blink::ClipboardFormat::kBookmark:
      return true;
  }
  return true;
}

void ClipboardHostImpl::IsFormatAvailable(
    ::blink::mojom::blink::ClipboardFormat format,
    ::blink::mojom::blink::ClipboardBuffer buffer,
    IsFormatAvailableCallback callback) {
  bool available = false;
  IsFormatAvailable(format, buffer, &available);
  std::move(callback).Run(available);
}

bool ClipboardHostImpl::ReadAvailableTypes(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    WTF::Vector<::WTF::String>* types) {
  types->clear();
  if (!IsStandardBuffer(buffer))
    return false;
  const auto formats = mini_electron::AvailableClipboardFormats();
  for (const auto& format : formats) {
    if (format == u"text/plain" || format == u"text/html"
        || format == u"image/png") {
      types->push_back(FromUTF16(format));
    }
  }
  return true;
}

void ClipboardHostImpl::ReadAvailableTypes(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ReadAvailableTypesCallback callback) {
  WTF::Vector<::WTF::String> types;
  ReadAvailableTypes(buffer, &types);
  std::move(callback).Run(types);
}

bool ClipboardHostImpl::ReadText(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::WTF::String* text) {
  if (!IsStandardBuffer(buffer))
    return false;
  *text = FromUTF16(mini_electron::ReadClipboardText());
  return true;
}

void ClipboardHostImpl::ReadText(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ReadTextCallback callback) {
  WTF::String text;
  ReadText(buffer, &text);
  std::move(callback).Run(text);
}

bool ClipboardHostImpl::ReadHtml(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::WTF::String* out_markup,
    ::blink::KURL* out_url,
    uint32_t* out_fragment_start,
    uint32_t* out_fragment_end) {
  if (!IsStandardBuffer(buffer))
    return false;
  mini_electron::ClipboardHtml html = mini_electron::ReadClipboardHtml();
  if (html.fragment_start > html.fragment_end
      || html.fragment_end > html.markup.size()) {
    return false;
  }
  *out_markup = FromUTF16(html.markup);
  *out_url = html.source_url.empty()
      ? blink::KURL()
      : blink::KURL(WTF::String::FromUTF8(html.source_url));
  *out_fragment_start = html.fragment_start;
  *out_fragment_end = html.fragment_end;
  return true;
}

void ClipboardHostImpl::ReadHtml(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ReadHtmlCallback callback) {
  WTF::String markup;
  blink::KURL url;
  uint32_t fragment_start = 0;
  uint32_t fragment_end = 0;
  ReadHtml(buffer, &markup, &url, &fragment_start, &fragment_end);
  std::move(callback).Run(markup, url, fragment_start, fragment_end);
}

void ClipboardHostImpl::ReadSvg(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ReadSvgCallback callback) {
  std::move(callback).Run(::WTF::String());
}

bool ClipboardHostImpl::ReadRtf(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::WTF::String* out_result) {
  if (!IsStandardBuffer(buffer))
    return false;
  *out_result = WTF::String::FromUTF8(mini_electron::ReadClipboardRtf());
  return true;
}

void ClipboardHostImpl::ReadRtf(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ReadRtfCallback callback) {
  WTF::String rtf;
  ReadRtf(buffer, &rtf);
  std::move(callback).Run(rtf);
}

bool ClipboardHostImpl::ReadPng(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::mojo_base::BigBuffer* out_png) {
  if (!IsStandardBuffer(buffer)) {
    *out_png = ::mojo_base::BigBuffer();
    return false;
  }
  std::vector<uint8_t> png = mini_electron::ReadClipboardPng();
  *out_png = ::mojo_base::BigBuffer(base::span<const uint8_t>(png));
  return !png.empty();
}

void ClipboardHostImpl::ReadPng(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ReadPngCallback callback) {
  ::mojo_base::BigBuffer png;
  ReadPng(buffer, &png);
  std::move(callback).Run(std::move(png));
}

bool ClipboardHostImpl::ReadFiles(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ::blink::mojom::blink::ClipboardFilesPtr* out_result) {
  return false;
}

void ClipboardHostImpl::ReadFiles(
    ::blink::mojom::blink::ClipboardBuffer buffer,
    ReadFilesCallback callback) {
  std::move(callback).Run(::blink::mojom::blink::ClipboardFiles::New());
}

bool ClipboardHostImpl::ReadAvailableCustomAndStandardFormats(
    WTF::Vector<::WTF::String>* out_format_types) {
  return ReadAvailableTypes(
      blink::mojom::blink::ClipboardBuffer::kStandard, out_format_types);
}

void ClipboardHostImpl::ReadAvailableCustomAndStandardFormats(
    ReadAvailableCustomAndStandardFormatsCallback callback) {
  WTF::Vector<::WTF::String> formats;
  ReadAvailableCustomAndStandardFormats(&formats);
  std::move(callback).Run(formats);
}

void ClipboardHostImpl::ReadUnsanitizedCustomFormat(
    const ::WTF::String& format,
    ReadUnsanitizedCustomFormatCallback callback) {
  std::move(callback).Run(::mojo_base::BigBuffer());
}

bool ClipboardHostImpl::ReadDataTransferCustomData(
    blink::mojom::blink::ClipboardBuffer buffer,
    const ::WTF::String& type,
    ::WTF::String* out_result) {
  *out_result = WTF::String();
  return IsStandardBuffer(buffer);
}

void ClipboardHostImpl::ReadDataTransferCustomData(
    blink::mojom::blink::ClipboardBuffer buffer,
    const ::WTF::String& type,
    ReadDataTransferCustomDataCallback callback) {
  std::move(callback).Run(WTF::String());
}

void ClipboardHostImpl::WriteDataTransferCustomData(
    const WTF::HashMap<::WTF::String, ::WTF::String>& data) {
}

void ClipboardHostImpl::WriteText(const ::WTF::String& text) {
  m_pendingText = ToUTF16(text);
}

void ClipboardHostImpl::WriteHtml(
    const ::WTF::String& markup,
    const ::blink::KURL& url) {
  m_pendingHtml = ToUTF16(markup);
  m_pendingHtmlSourceUrl = url.IsEmpty()
      ? std::string()
      : url.GetString().Utf8();
}

void ClipboardHostImpl::WriteSvg(const ::WTF::String& markup) {
}

void ClipboardHostImpl::WriteSmartPasteMarker() {
}

void ClipboardHostImpl::WriteBookmark(
    const WTF::String& url,
    const ::WTF::String& title) {
}

void ClipboardHostImpl::WriteImage(const ::SkBitmap& image) {
  m_pendingPng.reset();
  if (!IsRendererImageSizeValid(image))
    return;
  WTF::Vector<unsigned char> encoded;
  SkPngEncoder::Options options;
  if (!blink::ImageEncoder::Encode(&encoded, image.pixmap(), options)
      || encoded.empty()
      || encoded.size() > mini_electron::kMaximumClipboardImageBytes) {
    return;
  }
  m_pendingPng.emplace(encoded.begin(), encoded.end());
}

void ClipboardHostImpl::WriteUnsanitizedCustomFormat(
    const ::WTF::String& format,
    ::mojo_base::BigBuffer data) {
}

void ClipboardHostImpl::CommitWrite() {
  if (!m_pendingText && !m_pendingHtml && !m_pendingPng)
    return;

  mini_electron::ClipboardContent content;
  content.text = m_pendingText ? &*m_pendingText : nullptr;
  content.html = m_pendingHtml ? &*m_pendingHtml : nullptr;
  content.html_source_url = m_pendingHtmlSourceUrl
      ? &*m_pendingHtmlSourceUrl : nullptr;
  content.png = m_pendingPng ? &*m_pendingPng : nullptr;
  mini_electron::WriteClipboard(content);

  // A failed broker transaction must not be replayed into a later copy.
  m_pendingText.reset();
  m_pendingHtml.reset();
  m_pendingHtmlSourceUrl.reset();
  m_pendingPng.reset();
}

}  // namespace content
