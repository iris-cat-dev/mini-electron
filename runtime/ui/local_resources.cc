// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/local_resources.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define MINI_UI_HAS_OPENAT2 1
#endif
#endif
#endif

namespace mini_ui {
namespace {

namespace fs = std::filesystem;

void ClearError(Error* error)
{
    if (error)
        *error = {};
}

bool Fail(Error* error, ErrorCode code, std::string message)
{
    if (error) {
        error->code = code;
        error->message = std::move(message);
    }
    return false;
}

bool IsAsciiAlpha(char c)
{
    const unsigned char value = static_cast<unsigned char>(c);
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

char AsciiLower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c + ('a' - 'A'));
    return c;
}

bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (AsciiLower(left[i]) != AsciiLower(right[i]))
            return false;
    }
    return true;
}

bool HasValidUtf8(std::string_view value)
{
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80)
            continue;

        unsigned length = 0;
        std::uint32_t code_point = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 1;
            code_point = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 2;
            code_point = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 3;
            code_point = first & 0x07;
        } else {
            return false;
        }
        if (i + length > value.size())
            return false;
        for (unsigned n = 0; n < length; ++n) {
            const unsigned char continuation = static_cast<unsigned char>(value[i++]);
            if ((continuation & 0xc0) != 0x80)
                return false;
            code_point = (code_point << 6) | (continuation & 0x3f);
        }
        if ((length == 2 && code_point < 0x800)
            || (length == 3 && code_point < 0x10000)
            || code_point > 0x10ffff
            || (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool PercentDecode(std::string_view input, std::string* output, Error* error)
{
    output->clear();
    output->reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        unsigned char value = static_cast<unsigned char>(input[i]);
        if (input[i] == '%') {
            if (i + 2 >= input.size())
                return Fail(error, ErrorCode::resource_denied, "Malformed percent escape in local resource URL");
            const int high = HexValue(input[i + 1]);
            const int low = HexValue(input[i + 2]);
            if (high < 0 || low < 0)
                return Fail(error, ErrorCode::resource_denied, "Malformed percent escape in local resource URL");
            value = static_cast<unsigned char>((high << 4) | low);
            i += 2;
        }
        if (value == 0 || value == '\\' || value < 0x20 || value == 0x7f)
            return Fail(error, ErrorCode::resource_denied, "Unsafe character in local resource URL");
        output->push_back(static_cast<char>(value));
    }
    if (!HasValidUtf8(*output))
        return Fail(error, ErrorCode::resource_denied, "Local resource URL is not valid UTF-8");
    return true;
}

std::string_view PathPart(std::string_view url)
{
    const std::size_t query = url.find('?');
    const std::size_t fragment = url.find('#');
    const std::size_t end = std::min(query == std::string_view::npos ? url.size() : query,
        fragment == std::string_view::npos ? url.size() : fragment);
    return url.substr(0, end);
}

std::string_view SchemeOf(std::string_view url)
{
    if (url.empty() || !IsAsciiAlpha(url.front()))
        return {};
    std::size_t i = 1;
    while (i < url.size()) {
        const char c = url[i];
        if (c == ':')
            return url.substr(0, i);
        if (!(IsAsciiAlpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
            return {};
        ++i;
    }
    return {};
}

bool IsWindowsDeviceName(std::string_view segment)
{
#if defined(_WIN32)
    std::size_t end = segment.find('.');
    if (end == std::string_view::npos)
        end = segment.size();
    const std::string_view stem = segment.substr(0, end);
    if (EqualsIgnoringAsciiCase(stem, "con") || EqualsIgnoringAsciiCase(stem, "prn")
        || EqualsIgnoringAsciiCase(stem, "aux") || EqualsIgnoringAsciiCase(stem, "nul")
        || EqualsIgnoringAsciiCase(stem, "conin$") || EqualsIgnoringAsciiCase(stem, "conout$")) {
        return true;
    }
    if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') {
        const std::string_view prefix = stem.substr(0, 3);
        return EqualsIgnoringAsciiCase(prefix, "com") || EqualsIgnoringAsciiCase(prefix, "lpt");
    }
#else
    (void)segment;
#endif
    return false;
}

bool ValidateSegment(std::string_view segment, Error* error)
{
    if (segment.empty() || segment == "." || segment == "..")
        return Fail(error, ErrorCode::resource_denied, "Unsafe local resource path component");
    if (segment.find(':') != std::string_view::npos)
        return Fail(error, ErrorCode::resource_denied, "Device or alternate-stream path is not allowed");
#if defined(_WIN32)
    if (segment.back() == '.' || segment.back() == ' ' || IsWindowsDeviceName(segment))
        return Fail(error, ErrorCode::resource_denied, "Windows device path is not allowed");
#endif
    return true;
}

bool AppendPath(std::string_view decoded, std::vector<std::string>* segments, Error* error)
{
    std::size_t begin = 0;
    while (begin <= decoded.size()) {
        const std::size_t slash = decoded.find('/', begin);
        const std::size_t end = slash == std::string_view::npos ? decoded.size() : slash;
        const std::string_view segment = decoded.substr(begin, end - begin);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (segments->empty())
                    return Fail(error, ErrorCode::resource_denied, "Local resource path escapes its configured root");
                segments->pop_back();
            } else {
                if (!ValidateSegment(segment, error))
                    return false;
                segments->emplace_back(segment);
            }
        }
        if (slash == std::string_view::npos)
            break;
        begin = slash + 1;
    }
    return true;
}

