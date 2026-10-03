// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/common/renderer_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#if !BUILDFLAG(IS_WIN)
#include <unistd.h>
#endif

namespace atom::renderer_protocol {
namespace {

enum class ValueTag : uint8_t {
    kNull = 0,
    kFalse = 1,
    kTrue = 2,
    kInt = 3,
    kDouble = 4,
    kString = 5,
    kBinary = 6,
    kDict = 7,
    kList = 8,
};
struct WireHeader {
    uint32_t magic = kWireMagic;
    uint16_t version = kWireVersion;
    uint16_t kind = 0;
    uint32_t payload_size = 0;
    uint64_t request_id = 0;
    uint64_t auth_high = 0;
    uint64_t auth_low = 0;
};

// Keep the version-1 wire size and offsets stable without exposing native
// structure padding. Bytes 12..15 are an explicitly zeroed reserved field.
constexpr size_t kWireHeaderSize = 40;

template <typename T>
void StoreHeaderField(std::array<uint8_t, kWireHeaderSize>* bytes,
    size_t offset, const T& value)
{
    std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

template <typename T>
void LoadHeaderField(const std::array<uint8_t, kWireHeaderSize>& bytes,
    size_t offset, T* value)
{
    std::memcpy(value, bytes.data() + offset, sizeof(*value));
}

std::array<uint8_t, kWireHeaderSize> SerializeHeader(const WireHeader& header)
{
    std::array<uint8_t, kWireHeaderSize> bytes {};
    StoreHeaderField(&bytes, 0, header.magic);
    StoreHeaderField(&bytes, 4, header.version);
    StoreHeaderField(&bytes, 6, header.kind);
    StoreHeaderField(&bytes, 8, header.payload_size);
    StoreHeaderField(&bytes, 16, header.request_id);
    StoreHeaderField(&bytes, 24, header.auth_high);
    StoreHeaderField(&bytes, 32, header.auth_low);
    return bytes;
}

WireHeader DeserializeHeader(
    const std::array<uint8_t, kWireHeaderSize>& bytes)
{
    WireHeader header;
    LoadHeaderField(bytes, 0, &header.magic);
    LoadHeaderField(bytes, 4, &header.version);
    LoadHeaderField(bytes, 6, &header.kind);
    LoadHeaderField(bytes, 8, &header.payload_size);
    LoadHeaderField(bytes, 16, &header.request_id);
    LoadHeaderField(bytes, 24, &header.auth_high);
    LoadHeaderField(bytes, 32, &header.auth_low);
    return header;
}


bool ReadExact(PipeHandle pipe, void* bytes, size_t size)
{
    auto* cursor = static_cast<uint8_t*>(bytes);
    while (size) {
#if BUILDFLAG(IS_WIN)
        DWORD read = 0;
        if (!::ReadFile(pipe, cursor, static_cast<DWORD>(std::min<size_t>(size, MAXDWORD)), &read, nullptr) || !read)
            return false;
#else
        ssize_t read = ::read(pipe, cursor, size);
        if (read < 0 && errno == EINTR)
            continue;
        if (read <= 0)
            return false;
#endif
        cursor += read;
        size -= read;
    }
    return true;
}

bool WriteExact(PipeHandle pipe, const void* bytes, size_t size)
{
    const auto* cursor = static_cast<const uint8_t*>(bytes);
    while (size) {
#if BUILDFLAG(IS_WIN)
        DWORD written = 0;
        if (!::WriteFile(pipe, cursor, static_cast<DWORD>(std::min<size_t>(size, MAXDWORD)), &written, nullptr) || !written)
            return false;
#else
        ssize_t written = ::write(pipe, cursor, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
#endif
        cursor += written;
        size -= written;
    }
    return true;
}

template <typename T>
bool AppendScalar(std::vector<uint8_t>* output, const T& value)
{
    if (output->size() > kMaxMessageBytes - sizeof(T))
        return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    output->insert(output->end(), bytes, bytes + sizeof(T));
    return true;
}

bool AppendBytes(std::vector<uint8_t>* output, const void* bytes, size_t size)
{
    if (size > kMaxMessageBytes || output->size() > kMaxMessageBytes - size)
        return false;
    const auto* first = static_cast<const uint8_t*>(bytes);
    output->insert(output->end(), first, first + size);
    return true;
}

bool EncodeValue(const base::Value& value, size_t depth, std::vector<uint8_t>* output);

bool EncodeString(std::string_view value, std::vector<uint8_t>* output)
{
    if (value.size() > std::numeric_limits<uint32_t>::max())
        return false;
    uint32_t size = static_cast<uint32_t>(value.size());
    return AppendScalar(output, size) && AppendBytes(output, value.data(), value.size());
}

bool EncodeValue(const base::Value& value, size_t depth, std::vector<uint8_t>* output)
{
    if (depth > kMaxValueDepth)
        return false;
    ValueTag tag;
    switch (value.type()) {
    case base::Value::Type::NONE:
        tag = ValueTag::kNull;
        return AppendScalar(output, tag);
    case base::Value::Type::BOOLEAN:
        tag = value.GetBool() ? ValueTag::kTrue : ValueTag::kFalse;
        return AppendScalar(output, tag);
    case base::Value::Type::INTEGER: {
        tag = ValueTag::kInt;
        int32_t number = value.GetInt();
        return AppendScalar(output, tag) && AppendScalar(output, number);
    }
    case base::Value::Type::DOUBLE: {
        tag = ValueTag::kDouble;
        double number = value.GetDouble();
        return AppendScalar(output, tag) && AppendScalar(output, number);
    }
    case base::Value::Type::STRING:
        tag = ValueTag::kString;
        return AppendScalar(output, tag) && EncodeString(value.GetString(), output);
    case base::Value::Type::BINARY: {
        tag = ValueTag::kBinary;
        const auto& blob = value.GetBlob();
        if (blob.size() > std::numeric_limits<uint32_t>::max())
            return false;
        uint32_t size = static_cast<uint32_t>(blob.size());
        return AppendScalar(output, tag) && AppendScalar(output, size)
            && AppendBytes(output, blob.data(), blob.size());
    }
    case base::Value::Type::DICT: {
        tag = ValueTag::kDict;
        const auto& dict = value.GetDict();
        if (dict.size() > kMaxContainerEntries)
            return false;
        uint32_t size = static_cast<uint32_t>(dict.size());
        if (!AppendScalar(output, tag) || !AppendScalar(output, size))
            return false;
        for (const auto& [key, child] : dict) {
            if (!EncodeString(key, output) || !EncodeValue(child, depth + 1, output))
                return false;
        }
        return true;
    }
    case base::Value::Type::LIST: {
        tag = ValueTag::kList;
        const auto& list = value.GetList();
        if (list.size() > kMaxContainerEntries)
            return false;
        uint32_t size = static_cast<uint32_t>(list.size());
        if (!AppendScalar(output, tag) || !AppendScalar(output, size))
            return false;
        for (const auto& child : list) {
            if (!EncodeValue(child, depth + 1, output))
                return false;
        }
        return true;
    }
    }
    return false;
}

class Decoder {
public:
    Decoder(const uint8_t* data, size_t size)
        : cursor_(data)
        , end_(data + size)
    {
    }

    bool Decode(size_t depth, base::Value* output)
    {
        ValueTag tag;
        if (depth > kMaxValueDepth || !Take(&tag))
            return false;
        switch (tag) {
        case ValueTag::kNull:
            *output = base::Value();
            return true;
        case ValueTag::kFalse:
            *output = base::Value(false);
            return true;
        case ValueTag::kTrue:
            *output = base::Value(true);
            return true;
        case ValueTag::kInt: {
            int32_t number;
            if (!Take(&number))
                return false;
            *output = base::Value(number);
            return true;
        }
        case ValueTag::kDouble: {
            double number;
            if (!Take(&number))
                return false;
            *output = base::Value(number);
            return true;
        }
        case ValueTag::kString: {
            std::string string;
            if (!TakeString(&string))
                return false;
            *output = base::Value(std::move(string));
            return true;
        }
        case ValueTag::kBinary: {
            uint32_t size;
            if (!Take(&size) || Remaining() < size)
                return false;
            base::Value::BlobStorage blob(cursor_, cursor_ + size);
            cursor_ += size;
            *output = base::Value(std::move(blob));
            return true;
        }
        case ValueTag::kDict: {
            uint32_t size;
            if (!Take(&size) || size > kMaxContainerEntries)
                return false;
            base::Value::Dict dict;
            for (uint32_t i = 0; i < size; ++i) {
                std::string key;
                base::Value child;
                if (!TakeString(&key) || !Decode(depth + 1, &child) || dict.Find(key))
                    return false;
                dict.Set(key, std::move(child));
            }
            *output = base::Value(std::move(dict));
            return true;
        }
        case ValueTag::kList: {
            uint32_t size;
            if (!Take(&size) || size > kMaxContainerEntries)
                return false;
            base::Value::List list;
            list.reserve(size);
            for (uint32_t i = 0; i < size; ++i) {
                base::Value child;
                if (!Decode(depth + 1, &child))
                    return false;
                list.Append(std::move(child));
            }
            *output = base::Value(std::move(list));
            return true;
        }
        }
        return false;
    }

    bool AtEnd() const { return cursor_ == end_; }

private:
    template <typename T>
    bool Take(T* output)
    {
        if (Remaining() < sizeof(T))
            return false;
        std::memcpy(output, cursor_, sizeof(T));
        cursor_ += sizeof(T);
        return true;
    }

    bool TakeString(std::string* output)
    {
        uint32_t size;
        if (!Take(&size) || Remaining() < size)
            return false;
        output->assign(reinterpret_cast<const char*>(cursor_), size);
        cursor_ += size;
        return true;
    }

    size_t Remaining() const { return static_cast<size_t>(end_ - cursor_); }

    const uint8_t* cursor_;
    const uint8_t* end_;
};

} // namespace

bool ReadMessage(PipeHandle pipe, uint64_t auth_high, uint64_t auth_low,
    WireMessage* message, std::string* error)
{
    std::array<uint8_t, kWireHeaderSize> header_bytes;
    if (!ReadExact(pipe, header_bytes.data(), header_bytes.size())) {
        *error = "renderer IPC closed";
        return false;
    }
    WireHeader header = DeserializeHeader(header_bytes);
    if (header.magic != kWireMagic || header.version != kWireVersion
        || header.auth_high != auth_high || header.auth_low != auth_low) {
        *error = "renderer IPC authentication failed";
        return false;
    }
    if (header.payload_size > kMaxMessageBytes) {
        *error = "renderer IPC message exceeds limit";
        return false;
    }
    if (header.kind < static_cast<uint16_t>(MessageKind::kHello)
        || header.kind > static_cast<uint16_t>(MessageKind::kShutdown)) {
        *error = "renderer IPC message kind is invalid";
        return false;
    }
    std::vector<uint8_t> bytes(header.payload_size);
    if (!bytes.empty() && !ReadExact(pipe, bytes.data(), bytes.size())) {
        *error = "renderer IPC payload was truncated";
        return false;
    }
    base::Value value;
    Decoder decoder(bytes.data(), bytes.size());
    if (!decoder.Decode(0, &value) || !decoder.AtEnd() || !value.is_dict()) {
        *error = "renderer IPC payload is malformed";
        return false;
    }
    message->kind = static_cast<MessageKind>(header.kind);
    message->request_id = header.request_id;
    message->payload = std::move(value.GetDict());
    return true;
}

bool WriteMessage(PipeHandle pipe, uint64_t auth_high, uint64_t auth_low,
    const WireMessage& message, std::string* error)
{
    std::vector<uint8_t> bytes;
    if (!EncodeValue(base::Value(message.payload.Clone()), 0, &bytes)) {
        *error = "renderer IPC payload cannot be encoded";
        return false;
    }
    WireHeader header;
    header.kind = static_cast<uint16_t>(message.kind);
    header.payload_size = static_cast<uint32_t>(bytes.size());
    header.request_id = message.request_id;
    header.auth_high = auth_high;
    header.auth_low = auth_low;
    const std::array<uint8_t, kWireHeaderSize> header_bytes
        = SerializeHeader(header);
    if (!WriteExact(pipe, header_bytes.data(), header_bytes.size())
        || (!bytes.empty() && !WriteExact(pipe, bytes.data(), bytes.size()))) {
        *error = "renderer IPC write failed";
        return false;
    }
    return true;
}

void ClosePipe(PipeHandle pipe)
{
#if BUILDFLAG(IS_WIN)
    if (pipe != kInvalidPipe)
        ::CloseHandle(pipe);
#else
    if (pipe != kInvalidPipe)
        ::close(pipe);
#endif
}

bool IsBrowserToRendererMethod(const std::string& method)
{
    static constexpr std::string_view kMethods[] = {
        "navigate", "stop", "reload", "goBack", "goForward", "goToOffset",
        "goToIndex", "setViewport", "setFocus", "input", "ipc-message",
        "executeJavaScript", "insertCSS", "removeInsertedCSS", "capturePage",
        "setUserAgent", "setZoomLevel", "setVisualZoomLevelLimits",
        "setLayoutZoomLevelLimits", "setIgnoreMenuShortcuts", "invalidate",
        "download", "getState", "window-open-response", "guest.attach",
        "guest.detach", "guest.bounds", "guest.focus", "cdp.attach",
        "cdp.detach", "cdp.dispatch", "cdp.inspectElement",
        "network.websocket-event"
    };
    for (std::string_view allowed : kMethods) {
        if (method == allowed)
            return true;
    }
    return false;
}

bool IsRendererBrokerKind(const std::string& kind)
{
    return kind == "protocol" || kind == "storage" || kind == "file"
        || kind == "network";
}

} // namespace atom::renderer_protocol