#if defined(_WIN32)
bool EqualPathComponent(const fs::path& left, const fs::path& right)
{
    const std::wstring& a = left.native();
    const std::wstring& b = right.native();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
               static_cast<int>(b.size()), TRUE)
        == CSTR_EQUAL;
}
#else
bool EqualPathComponent(const fs::path& left, const fs::path& right)
{
    return left == right;
}
#endif

std::string PathComponentUtf8(const fs::path& path)
{
#if defined(__cpp_lib_char8_t)
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
    return path.u8string();
#endif
}

bool AbsolutePathToSegments(const fs::path& root, const fs::path& candidate_path,
    std::vector<std::string>* segments, Error* error, bool exact_root = false)
{
    const fs::path candidate = candidate_path.lexically_normal();
    const fs::path normalized_root = root.lexically_normal();
    if (!candidate.is_absolute())
        return Fail(error, ErrorCode::resource_denied, "A file URL must contain an absolute local path");

    auto root_it = normalized_root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        const bool equal = candidate_it != candidate.end()
            && (exact_root ? *root_it == *candidate_it
                           : EqualPathComponent(*root_it, *candidate_it));
        if (!equal)
            return Fail(error, ErrorCode::resource_denied, "Local resource path is outside its configured root");
    }

    if (!segments)
        return true;
    segments->clear();
    for (; candidate_it != candidate.end(); ++candidate_it) {
        if (candidate_it->empty())
            continue; // A native directory path may end in a separator.
        const std::string segment = PathComponentUtf8(*candidate_it);
        if (!HasValidUtf8(segment) || !ValidateSegment(segment, error))
            return false;
        segments->push_back(segment);
    }
    return true;
}

struct ParsedUrlPath {
    enum class Kind { relative, root_relative, file } kind = Kind::relative;
    std::string decoded;
    bool trailing_slash = false;
    bool empty_path = false;
};

bool ParseUrlPath(std::string_view url, ParsedUrlPath* parsed, Error* error)
{
    *parsed = {};
    const std::string_view path_part = PathPart(url);
    if (path_part.find('\\') != std::string_view::npos)
        return Fail(error, ErrorCode::resource_denied, "Backslash paths are not allowed in local resource URLs");

    const std::string_view scheme = SchemeOf(path_part);
    if (!scheme.empty()) {
        if (!EqualsIgnoringAsciiCase(scheme, "file"))
            return Fail(error, ErrorCode::resource_denied, "Network and non-file URL schemes are disabled");

        std::string_view remainder = path_part.substr(scheme.size() + 1);
        if (remainder.rfind("//", 0) == 0) {
            remainder.remove_prefix(2);
            const std::size_t slash = remainder.find('/');
            const std::string_view authority = remainder.substr(0, slash);
            if (!authority.empty() && !EqualsIgnoringAsciiCase(authority, "localhost"))
                return Fail(error, ErrorCode::resource_denied, "UNC and remote file URLs are disabled");
            if (slash == std::string_view::npos)
                return Fail(error, ErrorCode::resource_denied, "File URL does not contain a local absolute path");
            remainder.remove_prefix(slash);
        }
        if (remainder.empty() || remainder.front() != '/' || remainder.rfind("//", 0) == 0)
            return Fail(error, ErrorCode::resource_denied, "UNC, device, and relative file URLs are disabled");
        if (!PercentDecode(remainder, &parsed->decoded, error))
            return false;
        parsed->kind = ParsedUrlPath::Kind::file;
        parsed->trailing_slash = parsed->decoded.back() == '/';
        parsed->empty_path = false;
        return true;
    }

    if (path_part.rfind("//", 0) == 0)
        return Fail(error, ErrorCode::resource_denied, "Network-relative and UNC URLs are disabled");
    if (!PercentDecode(path_part, &parsed->decoded, error))
        return false;
    parsed->empty_path = parsed->decoded.empty();
    parsed->trailing_slash = !parsed->decoded.empty() && parsed->decoded.back() == '/';
    if (!parsed->decoded.empty() && parsed->decoded.front() == '/') {
        parsed->kind = ParsedUrlPath::Kind::root_relative;
        parsed->decoded.erase(parsed->decoded.begin());
    }
    return true;
}

bool FileUrlToPath(std::string_view decoded, fs::path* path, Error* error)
{
    try {
#if defined(_WIN32)
        if (decoded.size() < 3 || decoded[0] != '/' || !IsAsciiAlpha(decoded[1]) || decoded[2] != ':')
            return Fail(error, ErrorCode::resource_denied, "Windows file URL must use a local drive path");
        if (decoded.size() > 3 && decoded[3] != '/')
            return Fail(error, ErrorCode::resource_denied, "Malformed Windows file URL path");
        *path = fs::u8path(decoded.substr(1));
#else
        *path = fs::u8path(decoded);
#endif
    } catch (const fs::filesystem_error&) {
        return Fail(error, ErrorCode::resource_denied, "File URL path cannot be represented by the local filesystem");
    }
    return true;
}

bool ParsedToSegments(const ParsedUrlPath& parsed, const fs::path& root,
    std::vector<std::string>* segments, Error* error)
{
    segments->clear();
    if (parsed.kind == ParsedUrlPath::Kind::file) {
        fs::path path;
        if (!FileUrlToPath(parsed.decoded, &path, error))
            return false;
        return AbsolutePathToSegments(root, path, segments, error);
    }
    return AppendPath(parsed.decoded, segments, error);
}

bool ResolveRequest(std::string_view url, std::string_view base_url,
    const fs::path& root, std::vector<std::string>* segments, Error* error)
{
    ParsedUrlPath request;
    if (!ParseUrlPath(url, &request, error))
        return false;

    if (request.kind != ParsedUrlPath::Kind::relative) {
        if (!ParsedToSegments(request, root, segments, error))
            return false;
    } else if (request.empty_path) {
        if (base_url.empty())
            return Fail(error, ErrorCode::invalid_argument, "Local resource URL is empty and has no base URL");
        ParsedUrlPath base;
        if (!ParseUrlPath(base_url, &base, error))
            return false;
        if (!ParsedToSegments(base, root, segments, error))
            return false;
    } else {
        segments->clear();
        if (!base_url.empty()) {
            ParsedUrlPath base;
            if (!ParseUrlPath(base_url, &base, error))
                return false;
            if (!ParsedToSegments(base, root, segments, error))
                return false;
            if (!base.trailing_slash && !segments->empty())
                segments->pop_back();
        }
        if (!AppendPath(request.decoded, segments, error))
            return false;
    }

    if (segments->empty())
        return Fail(error, ErrorCode::resource_denied, "A local resource URL must identify a file below the configured root");
    return true;
}

std::string MimeTypeForPath(const fs::path& path)
{
    std::string extension = PathComponentUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), AsciiLower);
    if (extension == ".html" || extension == ".htm") return "text/html";
    if (extension == ".xhtml") return "application/xhtml+xml";
    if (extension == ".css") return "text/css";
    if (extension == ".js" || extension == ".mjs") return "text/javascript";
    if (extension == ".json" || extension == ".map") return "application/json";
    if (extension == ".xml") return "application/xml";
    if (extension == ".txt") return "text/plain";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".avif") return "image/avif";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".bmp") return "image/bmp";
    if (extension == ".woff") return "font/woff";
    if (extension == ".woff2") return "font/woff2";
    if (extension == ".ttf") return "font/ttf";
    if (extension == ".otf") return "font/otf";
    if (extension == ".eot") return "application/vnd.ms-fontobject";
    if (extension == ".wasm") return "application/wasm";
    return "application/octet-stream";
}

bool IsUrlUnreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

std::string PercentEncodePath(std::string_view path)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(path.size());
    for (const unsigned char c : path) {
        if (IsUrlUnreserved(c) || c == '/' || c == ':') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0f]);
        }
    }
    return encoded;
}

#if defined(_WIN32)

std::wstring StripExtendedPrefix(std::wstring path)
{
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return L"\\\\" + path.substr(8);
    if (path.rfind(L"\\\\?\\", 0) == 0)
        return path.substr(4);
    return path;
}

bool IsUncOrDevicePath(std::wstring_view path)
{
    return path.rfind(L"\\\\", 0) == 0 || path.rfind(L"//", 0) == 0
        || path.rfind(L"\\??\\", 0) == 0 || path.rfind(L"\\.\\", 0) == 0;
}

std::wstring ExtendedPath(const fs::path& path)
{
    const std::wstring native = path.native();
    if (native.rfind(L"\\\\?\\", 0) == 0)
        return native;
    return L"\\\\?\\" + native;
}

bool FinalPath(HANDLE handle, fs::path* path, Error* error)
{
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, kFlags);
    if (!needed)
        return Fail(error, ErrorCode::io, "Cannot determine the final local resource path");
    std::wstring value(needed, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, value.data(), needed, kFlags);
    if (!length || length >= needed)
        return Fail(error, ErrorCode::io, "Cannot determine the final local resource path");
    value.resize(length);
    value = StripExtendedPrefix(std::move(value));
    if (IsUncOrDevicePath(value))
        return Fail(error, ErrorCode::resource_denied, "UNC and device paths are disabled");
    *path = fs::path(std::move(value)).lexically_normal();
    return true;
}

std::string WindowsMessage(DWORD code)
{
    return std::error_code(static_cast<int>(code), std::system_category()).message();
}

ErrorCode OpenErrorCode(DWORD code)
{
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_NAME)
        return ErrorCode::not_found;
    return ErrorCode::io;
}

bool WideToUtf8(std::wstring_view input, std::string* output)
{
    if (input.empty()) {
        output->clear();
        return true;
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return false;
    output->resize(static_cast<std::size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
               static_cast<int>(input.size()), output->data(), size, nullptr, nullptr)
        == size;
}

std::string FileUrlForPath(const fs::path& path)
{
    std::wstring generic = path.native();
    std::replace(generic.begin(), generic.end(), L'\\', L'/');
    std::string utf8;
    if (!WideToUtf8(generic, &utf8))
        return {};
    return "file:///" + PercentEncodePath(utf8);
}

#else

std::string PosixMessage(int code)
{
    return std::error_code(code, std::generic_category()).message();
}

ErrorCode OpenErrorCode(int code)
{
    if (code == ENOENT || code == ENOTDIR)
        return ErrorCode::not_found;
    if (code == ELOOP || code == EXDEV)
        return ErrorCode::resource_denied;
    return ErrorCode::io;
}

#if defined(__linux__)
bool PathForFd(int fd, fs::path* path)
{
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    std::array<char, 4096> buffer {};
    const ssize_t length = readlink(link.c_str(), buffer.data(), buffer.size() - 1);
    if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size() - 1)
        return false;
    buffer[static_cast<std::size_t>(length)] = '\0';
    *path = fs::path(buffer.data()).lexically_normal();
    return true;
}
#endif

std::string FileUrlForPath(const fs::path& path)
{
    const std::string utf8 = PathComponentUtf8(path);
    return "file://" + PercentEncodePath(utf8);
}

#endif

} // namespace

struct LocalResources::NativeRoot {
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~NativeRoot()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    int fd = -1;
    ~NativeRoot()
    {
        if (fd >= 0)
            close(fd);
    }
#endif
};

LocalResources::LocalResources(fs::path root, std::unique_ptr<NativeRoot> native_root)
    : root_(std::move(root))
    , native_root_(std::move(native_root))
{
}

LocalResources::~LocalResources() = default;

std::unique_ptr<LocalResources> LocalResources::Open(const fs::path& root, Error* error)
{
    ClearError(error);
    if (root.empty()) {
        Fail(error, ErrorCode::invalid_argument, "Local resource root is empty");
        return nullptr;
    }

    try {
#if defined(_WIN32)
        const std::wstring raw = root.native();
        if (IsUncOrDevicePath(raw) || (root.has_root_name() && !root.has_root_directory())) {
            Fail(error, ErrorCode::resource_denied, "UNC, device, and drive-relative resource roots are disabled");
            return nullptr;
        }
        std::error_code absolute_error;
        fs::path absolute = fs::absolute(root, absolute_error).lexically_normal();
        if (absolute_error) {
            Fail(error, ErrorCode::io, "Cannot make the local resource root absolute: " + absolute_error.message());
            return nullptr;
        }
        auto native_root = std::make_unique<NativeRoot>();
        HANDLE handle = CreateFileW(ExtendedPath(absolute).c_str(),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            Fail(error, OpenErrorCode(code), "Cannot open the local resource root: " + WindowsMessage(code));
            return nullptr;
        }
        native_root->handle = handle;

        BY_HANDLE_FILE_INFORMATION information {};
        if (!GetFileInformationByHandle(handle, &information)
            || !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            Fail(error, ErrorCode::invalid_argument, "Local resource root is not a directory");
            return nullptr;
        }
        fs::path final_root;
        if (!FinalPath(handle, &final_root, error))
            return nullptr;
        return std::unique_ptr<LocalResources>(
            new LocalResources(std::move(final_root), std::move(native_root)));
#else
        std::error_code canonical_error;
        fs::path canonical = fs::canonical(root, canonical_error);
        if (canonical_error) {
            const ErrorCode code = canonical_error == std::errc::no_such_file_or_directory
                ? ErrorCode::not_found : ErrorCode::io;
            Fail(error, code, "Cannot resolve the local resource root: " + canonical_error.message());
            return nullptr;
        }
        auto native_root = std::make_unique<NativeRoot>();
        const int fd = open(canonical.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            const int code = errno;
            Fail(error, code == ENOTDIR ? ErrorCode::invalid_argument : OpenErrorCode(code),
                "Cannot open the local resource root: " + PosixMessage(code));
            return nullptr;
        }
        native_root->fd = fd;
#if defined(__linux__)
        fs::path actual_root;
        if (PathForFd(fd, &actual_root))
            canonical = std::move(actual_root);
#endif
        return std::unique_ptr<LocalResources>(
            new LocalResources(std::move(canonical), std::move(native_root)));
#endif
    } catch (const std::bad_alloc&) {
        Fail(error, ErrorCode::io, "Not enough memory to open the local resource root");
    } catch (const fs::filesystem_error& exception) {
        Fail(error, ErrorCode::io, "Cannot open the local resource root: " + std::string(exception.what()));
    }
    return nullptr;
}

bool LocalResources::OpenChecked(const std::vector<std::string>& segments,
    bool metadata, base::File* file, fs::path* final_path, Error* error) const
{
#if defined(_WIN32)
    // Pin every traversed directory against rename/reparse replacement until
    // final-path validation completes. Never follow a child reparse point.
    BY_HANDLE_FILE_INFORMATION pinned {};
    if (!GetFileInformationByHandle(native_root_->handle, &pinned))
        return Fail(error, ErrorCode::io, "Cannot inspect the pinned resource root");
    base::File root_check(CreateFileW(ExtendedPath(root_).c_str(),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION actual {};
    if (!root_check.IsValid()
        || !GetFileInformationByHandle(root_check.GetPlatformFile(), &actual)
        || (actual.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || actual.dwVolumeSerialNumber != pinned.dwVolumeSerialNumber
        || actual.nFileIndexHigh != pinned.nFileIndexHigh
        || actual.nFileIndexLow != pinned.nFileIndexLow)
        return Fail(error, ErrorCode::resource_denied, "Local resource root identity changed");
    fs::path requested = root_;
    std::vector<base::File> ancestors;
    ancestors.reserve(segments.size());
    base::File opened = std::move(root_check);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        requested /= fs::u8path(segments[i]);
        const bool last = i + 1 == segments.size();
        ancestors.push_back(std::move(opened));
        opened = base::File(CreateFileW(ExtendedPath(requested).c_str(),
            last && !metadata ? GENERIC_READ : FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT
                | (last && !metadata ? FILE_FLAG_SEQUENTIAL_SCAN : 0),
            nullptr));
        if (!opened.IsValid()) {
            const DWORD code = GetLastError();
            return Fail(error, OpenErrorCode(code), "Cannot open local resource: " + WindowsMessage(code));
        }
        BY_HANDLE_FILE_INFORMATION information {};
        if (!GetFileInformationByHandle(opened.GetPlatformFile(), &information))
            return Fail(error, ErrorCode::io, "Cannot inspect the opened local resource");
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || (!last && !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)))
            return Fail(error, ErrorCode::resource_denied, "Local resource reparse points are disabled");
    }
    if (!FinalPath(opened.GetPlatformFile(), final_path, error)
        || !AbsolutePathToSegments(root_, *final_path, nullptr, error, true))
        return false;
    *file = std::move(opened);
#else
    int fd = -1;
#if defined(__linux__)
    fs::path pinned_path;
    if (!PathForFd(native_root_->fd, &pinned_path) || pinned_path != root_)
        return Fail(error, ErrorCode::resource_denied, "Local resource root moved");
#else
    base::File root_check(open(root_.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat pinned {};
    struct stat actual {};
    if (!root_check.IsValid()
        || fstat(native_root_->fd, &pinned) != 0
        || fstat(root_check.GetPlatformFile(), &actual) != 0
        || pinned.st_dev != actual.st_dev || pinned.st_ino != actual.st_ino)
        return Fail(error, ErrorCode::resource_denied, "Local resource root identity changed");
#endif
#if defined(MINI_UI_HAS_OPENAT2) && defined(SYS_openat2)
    std::string relative_path;
    for (const std::string& segment : segments) {
        if (!relative_path.empty())
            relative_path.push_back('/');
        relative_path += segment;
    }
    if (relative_path.empty())
        relative_path = ".";
    open_how how {};
    how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS;
    fd = static_cast<int>(syscall(SYS_openat2, native_root_->fd,
        relative_path.c_str(), &how, sizeof(how)));
    if (fd < 0 && errno != ENOSYS && errno != EINVAL) {
        const int code = errno;
        return Fail(error, OpenErrorCode(code), "Cannot open local resource: " + PosixMessage(code));
    }
#endif
    if (fd < 0) {
        int current = fcntl(native_root_->fd, F_DUPFD_CLOEXEC, 0);
        if (current < 0)
            return Fail(error, ErrorCode::io, "Cannot duplicate the local resource root handle");
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const bool last = i + 1 == segments.size();
            const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (last ? 0 : O_DIRECTORY);
            const int next = openat(current, segments[i].c_str(), flags);
            const int code = errno;
            close(current);
            current = next;
            if (current < 0)
                return Fail(error, OpenErrorCode(code), "Cannot open local resource: " + PosixMessage(code));
        }
        fd = current;
    }
    base::File opened(fd);
    struct stat information {};
    if (fstat(fd, &information) != 0)
        return Fail(error, ErrorCode::io, "Cannot inspect the opened local resource: " + PosixMessage(errno));
    if (!S_ISREG(information.st_mode) && !(metadata && S_ISDIR(information.st_mode)))
        return Fail(error, ErrorCode::resource_denied, "Local resource is not a regular file or permitted directory");
#if defined(__linux__)
    if (!PathForFd(fd, final_path)
        || !AbsolutePathToSegments(root_, *final_path, nullptr, error))
        return false;
#else
    *final_path = root_;
    for (const std::string& segment : segments)
        *final_path /= fs::u8path(segment);
#endif
    *file = std::move(opened);
#endif
    return true;
}

bool LocalResources::GetFileInfo(const base::FilePath& path,
    base::File::Info* out) const
{
    if (!out || !path.IsAbsolute()
        || path.value().find(base::FilePath::CharType(0)) != base::FilePath::StringType::npos)
        return false;
    try {
        const fs::path candidate(path.value());
#if defined(_WIN32)
        if (IsUncOrDevicePath(candidate.native()))
            return false;
#endif
        // Do not normalize away traversal in native metadata requests.
        for (const auto& component : candidate) {
            if (component == fs::path(".."))
                return false;
        }
        std::vector<std::string> segments;
        if (!AbsolutePathToSegments(root_, candidate, &segments, nullptr))
            return false;
        base::File file;
        fs::path final_path;
        if (!OpenChecked(segments, true, &file, &final_path, nullptr))
            return false;
        return file.GetInfo(out);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const fs::filesystem_error&) {
        return false;
    }
}

base::File LocalResources::OpenFile(const base::FilePath& path,
    Error* error) const
{
    ClearError(error);
    if (!path.IsAbsolute()
        || path.value().find(base::FilePath::CharType(0)) != base::FilePath::StringType::npos) {
        Fail(error, ErrorCode::resource_denied, "Local file path must be native and absolute");
        return {};
    }
    try {
        const fs::path candidate(path.value());
#if defined(_WIN32)
        if (IsUncOrDevicePath(candidate.native())) {
            Fail(error, ErrorCode::resource_denied, "Local file device and UNC paths are disabled");
            return {};
        }
#endif
        for (const auto& component : candidate) {
            if (component == fs::path("..")) {
                Fail(error, ErrorCode::resource_denied, "Local file traversal is disabled");
                return {};
            }
        }
        std::vector<std::string> segments;
        if (!AbsolutePathToSegments(root_, candidate, &segments, error))
            return {};
        base::File file;
        fs::path final_path;
        if (!OpenChecked(segments, false, &file, &final_path, error))
            return {};
        base::File::Info info;
        if (!file.GetInfo(&info)) {
            Fail(error, ErrorCode::io, "Cannot inspect the opened local file");
            return {};
        }
        if (info.is_directory) {
            Fail(error, ErrorCode::resource_denied, "Local file path identifies a directory");
            return {};
        }
        return file;
    } catch (const std::bad_alloc&) {
        Fail(error, ErrorCode::io, "Not enough memory to open the local file");
    } catch (const fs::filesystem_error&) {
        Fail(error, ErrorCode::io, "Cannot open the local file");
    }
    return {};
}

bool LocalResources::Read(std::string_view url, std::string_view base_url,
    Resource* out, Error* error) const
{
    ClearError(error);
    if (!out)
        return Fail(error, ErrorCode::invalid_argument, "Local resource output is null");
    *out = {};

    try {
        std::vector<std::string> segments;
        if (!ResolveRequest(url, base_url, root_, &segments, error))
            return false;

        base::File file;
        fs::path final_path;
        if (!OpenChecked(segments, false, &file, &final_path, error))
            return false;
#if defined(_WIN32)
        const HANDLE handle = file.GetPlatformFile();
        BY_HANDLE_FILE_INFORMATION information {};
        if (!GetFileInformationByHandle(handle, &information))
            return Fail(error, ErrorCode::io, "Cannot inspect the opened local resource");
        if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            return Fail(error, ErrorCode::io, "Local resource URL identifies a directory, not a file");
        LARGE_INTEGER file_size {};
        if (!GetFileSizeEx(handle, &file_size) || file_size.QuadPart < 0)
            return Fail(error, ErrorCode::io, "Cannot determine local resource size");
        const unsigned long long size = static_cast<unsigned long long>(file_size.QuadPart);
        if (size > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())
            || size > static_cast<unsigned long long>(std::vector<std::uint8_t>().max_size())) {
            return Fail(error, ErrorCode::io, "Local resource is too large to read");
        }

        Resource resource;
        resource.bytes.resize(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < resource.bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                resource.bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD read = 0;
            if (!ReadFile(handle, resource.bytes.data() + offset, chunk, &read, nullptr))
                return Fail(error, ErrorCode::io, "Cannot read local resource: " + WindowsMessage(GetLastError()));
            if (!read)
                return Fail(error, ErrorCode::io, "Local resource changed while it was being read");
            offset += read;
        }
        resource.mime_type = MimeTypeForPath(final_path);
        resource.url = FileUrlForPath(final_path);
        if (resource.url.empty())
            return Fail(error, ErrorCode::io, "Cannot encode the final local resource URL");
        *out = std::move(resource);
        return true;
#else

        const int fd = file.GetPlatformFile();

        struct stat information {};
        if (fstat(fd, &information) != 0)
            return Fail(error, ErrorCode::io, "Cannot inspect the opened local resource: " + PosixMessage(errno));
        if (!S_ISREG(information.st_mode))
            return Fail(error, ErrorCode::io, "Local resource URL does not identify a regular file");
        if (information.st_size < 0
            || static_cast<std::uintmax_t>(information.st_size) > std::vector<std::uint8_t>().max_size()) {
            return Fail(error, ErrorCode::io, "Local resource is too large to read");
        }


        Resource resource;
        resource.bytes.resize(static_cast<std::size_t>(information.st_size));
        std::size_t offset = 0;
        while (offset < resource.bytes.size()) {
            const std::size_t remaining = resource.bytes.size() - offset;
            const ssize_t count = read(fd, resource.bytes.data() + offset,
                std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())));
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                return Fail(error, ErrorCode::io, "Cannot read local resource: " + PosixMessage(errno));
            }
            if (count == 0)
                return Fail(error, ErrorCode::io, "Local resource changed while it was being read");
            offset += static_cast<std::size_t>(count);
        }
        resource.mime_type = MimeTypeForPath(final_path);
        resource.url = FileUrlForPath(final_path);
        *out = std::move(resource);
        return true;
#endif
    } catch (const std::bad_alloc&) {
        return Fail(error, ErrorCode::io, "Not enough memory to read the local resource");
    } catch (const fs::filesystem_error& exception) {
        return Fail(error, ErrorCode::io, "Cannot read the local resource: " + std::string(exception.what()));
    }
}

} // namespace mini_ui
