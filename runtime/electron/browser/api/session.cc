// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/browser/api/session.h"

#include "runtime/electron/node_bindings.h"
#include "runtime/electron/browser/api/web_request.h"
#include "runtime/electron/browser/api/download_item.h"
#include "runtime/electron/browser/api/web_contents.h"
#include "runtime/electron/browser/api/protocol_interface.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/file_util.h"
#include "runtime/electron/common/string_util.h"
#include "runtime/electron/common/id_live_detect.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/promise.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/storage/storage_def.h"
#include "runtime/storage/default_local_storage_dir.h"
#include "runtime/storage/stock_profile_migrator.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libuv/include/uv.h"
#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/base_paths.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/hash/sha1.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/memory/scoped_refptr.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "net/base/filename_util.h"
#include "net/base/ip_address.h"
#include "net/base/mime_util.h"
#include "third_party/libcurl/include/curl/curl.h"
#include "third_party/libcurl/include/curl/header.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <limits>
#include <optional>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <string_view>
#include <utility>
#include <vector>
#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/select.h>
#include <netdb.h>
#endif

namespace atom {

namespace {

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)

std::string BrokerResourceMimeType(const base::FilePath& path)
{
    const std::string extension = base::ToLowerASCII(
        base::FilePath(path.FinalExtension()).AsUTF8Unsafe());
    if (extension == ".html")
        return "text/html";
    if (extension == ".js" || extension == ".mjs")
        return "text/javascript";
    if (extension == ".css")
        return "text/css";
    if (extension == ".json" || extension == ".map")
        return "application/json";
    if (extension == ".wasm")
        return "application/wasm";
    if (extension == ".svg")
        return "image/svg+xml";
    if (extension == ".png")
        return "image/png";
    if (extension == ".avif")
        return "image/avif";
    if (extension == ".webp")
        return "image/webp";
    if (extension == ".gif")
        return "image/gif";
    if (extension == ".jpg" || extension == ".jpeg")
        return "image/jpeg";
    if (extension == ".woff")
        return "font/woff";
    if (extension == ".woff2")
        return "font/woff2";
    if (extension == ".ttf")
        return "font/ttf";
    return "application/octet-stream";
}
#endif

void EnsureBrokerCurlInitialized()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

bool WaitForCurlSocket(CURL* curl, bool readable)
{
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD) {
        return false;
    }

    for (;;) {
        fd_set read_sockets;
        fd_set write_sockets;
        fd_set error_sockets;
        FD_ZERO(&read_sockets);
        FD_ZERO(&write_sockets);
        FD_ZERO(&error_sockets);
        if (readable)
            FD_SET(socket, &read_sockets);
        else
            FD_SET(socket, &write_sockets);
        FD_SET(socket, &error_sockets);

        timeval timeout = { 0, 100 * 1000 };
        int result = select(
#if BUILDFLAG(IS_WIN)
            0,
#else
            socket + 1,
#endif
            readable ? &read_sockets : nullptr,
            readable ? nullptr : &write_sockets, &error_sockets, &timeout);
        if (result >= 0)
            return result == 0 || !FD_ISSET(socket, &error_sockets);
#if BUILDFLAG(IS_WIN)
        return false;
#else
        if (errno != EINTR)
            return false;
#endif
    }
}

void BrokerCurlShareLock(CURL*, curl_lock_data,
    curl_lock_access, void* user_data)
{
    static_cast<base::Lock*>(user_data)->Acquire();
}

void BrokerCurlShareUnlock(CURL*, curl_lock_data, void* user_data)
{
    static_cast<base::Lock*>(user_data)->Release();
}

constexpr size_t kMaximumBrokerBodySize = 60 * 1024 * 1024;

struct BrokerNetworkRequest {
    std::string url;
    std::string method;
    std::string referrer;
    std::string initiator_origin;
    std::string mode;
    std::string credentials_mode;
    std::string resource_type;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    std::vector<base::FilePath> application_resource_roots;
    std::vector<base::FilePath> excluded_file_roots;
    bool web_security = true;
    bool allow_running_insecure_content = false;
    bool initiator_is_local_application = false;
    bool force_preflight = false;
    CURLSH* cookie_share = nullptr;
    std::shared_ptr<std::atomic_bool> canceled;
    std::function<bool(int, const base::Value::Dict&)> headers_sink;
    std::function<size_t(const char*, size_t)> body_sink;
};

struct BrokerNetworkResult {
    base::Value::Dict value;
    std::string error;
};

struct CurlResponse {
    std::vector<uint8_t> body;
    base::Value::Dict headers;
    std::string status_text;
    bool too_large = false;
    int status = 0;
    const BrokerNetworkRequest* request = nullptr;
};

size_t WriteCurlBody(char* bytes, size_t size, size_t count, void* parameter)
{
    auto* response = static_cast<CurlResponse*>(parameter);
    size_t length = size * count;
    if (response->request && response->request->body_sink) {
        if (response->status < 200 || response->status >= 300)
            return length;
        return response->request->body_sink(bytes, length);
    }
    if (length > kMaximumBrokerBodySize - response->body.size()) {
        response->too_large = true;
        return 0;
    }
    const auto* first = reinterpret_cast<const uint8_t*>(bytes);
    response->body.insert(response->body.end(), first, first + length);
    return length;
}

std::string TrimHeader(std::string value)
{
    base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
    return value;
}

size_t WriteCurlHeader(char* bytes, size_t size, size_t count, void* parameter)
{
    auto* response = static_cast<CurlResponse*>(parameter);
    size_t length = size * count;
    std::string line(bytes, length);
    if (base::StartsWith(line, "HTTP/", base::CompareCase::INSENSITIVE_ASCII)) {
        response->headers.clear();
        size_t first_space = line.find(' ');
        size_t second_space = first_space == std::string::npos
            ? std::string::npos : line.find(' ', first_space + 1);
        if (first_space != std::string::npos)
            base::StringToInt(line.substr(first_space + 1,
                second_space - first_space - 1), &response->status);
        response->status_text = second_space == std::string::npos
            ? std::string() : TrimHeader(line.substr(second_space + 1));
        return length;
    }
    if ((line == "\r\n" || line == "\n") && response->status >= 200
        && (response->status < 300 || response->status >= 400)
        && response->request && response->request->headers_sink
        && !response->request->headers_sink(response->status, response->headers))
        return 0;
    size_t separator = line.find(':');
    if (separator != std::string::npos) {
        std::string name = TrimHeader(line.substr(0, separator));
        std::string value = TrimHeader(line.substr(separator + 1));
        if (!name.empty()) {
            for (const auto& item : response->headers) {
                if (base::EqualsCaseInsensitiveASCII(item.first, name)) {
                    std::string* existing =
                        response->headers.FindString(item.first);
                    *existing += "\n" + value;
                    return length;
                }
            }
            response->headers.Set(std::move(name), std::move(value));
        }
    }
    return length;
}

const std::string* FindHeader(const base::Value::Dict& headers,
    std::string_view name)
{
    for (const auto& item : headers) {
        if (base::EqualsCaseInsensitiveASCII(item.first, name))
            return item.second.GetIfString();
    }
    return nullptr;
}


bool SameOrigin(const GURL& first, const GURL& second)
{
    url::Origin first_origin = url::Origin::Create(first);
    url::Origin second_origin = url::Origin::Create(second);
    return !first_origin.opaque() && !second_origin.opaque()
        && first_origin.IsSameOriginWith(second_origin);
}

bool IsPotentiallyPrivateHost(const GURL& url)
{
    const std::string host = url.HostNoBrackets();
    if (host == "localhost"
        || base::EndsWith(host, ".localhost",
            base::CompareCase::INSENSITIVE_ASCII)) {
        return true;
    }
    net::IPAddress address;
    return address.AssignFromIPLiteral(host)
        && !address.IsPubliclyRoutable();
}

bool IsLocalApplicationOrigin(const GURL& url)
{
    if (!url.is_valid())
        return false;
    return url.SchemeIsFile()
        || (!url.SchemeIsHTTPOrHTTPS() && !url.SchemeIs("data")
            && !url.SchemeIs("about") && !url.SchemeIs("blob"));
}

struct ResolvedEndpoint {
    std::string curl_resolution;
    bool is_private = true;
};

bool IsPrivateNumericAddress(std::string numeric)
{
    size_t zone = numeric.find('%');
    if (zone != std::string::npos)
        numeric.resize(zone);
    std::string lower = base::ToLowerASCII(numeric);
    constexpr std::string_view kMappedPrefix = "::ffff:";
    if (base::StartsWith(lower, kMappedPrefix,
            base::CompareCase::SENSITIVE)) {
        net::IPAddress mapped;
        if (mapped.AssignFromIPLiteral(
                lower.substr(kMappedPrefix.size()))) {
            return !mapped.IsPubliclyRoutable();
        }
    }
    net::IPAddress address;
    return !address.AssignFromIPLiteral(numeric)
        || !address.IsPubliclyRoutable();
}

bool ResolvePinnedEndpoint(const GURL& url, ResolvedEndpoint* endpoint)
{
    const std::string host = url.HostNoBrackets();
    const int port = url.EffectiveIntPort();
    if (host.empty() || port <= 0)
        return false;
    net::IPAddress literal;
    std::string literal_host = host;
    size_t zone = literal_host.find('%');
    if (zone != std::string::npos)
        literal_host.resize(zone);
    if (literal.AssignFromIPLiteral(literal_host)) {
        endpoint->curl_resolution.clear();
        endpoint->is_private = IsPrivateNumericAddress(host);
        return true;
    }
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const std::string service = base::NumberToString(port);
    if (getaddrinfo(host.c_str(), service.c_str(),
            &hints, &addresses) != 0) {
        return false;
    }
    std::vector<std::string> selected;
    std::optional<bool> selected_class;
    for (addrinfo* current = addresses; current; current = current->ai_next) {
        char numeric[NI_MAXHOST] = {};
        if (getnameinfo(current->ai_addr, current->ai_addrlen,
                numeric, sizeof(numeric), nullptr, 0, NI_NUMERICHOST) != 0) {
            continue;
        }
        const bool is_private = IsPrivateNumericAddress(numeric);
        if (!selected_class)
            selected_class = is_private;
        if (*selected_class != is_private
            || std::find(selected.begin(), selected.end(), numeric)
                != selected.end()) {
            continue;
        }
        std::string address = numeric;
        if (address.find(':') != std::string::npos)
            address = "[" + address + "]";
        selected.push_back(std::move(address));
    }
    freeaddrinfo(addresses);
    if (!selected_class || selected.empty())
        return false;
    std::string resolve_host = host;
    if (resolve_host.find(':') != std::string::npos)
        resolve_host = "[" + resolve_host + "]";
    endpoint->curl_resolution = resolve_host + ":" + service + ":"
        + base::JoinString(selected, ",");
    endpoint->is_private = *selected_class;
    return true;
}

bool IsSimpleCorsMethod(const std::string& method)
{
    return method == "GET" || method == "HEAD" || method == "POST";
}

bool IsSimpleCorsHeader(const std::string& name, const std::string& value)
{
    if (base::EqualsCaseInsensitiveASCII(name, "accept")
        || base::EqualsCaseInsensitiveASCII(name, "accept-language")
        || base::EqualsCaseInsensitiveASCII(name, "content-language")) {
        return value.size() <= 128;
    }
    if (!base::EqualsCaseInsensitiveASCII(name, "content-type"))
        return false;
    std::string mime = value.substr(0, value.find(';'));
    base::TrimWhitespaceASCII(mime, base::TRIM_ALL, &mime);
    return base::EqualsCaseInsensitiveASCII(mime, "application/x-www-form-urlencoded")
        || base::EqualsCaseInsensitiveASCII(mime, "multipart/form-data")
        || base::EqualsCaseInsensitiveASCII(mime, "text/plain");
}

bool IsForbiddenRequestHeader(std::string_view name)
{
    static constexpr std::string_view kForbidden[] = {
        "connection", "content-length", "cookie", "cookie2", "host",
        "keep-alive", "origin", "proxy-authorization",
        "proxy-connection", "referer", "te", "trailer",
        "transfer-encoding", "upgrade", "via"
    };
    for (std::string_view forbidden : kForbidden) {
        if (base::EqualsCaseInsensitiveASCII(name, forbidden))
            return true;
    }
    return base::StartsWith(name, "proxy-",
               base::CompareCase::INSENSITIVE_ASCII)
        || base::StartsWith(name, "sec-",
            base::CompareCase::INSENSITIVE_ASCII);
}
bool IsHttpToken(std::string_view value)
{
    if (value.empty())
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '!' || c == '#' || c == '$'
            || c == '%' || c == '&' || c == '\'' || c == '*'
            || c == '+' || c == '-' || c == '.' || c == '^'
            || c == '_' || c == '`' || c == '|' || c == '~';
    });
}

std::string BrokerResourceTypeName(int type)
{
    switch (type) {
    case 0: return "mainFrame";
    case 1: return "subFrame";
    case 2: return "stylesheet";
    case 3: return "script";
    case 4: return "image";
    case 5: return "font";
    case 7: return "object";
    case 8: return "media";
    case 13: return "xhr";
    case 14: return "ping";
    default: return "other";
    }
}

bool HeaderListContains(const std::string& list, std::string_view token)
{
    size_t offset = 0;
    while (offset <= list.size()) {
        size_t comma = list.find(',', offset);
        std::string item = list.substr(offset,
            comma == std::string::npos ? std::string::npos : comma - offset);
        base::TrimWhitespaceASCII(item, base::TRIM_ALL, &item);
        if (base::EqualsCaseInsensitiveASCII(item, token))
            return true;
        if (comma == std::string::npos)
            break;
        offset = comma + 1;
    }
    return false;
}

bool IsBrokerFileAllowed(const base::FilePath& path,
    const BrokerNetworkRequest& request)
{
    base::FilePath absolute = base::MakeAbsoluteFilePath(path);
    base::FilePath canonical;
    if (absolute.empty() || !base::NormalizeFilePath(absolute, &canonical)
        || base::DirectoryExists(canonical)) {
        return false;
    }
    for (const base::FilePath& excluded : request.excluded_file_roots) {
        base::FilePath excluded_absolute =
            base::MakeAbsoluteFilePath(excluded);
        base::FilePath canonical_excluded;
        if (!excluded_absolute.empty()
            && base::NormalizeFilePath(
                excluded_absolute, &canonical_excluded)
            && (canonical == canonical_excluded
                || canonical_excluded.IsParent(canonical))) {
            return false;
        }
    }
    for (const base::FilePath& root : request.application_resource_roots) {
        base::FilePath root_absolute = base::MakeAbsoluteFilePath(root);
        base::FilePath canonical_root;
        if (!root_absolute.empty()
            && base::NormalizeFilePath(root_absolute, &canonical_root)
            && (canonical == canonical_root
                || canonical_root.IsParent(canonical))) {
            return true;
        }
    }
    return false;
}

bool CorsResponseAllows(const CurlResponse& response,
    const BrokerNetworkRequest& request, bool credentials,
    bool private_network)
{
    const std::string* allow_origin =
        FindHeader(response.headers, "access-control-allow-origin");
    if (!allow_origin
        || (*allow_origin != request.initiator_origin
            && (*allow_origin != "*" || credentials))) {
        return false;
    }
    if (credentials) {
        const std::string* allowed =
            FindHeader(response.headers, "access-control-allow-credentials");
        if (!allowed
            || !base::EqualsCaseInsensitiveASCII(*allowed, "true")) {
            return false;
        }
    }
    if (private_network) {
        const std::string* allowed =
            FindHeader(response.headers,
                "access-control-allow-private-network");
        if (!allowed
            || !base::EqualsCaseInsensitiveASCII(*allowed, "true")) {
            return false;
        }
    }
    return true;
}
int AbortCanceledCurl(void* data, curl_off_t, curl_off_t,
    curl_off_t, curl_off_t)
{
    return static_cast<std::atomic_bool*>(data)->load(
        std::memory_order_acquire) ? 1 : 0;
}


bool RunCurlRequest(const BrokerNetworkRequest& request, const GURL& url,
    const ResolvedEndpoint& endpoint, const std::string& method,
    const std::vector<std::pair<std::string, std::string>>& headers,
    bool credentials, CurlResponse* response, long* status,
    std::string* content_type, std::string* error)
{
    response->request = &request;
    CURL* curl = curl_easy_init();
    if (!curl) {
        *error = "Unable to initialize network request";
        return false;
    }
    curl_slist* request_headers = nullptr;
    curl_slist* pinned_resolution = nullptr;
    if (!endpoint.curl_resolution.empty()) {
        pinned_resolution =
            curl_slist_append(nullptr, endpoint.curl_resolution.c_str());
        if (!pinned_resolution) {
            curl_easy_cleanup(curl);
            *error = "Unable to pin network endpoint";
            return false;
        }
    }
    for (const auto& header : headers) {
        std::string line = header.first + ": " + header.second;
        request_headers = curl_slist_append(request_headers, line.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.spec().c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCurlBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &WriteCurlHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    if (pinned_resolution)
        curl_easy_setopt(curl, CURLOPT_RESOLVE, pinned_resolution);
    if (credentials && request.cookie_share) {
        curl_easy_setopt(curl, CURLOPT_SHARE, request.cookie_share);
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    }
    if (!request.referrer.empty())
        curl_easy_setopt(curl, CURLOPT_REFERER, request.referrer.c_str());
    if (method != "GET" && method != "HEAD" && !request.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
            static_cast<curl_off_t>(request.body.size()));
    }
    if (request.canceled) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &AbortCanceledCurl);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, request.canceled.get());
    }
    CURLcode code = curl_easy_perform(curl);
    char* type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
    if (type)
        *content_type = type;
    curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
    curl_slist_free_all(request_headers);
    curl_slist_free_all(pinned_resolution);
    curl_easy_cleanup(curl);
    if (code == CURLE_OK)
        return true;
    *error = response->too_large
        ? "Network response exceeds renderer IPC limit"
        : curl_easy_strerror(code);
    return false;
}

BrokerNetworkResult PerformBrokerNetworkRequest(BrokerNetworkRequest request)
{
    BrokerNetworkResult output;
    GURL current(request.url);
    if (!current.is_valid()) {
        output.error = "Invalid network broker URL";
        return output;
    }
    if (current.SchemeIsFile()) {
        base::FilePath path;
        if ((request.method != "GET" && request.method != "HEAD")
            || !net::FileURLToFilePath(current, &path)
            || !IsBrokerFileAllowed(path, request)) {
            output.error = "File URL is outside authorized application roots";
            return output;
        }
        std::string bytes;
        if (!base::ReadFileToStringWithMaxSize(
                path, &bytes, kMaximumBrokerBodySize)) {
            output.error = "Unable to read brokered application resource";
            return output;
        }
        std::string mime_type;
        net::GetMimeTypeFromFile(path, &mime_type);
        output.value.Set("status", 200);
        output.value.Set("statusText", "OK");
        output.value.Set("finalUrl", request.url);
        output.value.Set("mimeType",
            mime_type.empty() ? "application/octet-stream" : mime_type);
        output.value.Set("headers", base::Value::Dict());
        std::vector<uint8_t> body;
        if (request.method != "HEAD")
            body.assign(bytes.begin(), bytes.end());
        output.value.Set("body", base::Value(std::move(body)));
        return output;
    }
    if (!current.SchemeIsHTTPOrHTTPS()) {
        output.error = "Network broker only permits HTTP, HTTPS, and file URLs";
        return output;
    }

    GURL initiator(request.initiator_origin);
    if (request.web_security
        && initiator.SchemeIsCryptographic()
        && !current.SchemeIsCryptographic()
        && !request.allow_running_insecure_content) {
        output.error = "Mixed-content request blocked";
        return output;
    }

    std::string method = request.method;
    for (int redirects = 0; redirects <= 10; ++redirects) {
        if (request.web_security
            && initiator.SchemeIsCryptographic()
            && !current.SchemeIsCryptographic()
            && !request.allow_running_insecure_content) {
            output.error = "Mixed-content redirect blocked";
            return output;
        }
        const bool cross_origin =
            !initiator.is_valid() || !SameOrigin(initiator, current);
        const bool enforce_cors = request.web_security && cross_origin
            && request.mode != "navigate";
        if (cross_origin && request.web_security
            && request.mode == "same-origin") {
            output.error = "Cross-origin request blocked";
            return output;
        }
        const bool credentials = request.credentials_mode == "include"
            || (request.credentials_mode == "same-origin" && !cross_origin);
        ResolvedEndpoint endpoint;
        if (!ResolvePinnedEndpoint(current, &endpoint)) {
            output.error = "Network target could not be resolved safely";
            return output;
        }
        const bool private_network = request.web_security
            && request.mode != "navigate" && endpoint.is_private
            && !(request.initiator_is_local_application
                && IsPotentiallyPrivateHost(current))
            && !IsPotentiallyPrivateHost(initiator);
        bool preflight = private_network
            || (enforce_cors && (request.force_preflight
                || !IsSimpleCorsMethod(method)));
        std::vector<std::string> non_simple_headers;
        for (const auto& header : request.headers) {
            if (!IsSimpleCorsHeader(header.first, header.second)) {
                non_simple_headers.push_back(
                    base::ToLowerASCII(header.first));
                preflight = preflight || enforce_cors;
            }
        }
        if (preflight) {
            std::vector<std::pair<std::string, std::string>> headers = {
                { "Origin", request.initiator_origin },
                { "Access-Control-Request-Method", method }
            };
            if (!non_simple_headers.empty()) {
                headers.emplace_back("Access-Control-Request-Headers",
                    base::JoinString(non_simple_headers, ", "));
            }
            if (private_network) {
                headers.emplace_back(
                    "Access-Control-Request-Private-Network", "true");
            }
            CurlResponse preflight_response;
            long preflight_status = 0;
            std::string ignored_type;
            if (!RunCurlRequest(request, current, endpoint, "OPTIONS",
                    headers, false, &preflight_response, &preflight_status,
                    &ignored_type, &output.error)) {
                return output;
            }
            const std::string* allow_method = FindHeader(
                preflight_response.headers, "access-control-allow-methods");
            const std::string* allow_headers = FindHeader(
                preflight_response.headers, "access-control-allow-headers");
            bool headers_allowed = non_simple_headers.empty();
            if (allow_headers) {
                headers_allowed = std::all_of(non_simple_headers.begin(),
                    non_simple_headers.end(),
                    [allow_headers](const std::string& header) {
                        return HeaderListContains(*allow_headers, header)
                            || HeaderListContains(*allow_headers, "*");
                    });
            }
            if (preflight_status < 200 || preflight_status >= 300
                || !allow_method
                || !HeaderListContains(*allow_method, method)
                || !headers_allowed
                || !CorsResponseAllows(preflight_response, request,
                    credentials, private_network)) {
                output.error = "CORS preflight rejected";
                return output;
            }
        }

        std::vector<std::pair<std::string, std::string>> headers =
            request.headers;
        if (enforce_cors || private_network)
            headers.emplace_back("Origin", request.initiator_origin);
        CurlResponse response;
        long status = 0;
        std::string content_type;
        if (!RunCurlRequest(request, current, endpoint, method, headers,
                credentials, &response, &status, &content_type,
                &output.error)) {
            return output;
        }
        if (enforce_cors
            && !CorsResponseAllows(response, request, credentials, false)) {
            output.error = "CORS response rejected";
            return output;
        }
        const std::string* location = FindHeader(response.headers, "location");
        if (status >= 300 && status < 400 && location) {
            if (redirects == 10) {
                output.error = "Too many network redirects";
                return output;
            }
            GURL next = current.Resolve(*location);
            if (!next.is_valid() || !next.SchemeIsHTTPOrHTTPS()) {
                output.error = "Network redirect target is not authorized";
                return output;
            }
            if ((status == 303 || ((status == 301 || status == 302)
                    && method == "POST"))) {
                method = "GET";
                request.body.clear();
            }
            if (!SameOrigin(current, next)) {
                request.referrer.clear();
                request.headers.erase(std::remove_if(
                    request.headers.begin(), request.headers.end(),
                    [](const auto& header) {
                        return base::EqualsCaseInsensitiveASCII(
                            header.first, "authorization");
                    }),
                    request.headers.end());
            }
            current = std::move(next);
            continue;
        }
        output.value.Set("status", static_cast<int>(status));
        output.value.Set("statusText", response.status_text);
        output.value.Set("finalUrl", current.spec());
        if (!content_type.empty())
            output.value.Set("mimeType", content_type);
        output.value.Set("headers", std::move(response.headers));
        output.value.Set("body", base::Value(std::move(response.body)));
        return output;
    }
    output.error = "Too many network redirects";
    return output;
}
} // namespace

struct BrokerSocketRegistry {
    base::Lock lock;
    std::map<std::pair<int, uint64_t>, std::shared_ptr<BrokerWebSocket>> sockets;
};


class BrokerWebSocket
    : public std::enable_shared_from_this<BrokerWebSocket> {
public:
    BrokerWebSocket(int contents_id, uint64_t socket_id, std::string url,
        std::string protocol, std::string origin,
        bool enforce_private_network, CURLSH* cookie_share,
        scoped_refptr<base::SequencedTaskRunner> main_runner,
        std::weak_ptr<BrokerSocketRegistry> registry)
        : contents_id_(contents_id)
        , socket_id_(socket_id)
        , url_(std::move(url))
        , protocol_(std::move(protocol))
        , origin_(std::move(origin))
        , enforce_private_network_(enforce_private_network)
        , cookie_share_(cookie_share)
        , main_runner_(std::move(main_runner))
        , registry_(std::move(registry))
    {
    }

    ~BrokerWebSocket() { Stop(); }

    void Start()
    {
        std::shared_ptr<BrokerWebSocket> self = shared_from_this();
        thread_ = std::thread([self] { self->Run(); });
    }

    void Queue(bool binary, std::vector<uint8_t> data)
    {
        std::lock_guard<std::mutex> lock(queue_lock_);
        queue_.push_back({ binary, false, 0, std::move(data) });
    }

    void Close(int code, std::string reason)
    {
        std::vector<uint8_t> payload;
        payload.reserve(reason.size() + 2);
        payload.push_back(static_cast<uint8_t>((code >> 8) & 0xff));
        payload.push_back(static_cast<uint8_t>(code & 0xff));
        payload.insert(payload.end(), reason.begin(), reason.end());
        std::lock_guard<std::mutex> lock(queue_lock_);
        queue_.push_back({ false, true, code, std::move(payload) });
    }

    void Stop()
    {
        stop_.store(true);
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id())
                thread_.detach();
            else
                thread_.join();
        }
    }

private:
    struct Outgoing {
        bool binary = false;
        bool close = false;
        int code = 0;
        std::vector<uint8_t> data;
    };

    void Emit(base::Value::Dict event)
    {
        event.Set("socketId", static_cast<double>(socket_id_));
        main_runner_->PostTask(FROM_HERE,
            base::BindOnce(
                [](int contents_id, base::Value::Dict value) {
                    WebContents::sendRendererMessage(contents_id,
                        "network.websocket-event", std::move(value));
                },
                contents_id_, std::move(event)));
    }

    bool SendFrame(CURL* curl, const Outgoing& outgoing)
    {
        size_t offset = 0;
        unsigned int flags = outgoing.close ? CURLWS_CLOSE
            : outgoing.binary ? CURLWS_BINARY : CURLWS_TEXT;
        do {
            size_t sent = 0;
            const void* data = outgoing.data.empty()
                ? nullptr : outgoing.data.data() + offset;
            CURLcode result = curl_ws_send(curl, data,
                outgoing.data.size() - offset, &sent,
                static_cast<curl_off_t>(outgoing.data.size()), flags);
            if (result == CURLE_AGAIN) {
                if (!WaitForCurlSocket(curl, false))
                    return false;
                continue;
            }
            if (result != CURLE_OK)
                return false;
            offset += sent;
            flags = CURLWS_CONT;
        } while (offset < outgoing.data.size());
        return true;
    }

    void Finished()
    {
        std::weak_ptr<BrokerSocketRegistry> registry = registry_;
        const auto key = std::make_pair(contents_id_, socket_id_);
        main_runner_->PostTask(FROM_HERE,
            base::BindOnce(
                [](std::weak_ptr<BrokerSocketRegistry> weak_registry,
                    std::pair<int, uint64_t> socket_key,
                    BrokerWebSocket* completed) {
                    std::shared_ptr<BrokerSocketRegistry> registry =
                        weak_registry.lock();
                    if (!registry)
                        return;
                    base::AutoLock lock(registry->lock);
                    auto found = registry->sockets.find(socket_key);
                    if (found != registry->sockets.end()
                        && found->second.get() == completed) {
                        registry->sockets.erase(found);
                    }
                },
                std::move(registry), key, this));
    }

    void Run()
    {
        ResolvedEndpoint endpoint;
        if (!ResolvePinnedEndpoint(GURL(url_), &endpoint)
            || (enforce_private_network_ && endpoint.is_private)) {
            base::Value::Dict event;
            event.Set("event", "error");
            event.Set("message", "WebSocket endpoint is not authorized");
            Emit(std::move(event));
            Finished();
            return;
        }
        CURL* curl = curl_easy_init();
        if (!curl) {
            base::Value::Dict event;
            event.Set("event", "error");
            event.Set("message", "Unable to initialize WebSocket");
            Emit(std::move(event));
            Finished();
            return;
        }
        curl_slist* pinned_resolution = nullptr;
        if (!endpoint.curl_resolution.empty()) {
            pinned_resolution =
                curl_slist_append(nullptr, endpoint.curl_resolution.c_str());
            if (!pinned_resolution) {
                base::Value::Dict event;
                event.Set("event", "error");
                event.Set("message", "Unable to pin WebSocket endpoint");
                Emit(std::move(event));
                curl_easy_cleanup(curl);
                Finished();
                return;
            }
        }
        curl_slist* headers = nullptr;
        if (!protocol_.empty()) {
            std::string header = "Sec-WebSocket-Protocol: " + protocol_;
            headers = curl_slist_append(headers, header.c_str());
        }
        if (!origin_.empty()) {
            std::string header = "Origin: " + origin_;
            headers = curl_slist_append(headers, header.c_str());
        }
        if (headers)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &AbortCanceledCurl);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stop_);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "ws,wss");
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        if (pinned_resolution)
            curl_easy_setopt(curl, CURLOPT_RESOLVE, pinned_resolution);
        if (cookie_share_) {
            curl_easy_setopt(curl, CURLOPT_SHARE, cookie_share_);
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
        }
        CURLcode connected = curl_easy_perform(curl);
        if (connected != CURLE_OK) {
            base::Value::Dict event;
            event.Set("event", "error");
            event.Set("message", curl_easy_strerror(connected));
            Emit(std::move(event));
            curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
            curl_slist_free_all(headers);
            curl_slist_free_all(pinned_resolution);
            curl_easy_cleanup(curl);
            Finished();
            return;
        }
        base::Value::Dict opened;
        opened.Set("event", "open");
        curl_header* selected_protocol = nullptr;
        if (curl_easy_header(curl, "Sec-WebSocket-Protocol", 0,
                CURLH_HEADER, -1, &selected_protocol) == CURLHE_OK
            && selected_protocol && selected_protocol->value) {
            opened.Set("protocol", selected_protocol->value);
        } else {
            opened.Set("protocol", "");
        }
        opened.Set("extensions", "");
        Emit(std::move(opened));

        std::vector<uint8_t> incoming;
        bool incoming_binary = false;
        bool close_sent = false;
        while (!stop_.load()) {
            std::deque<Outgoing> outgoing;
            {
                std::lock_guard<std::mutex> lock(queue_lock_);
                outgoing.swap(queue_);
            }
            for (const Outgoing& item : outgoing) {
                if (!SendFrame(curl, item)) {
                    base::Value::Dict event;
                    event.Set("event", "error");
                    event.Set("message", "WebSocket send failed");
                    Emit(std::move(event));
                    stop_.store(true);
                    break;
                }
                close_sent = close_sent || item.close;
                if (!item.close && !item.data.empty()) {
                    base::Value::Dict consumed;
                    consumed.Set("event", "sent");
                    consumed.Set("bytes", static_cast<int>(item.data.size()));
                    Emit(std::move(consumed));
                }
            }
            if (stop_.load())
                break;

            uint8_t buffer[64 * 1024];
            size_t received = 0;
            const curl_ws_frame* metadata = nullptr;
            CURLcode result = curl_ws_recv(curl, buffer, sizeof(buffer),
                &received, &metadata);
            if (result == CURLE_AGAIN) {
                if (WaitForCurlSocket(curl, true))
                    continue;
                result = CURLE_RECV_ERROR;
            }
            if (result != CURLE_OK || !metadata) {
                base::Value::Dict event;
                event.Set("event", "error");
                event.Set("message", result == CURLE_OK
                    ? "Invalid WebSocket frame" : curl_easy_strerror(result));
                Emit(std::move(event));
                break;
            }
            if (metadata->flags & CURLWS_CLOSE) {
                int code = 1000;
                std::string reason;
                if (received >= 2) {
                    code = (static_cast<int>(buffer[0]) << 8) | buffer[1];
                    reason.assign(reinterpret_cast<char*>(buffer + 2),
                        received - 2);
                }
                if (!close_sent) {
                    Outgoing reply;
                    reply.close = true;
                    reply.data.assign(buffer, buffer + received);
                    close_sent = SendFrame(curl, reply);
                }
                base::Value::Dict event;
                event.Set("event", "close");
                event.Set("code", code);
                event.Set("reason", std::move(reason));
                event.Set("clean", close_sent);
                Emit(std::move(event));
                break;
            }
            if (metadata->flags & CURLWS_PING) {
                size_t sent = 0;
                curl_ws_send(curl, buffer, received, &sent,
                    static_cast<curl_off_t>(received), CURLWS_PONG);
                continue;
            }
            if (incoming.empty())
                incoming_binary = !!(metadata->flags & CURLWS_BINARY);
            if (received > kMaximumBrokerBodySize - incoming.size()) {
                base::Value::Dict event;
                event.Set("event", "error");
                event.Set("message", "WebSocket message exceeds IPC limit");
                Emit(std::move(event));
                break;
            }
            incoming.insert(incoming.end(), buffer, buffer + received);
            if (metadata->bytesleft == 0) {
                base::Value::Dict event;
                event.Set("event", incoming_binary ? "binary" : "text");
                if (incoming_binary) {
                    event.Set("data", base::Value(std::move(incoming)));
                } else {
                    event.Set("data", std::string(incoming.begin(),
                        incoming.end()));
                    incoming.clear();
                }
                Emit(std::move(event));
                incoming.clear();
            }
        }
        curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
        curl_slist_free_all(headers);
        curl_slist_free_all(pinned_resolution);
        curl_easy_cleanup(curl);
        Finished();
    }

    int contents_id_;
    uint64_t socket_id_;
    std::string url_;
    std::string protocol_;
    std::string origin_;
    bool enforce_private_network_ = false;
    CURLSH* cookie_share_;
    scoped_refptr<base::SequencedTaskRunner> main_runner_;
    std::weak_ptr<BrokerSocketRegistry> registry_;
    std::atomic<bool> stop_ { false };
    std::mutex queue_lock_;
    std::deque<Outgoing> queue_;
    std::thread thread_;
};

ApiSession::ApiSession(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
    : m_brokerSocketRegistry(std::make_shared<BrokerSocketRegistry>())
{
    gin_helper::Wrappable<ApiSession>::InitWith(isolate, wrapper);
}

class BrokerRequestPipeline
    : public std::enable_shared_from_this<BrokerRequestPipeline> {
public:
    BrokerRequestPipeline(ApiSession* session, int contents_id,
        BrokerNetworkRequest request, ApiSession::BrokerReply reply)
        : session_(session)
        , contents_id_(contents_id)
        , request_(std::move(request))
        , reply_(std::move(reply))
    {
        static std::atomic<uint64_t> next_request_id { 1 };
        request_id_ =
            next_request_id.fetch_add(1, std::memory_order_relaxed);
    }

    void Start()
    {
        Invoke(Listener(&ApiWebRequest::m_beforeRequestCb,
                   "onBeforeRequest"),
            RequestDetails(), [self = shared_from_this()](
                base::Value::Dict response) {
                if (response.FindBool("cancel").value_or(false)) {
                    self->Fail("ERR_BLOCKED_BY_CLIENT");
                    return;
                }
                if (const std::string* redirect =
                        response.FindString("redirectURL");
                    redirect && GURL(*redirect).is_valid()) {
                    self->request_.url = *redirect;
                }
                self->BeforeSendHeaders();
            });
    }

private:
    struct CallbackState {
        std::function<void(base::Value::Dict)> callback;
        bool completed = false;
        v8::Global<v8::Function> function;
        scoped_refptr<base::SequencedTaskRunner> runner;
    };

    using ListenerMember = v8::Persistent<v8::Value> ApiWebRequest::*;

    v8::Persistent<v8::Value>* Listener(
        ListenerMember member, const char* event_name)
    {
        return session_->m_webRequest
                && session_->m_webRequest->matchesListener(
                    event_name, request_.url, request_.resource_type)
            ? &(session_->m_webRequest->*member) : nullptr;
    }

    static void DeleteCallbackState(
        const v8::WeakCallbackInfo<CallbackState>& info)
    {
        CallbackState* state = info.GetParameter();
        if (!state->completed && state->callback) {
            std::function<void(base::Value::Dict)> callback =
                std::move(state->callback);
            state->runner->PostTask(FROM_HERE,
                base::BindOnce(
                    [](std::function<void(base::Value::Dict)> continuation) {
                        base::Value::Dict response;
                        response.Set("cancel", true);
                        continuation(std::move(response));
                    },
                    std::move(callback)));
        }
        state->function.Reset();
        delete state;
    }

    static void Complete(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        auto* state = static_cast<CallbackState*>(
            info.Data().As<v8::External>()->Value());
        if (state->completed)
            return;
        state->completed = true;
        base::Value::Dict response;
        if (info.Length() && info[0]->IsObject())
            gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &response);
        std::function<void(base::Value::Dict)> callback =
            std::move(state->callback);
        callback(std::move(response));
    }

    void Invoke(v8::Persistent<v8::Value>* listener,
        base::Value::Dict details,
        std::function<void(base::Value::Dict)> continuation)
    {
        if (!listener || listener->IsEmpty()) {
            continuation({});
            return;
        }
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);
        v8::Local<v8::Function> function =
            listener->Get(isolate).As<v8::Function>();
        v8::Local<v8::Context> context =
            function->GetCreationContextChecked();
        v8::Context::Scope context_scope(context);
        auto* state = new CallbackState {
            std::move(continuation), false, {},
            base::SequencedTaskRunner::GetCurrentDefault()
        };
        v8::Local<v8::Function> callback = v8::Function::New(context,
            &Complete, v8::External::New(isolate, state)).ToLocalChecked();
        state->function.Reset(isolate, callback);
        state->function.SetWeak(state, &DeleteCallbackState,
            v8::WeakCallbackType::kParameter);
        v8::Local<v8::Value> arguments[] = {
            gin_helper::Converter<base::Value::Dict>::ToV8(
                isolate, details),
            callback
        };
        v8::MaybeLocal<v8::Value> called = function->Call(context,
            v8::Undefined(isolate), 2, arguments);
        if (called.IsEmpty() && !state->completed) {
            state->completed = true;
            state->callback = {};
            Fail("webRequest listener threw an exception");
        }
    }

    void Notify(v8::Persistent<v8::Value>* listener,
        base::Value::Dict details)
    {
        if (!listener || listener->IsEmpty())
            return;
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);
        v8::Local<v8::Function> function =
            listener->Get(isolate).As<v8::Function>();
        v8::Local<v8::Context> context =
            function->GetCreationContextChecked();
        v8::Context::Scope context_scope(context);
        v8::Local<v8::Value> argument =
            gin_helper::Converter<base::Value::Dict>::ToV8(
                isolate, details);
        function->Call(context, v8::Undefined(isolate), 1, &argument);
    }

    static std::string HeaderValue(const base::Value& value)
    {
        if (const std::string* text = value.GetIfString())
            return *text;
        const base::Value::List* values = value.GetIfList();
        if (!values)
            return {};
        std::string combined;
        for (const base::Value& entry : *values) {
            const std::string* text = entry.GetIfString();
            if (!text)
                continue;
            if (!combined.empty())
                combined += "\n";
            combined += *text;
        }
        return combined;
    }

    base::Value::Dict RequestHeaders() const
    {
        base::Value::Dict headers;
        for (const auto& header : request_.headers)
            headers.Set(header.first, header.second);
        return headers;
    }

    base::Value::Dict RequestDetails() const
    {
        base::Value::Dict details;
        details.Set("id", static_cast<double>(request_id_));
        details.Set("url", request_.url);
        details.Set("method", request_.method);
        details.Set("referrer", request_.referrer);
        details.Set("webContentsId", contents_id_);
        details.Set("resourceType", request_.resource_type);
        details.Set("requestHeaders", RequestHeaders());
        return details;
    }

    base::Value::Dict ResponseDetails() const
    {
        base::Value::Dict details = RequestDetails();
        int status = result_.value.FindInt("status").value_or(0);
        const std::string* text = result_.value.FindString("statusText");
        details.Set("statusCode", status);
        details.Set("statusLine", "HTTP/1.1 " + base::NumberToString(status)
            + " " + (text ? *text : std::string()));
        if (const base::Value::Dict* headers =
                result_.value.FindDict("headers")) {
            details.Set("responseHeaders", headers->Clone());
        }
        return details;
    }

    void BeforeSendHeaders()
    {
        Invoke(Listener(&ApiWebRequest::m_beforeSendHeadersCb,
                   "onBeforeSendHeaders"),
            RequestDetails(), [self = shared_from_this()](
                base::Value::Dict response) {
                if (response.FindBool("cancel").value_or(false)) {
                    self->Fail("ERR_BLOCKED_BY_CLIENT");
                    return;
                }
                if (base::Value::Dict* headers =
                        response.FindDict("requestHeaders")) {
                    self->request_.headers.clear();
                    for (const auto& item : *headers) {
                        std::string value = HeaderValue(item.second);
                        if (value.empty())
                            continue;
                        if (!IsHttpToken(item.first)
                            || IsForbiddenRequestHeader(item.first)
                            || value.find_first_of("\r\n")
                                != std::string::npos) {
                            self->Fail(
                                "webRequest supplied a forbidden request header");
                            return;
                        }
                        self->request_.headers.emplace_back(
                            item.first, std::move(value));
                    }
                }
                self->Notify(self->Listener(
                    &ApiWebRequest::m_sendHeadersCb, "onSendHeaders"),
                    self->RequestDetails());
                self->Perform();
            });
    }

    void Perform()
    {
        request_url_before_perform_ = request_.url;
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE, { base::MayBlock(), base::TaskPriority::USER_VISIBLE },
            base::BindOnce(&PerformBrokerNetworkRequest, request_),
            base::BindOnce(&BrokerRequestPipeline::Performed,
                shared_from_this()));
    }

    void Performed(BrokerNetworkResult result)
    {
        result_ = std::move(result);
        if (!result_.error.empty()) {
            Fail(result_.error);
            return;
        }
        const std::string* final_url =
            result_.value.FindString("finalUrl");
        if (final_url && *final_url != request_url_before_perform_) {
            base::Value::Dict redirect = ResponseDetails();
            redirect.Set("redirectURL", *final_url);
            Notify(Listener(&ApiWebRequest::m_beforeRedirectCb,
                       "onBeforeRedirect"),
                std::move(redirect));
        }
        Invoke(Listener(&ApiWebRequest::m_headersReceivedCb,
                   "onHeadersReceived"),
            ResponseDetails(), [self = shared_from_this()](
                base::Value::Dict response) {
                if (response.FindBool("cancel").value_or(false)) {
                    self->Fail("ERR_BLOCKED_BY_CLIENT");
                    return;
                }
                if (base::Value::Dict* headers =
                        response.FindDict("responseHeaders")) {
                    base::Value::Dict normalized;
                    for (const auto& item : *headers) {
                        std::string value = HeaderValue(item.second);
                        if (!value.empty())
                            normalized.Set(item.first, std::move(value));
                    }
                    self->result_.value.Set("headers",
                        std::move(normalized));
                }
                base::Value::Dict details = self->ResponseDetails();
                self->Notify(self->Listener(
                    &ApiWebRequest::m_responseStartedCb,
                    "onResponseStarted"), details.Clone());
                self->Notify(self->Listener(
                    &ApiWebRequest::m_completedCb, "onCompleted"),
                    std::move(details));
                ApiSession::BrokerReply reply =
                    std::move(self->reply_);
                reply(std::move(self->result_.value), {});
            });
    }

    void Fail(std::string error)
    {
        base::Value::Dict details = RequestDetails();
        details.Set("error", error);
        Notify(Listener(&ApiWebRequest::m_errorOccurredCb,
                   "onErrorOccurred"),
            std::move(details));
        if (reply_) {
            ApiSession::BrokerReply reply = std::move(reply_);
            reply({}, std::move(error));
        }
    }

    ApiSession* session_;
    int contents_id_;
    uint64_t request_id_ = 0;
    std::string request_url_before_perform_;
    BrokerNetworkRequest request_;
    BrokerNetworkResult result_;
    ApiSession::BrokerReply reply_;
};

ApiSession::~ApiSession()
{
    std::vector<std::shared_ptr<BrokerWebSocket>> sockets;
    if (m_brokerSocketRegistry) {
        base::AutoLock lock(m_brokerSocketRegistry->lock);
        for (auto& item : m_brokerSocketRegistry->sockets)
            sockets.push_back(item.second);
        m_brokerSocketRegistry->sockets.clear();
    }
    for (const auto& socket : sockets)
        socket->Stop();
    m_brokerSocketRegistry.reset();
    if (m_brokerCookieShare) {
        if (m_persistent) {
            CURL* flush = curl_easy_init();
            if (flush) {
                std::string cookie_path =
                    m_path.AppendASCII("cookie.dat").AsUTF8Unsafe();
                curl_easy_setopt(flush, CURLOPT_SHARE,
                    static_cast<CURLSH*>(m_brokerCookieShare));
                curl_easy_setopt(flush, CURLOPT_COOKIEJAR,
                    cookie_path.c_str());
                curl_easy_setopt(flush, CURLOPT_COOKIELIST, "FLUSH");
                curl_easy_setopt(flush, CURLOPT_SHARE, nullptr);
                curl_easy_cleanup(flush);
            }
        }
        curl_share_cleanup(static_cast<CURLSH*>(m_brokerCookieShare));
        m_brokerCookieShare = nullptr;
    }
    delete m_webRequest;
    if (m_removePathOnDestroy)
        base::DeletePathRecursively(m_path);
}

void* ApiSession::brokerCookieShare()
{
    base::AutoLock lock(m_brokerLock);
    if (m_brokerCookieShare)
        return m_brokerCookieShare;
    CURLSH* share = curl_share_init();
    if (!share)
        return nullptr;
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(share, CURLSHOPT_LOCKFUNC, &BrokerCurlShareLock);
    curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, &BrokerCurlShareUnlock);
    curl_share_setopt(share, CURLSHOPT_USERDATA, &m_brokerCookieShareLock);
    if (m_persistent) {
        CURL* load = curl_easy_init();
        if (load) {
            std::string cookie_path =
                m_path.AppendASCII("cookie.dat").AsUTF8Unsafe();
            curl_easy_setopt(load, CURLOPT_SHARE, share);
            curl_easy_setopt(load, CURLOPT_COOKIEFILE, cookie_path.c_str());
            curl_easy_setopt(load, CURLOPT_COOKIELIST, "RELOAD");
            curl_easy_setopt(load, CURLOPT_SHARE, nullptr);
            curl_easy_cleanup(load);
        }
    }
    m_brokerCookieShare = share;
    return share;
}

void ApiSession::init(v8::Isolate* isolate, v8::Local<v8::Object> target)
{
    const char* className = "Session";
    v8::Local<v8::FunctionTemplate> funTempl = v8::FunctionTemplate::New(isolate, newFunction);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();

    funTempl->SetClassName(v8::String::NewFromUtf8(isolate, className).ToLocalChecked());
    gin_helper::ObjectTemplateBuilder builder(isolate, funTempl->InstanceTemplate());
    //builder.SetMethod("_getWebContents", &ApiSession::_getWebContentsApi);
    builder.SetProperty("webRequest", &ApiSession::webRequestApi);
    builder.SetProperty("protocol", &ApiSession::protocolApi);
    builder.SetMethod("setDownloadPath", &ApiSession::setDownloadPathApi);
    builder.SetMethod("setPermissionRequestHandler", &ApiSession::setPermissionRequestHandlerApi);
    builder.SetMethod("setPermissionCheckHandler", &ApiSession::setPermissionCheckHandlerApi);
    builder.SetMethod("setDevicePermissionHandler", &ApiSession::setDevicePermissionHandlerApi);
    builder.SetMethod("getPreloads", &ApiSession::getPreloadsApi);
    builder.SetMethod("setPreloads", &ApiSession::setPreloadsApi);
    builder.SetMethod("getStoragePath", &ApiSession::getStoragePathApi);
    builder.SetMethod("clearStorageData", &ApiSession::clearStorageDataApi);
    builder.SetMethod("clearCache", &ApiSession::clearCacheApi);
    builder.SetMethod("clearAuthCache", &ApiSession::clearAuthCacheApi);
    builder.SetMethod("getPartition", &ApiSession::getPartition);

    v8::Local<v8::Function> fun = funTempl->GetFunction(context).ToLocalChecked();
    gin_helper::Dictionary sessionClass(isolate, fun);
    sessionClass.SetMethod("fromPartition", &ApiSession::fromPartitionApi);

    constructor.Reset(isolate, fun);
    target->Set(context, v8::String::NewFromUtf8(isolate, className).ToLocalChecked(), fun);
}


void ApiSession::newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.IsConstructCall()) {
        new ApiSession(isolate, args.This());
        args.GetReturnValue().Set(args.This());
        return;
    }
}

namespace {

std::basic_string<WCHAR> ToEngineWide(const std::string& value)
{
    static_assert(sizeof(WCHAR) == sizeof(char16_t));
    std::u16string converted = base::UTF8ToUTF16(value);
    return std::basic_string<WCHAR>(
        reinterpret_cast<const WCHAR*>(converted.data()), converted.size());
}

bool IsSafePartitionComponent(const std::string& name)
{
    if (name.empty() || name == "." || name == "..")
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

std::string StablePartitionDirectory(const std::string& name)
{
    if (IsSafePartitionComponent(name))
        return name;
    const std::string digest = base::SHA1HashString(name);
    static const char kHex[] = "0123456789abcdef";
    std::string result("partition-");
    result.reserve(10 + digest.size() * 2);
    for (unsigned char byte : digest) {
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0f]);
    }
    return result;
}

bool CopyMissingProfileFiles(const base::FilePath& source,
    const base::FilePath& destination)
{
    if (!base::DirectoryExists(source) || source == destination)
        return true;
    bool sawFile = false;
    base::FileEnumerator files(source, true, base::FileEnumerator::FILES);
    for (base::FilePath file = files.Next(); !file.empty(); file = files.Next()) {
        sawFile = true;
        base::FilePath relative;
        if (!source.AppendRelativePath(file, &relative))
            return false;
        base::FilePath target = destination.Append(relative);
        if (base::PathExists(target))
            continue;
        if (!base::CreateDirectory(target.DirName()) ||
            !base::CopyFile(file, target))
            return false;
    }
    // An empty legacy directory is not evidence of a migrated profile.
    return !sawFile || base::DirectoryExists(destination);
}

bool MigrateLegacyMiniElectronProfile(const std::string& partition,
    const base::FilePath& destination)
{
#if BUILDFLAG(IS_WIN)
    base::FilePath marker =
        destination.AppendASCII(".mini-electron-profile-migrated");
    if (base::PathExists(marker))
        return true;
    base::FilePath module;
    if (!base::PathService::Get(base::DIR_MODULE, &module))
        return false;
    std::string legacyName;
    if (partition.empty()) {
        legacyName = "default";
    } else {
        constexpr char kPersistPrefix[] = "persist:";
        legacyName = base::StartsWith(partition, kPersistPrefix,
                         base::CompareCase::SENSITIVE)
            ? partition.substr(sizeof(kPersistPrefix) - 1)
            : partition;
        unsigned int hash = StringUtil::hashString(legacyName.c_str());
        legacyName = base::StringPrintf("%x", hash);
    }
    std::vector<base::FilePath> candidates = {
        module.AppendASCII("profiles").AppendASCII("profiles")
            .AppendASCII(legacyName),
        module.AppendASCII("profiles").AppendASCII(legacyName),
        module.AppendASCII("minieleses").AppendASCII("profiles")
            .AppendASCII(legacyName),
        module.AppendASCII("minieleses").AppendASCII(legacyName)
    };
    bool hadLegacyFiles = false;
    for (const auto& candidate : candidates) {
        base::FileEnumerator probe(
            candidate, true, base::FileEnumerator::FILES);
        if (probe.Next().empty())
            continue;
        hadLegacyFiles = true;
        if (!CopyMissingProfileFiles(candidate, destination))
            return false;
    }
    if (hadLegacyFiles) {
        if (!base::CreateDirectory(destination) ||
            !base::WriteFile(marker, "copied-without-deleting-source\n"))
            return false;
    }
#endif
    return true;
}


} // namespace

base::FilePath SessionMgr::createSessionDirname(
    const std::string& partition, bool* persistent) const
{
    if (persistent)
        *persistent = true;
    if (partition.empty())
        return m_rootDir;

    constexpr char kPersistPrefix[] = "persist:";
    if (!base::StartsWith(partition, kPersistPrefix,
            base::CompareCase::SENSITIVE)) {
        if (persistent)
            *persistent = false;
        return m_rootDir.AppendASCII("Session Storage")
            .AppendASCII(StablePartitionDirectory(partition));
    }

    std::string name = partition.substr(sizeof(kPersistPrefix) - 1);
    if (name.empty())
        name = "empty";
    return m_rootDir.AppendASCII("Partitions")
        .AppendASCII(StablePartitionDirectory(name));
}

ApiSession* ApiSession::create(v8::Isolate* isolate,
    const std::string& partition, const base::FilePath& path, bool persistent)
{
    if (!isolate || path.empty())
        return nullptr;

    v8::Local<v8::Function> constructorFunction =
        v8::Local<v8::Function>::New(isolate, constructor);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::MaybeLocal<v8::Object> obj = constructorFunction->NewInstance(context);
    v8::Local<v8::Object> objV8;
    if (!obj.ToLocal(&objV8))
        return nullptr;

    ApiSession* self = static_cast<ApiSession*>(
        WrappableBase::GetNativePtr(objV8, &kWrapperInfo));
    self->m_liveSelf.Reset(isolate, objV8);
    self->m_partition = partition;
    self->m_path = path;
    self->m_persistent = persistent;
    self->m_removePathOnDestroy = !persistent;
    self->m_downloadPath = path.AsUTF8Unsafe();

    if (!base::CreateDirectory(path) || !base::DirectoryExists(path))
        return nullptr;
    return self;
}

void ApiSession::webRequestApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (!m_webRequest)
        m_webRequest = ApiWebRequest::create(args.GetIsolate());

    v8::Local<v8::Object> v = m_webRequest->GetWrapper(args.GetIsolate());
    args.GetReturnValue().Set(v);
}

void ApiSession::protocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (!m_protocol)
        m_protocol = ProtocolInterface::inst();

    v8::Local<v8::Object> v = m_protocol->getWrapper(args.GetIsolate());
    args.GetReturnValue().Set(v);
}

void ApiSession::setDownloadPathApi(const std::string& path)
{
    m_downloadPath = StringUtil::normalizePath(path); // 不包括文件名
}

void ApiSession::downloadURL(WebContents* contents, const std::string& url)
{
    GURL target(url);
    if (!target.is_valid() || !target.SchemeIsHTTPOrHTTPS()) {
        isolate()->ThrowException(v8::Exception::TypeError(
            gin_helper::StringToV8(isolate(), "downloadURL requires an HTTP(S) URL")));
        return;
    }
    struct Transfer {
        std::shared_ptr<std::atomic_bool> canceled =
            std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> paused =
            std::make_shared<std::atomic_bool>(false);
        base::ScopedTempDir staging;
        base::File file;
        base::FilePath destination;
        base::FilePath temporary;
        ApiDownloadItem* item = nullptr; // UI thread only; item pins its V8 wrapper.
        int item_id = 0;
        size_t received = 0; // Worker until its completion reply.
        size_t reported = 0;
    };
    auto transfer = std::make_shared<Transfer>();
    const int contents_id = contents->getIdApi();
    auto runner = base::SequencedTaskRunner::GetCurrentDefault();
    auto offer = [this, contents_id, url, transfer](
                     const base::Value::Dict& headers, bool select_path) {
        WebContents* owner = WebContents::fromId(contents_id);
        if (!owner || owner->isDestroyedApi())
            return false;
        if (!transfer->item) {
            ApiDownloadItem* item = ApiDownloadItem::create(isolate());
            if (!item)
                return false;
            transfer->item = item;
            transfer->item_id = item->m_id;
            item->m_url = url;
            item->m_canceled = transfer->canceled;
            item->m_paused = transfer->paused;
            if (const std::string* mime = FindHeader(headers, "content-type"))
                item->m_mime = *mime;
            if (const std::string* disposition = FindHeader(headers, "content-disposition"))
                item->m_disposition = *disposition;
            if (const std::string* length = FindHeader(headers, "content-length"))
                base::StringToSizeT(*length, &item->m_allSize);
            const bool prevented = mate::EventEmitter<ApiSession>::emit(
                "will-download", item, owner);
            if (prevented) {
                item->m_state = ApiDownloadItem::kCancelled;
                transfer->canceled->store(true, std::memory_order_release);
            }
            if (item->isCancelled()) {
                item->finishCancelledBeforeStart();
                return false;
            }
        }
        ApiDownloadItem* item = transfer->item;
        if (select_path && item->getSavePath().empty()) {
            owner->mate::EventEmitter<WebContents>::emit(
                "_download-save-dialog", item, m_downloadPath);
        }
        if (item->isCancelled())
            return false;
        transfer->destination =
            base::FilePath::FromUTF8Unsafe(item->getSavePath());
        return !select_path || !transfer->destination.empty();
    };
    BrokerNetworkRequest request;
    request.url = target.spec();
    request.method = "GET";
    request.mode = "navigate"; // Exact trusted main-process call, never a child field.
    request.credentials_mode = "include";
    request.resource_type = "other";
    request.canceled = transfer->canceled;
    request.cookie_share = static_cast<CURLSH*>(brokerCookieShare());
    request.headers_sink = [transfer, offer](int status,
                               const base::Value::Dict& headers) {
        bool accepted = false;
        content::ThreadCall::callUiThreadSync(FROM_HERE, [&] {
            accepted = offer(headers, status >= 200 && status < 300);
        });
        if (!accepted || status < 200 || status >= 300)
            return false;
        if (!transfer->staging.IsValid()) {
            if (!transfer->staging.CreateUniqueTempDirUnderPath(
                    transfer->destination.DirName()))
                return false;
            transfer->temporary = transfer->staging.GetPath().AppendASCII("download");
            transfer->file = base::File(transfer->temporary,
                base::File::FLAG_CREATE | base::File::FLAG_WRITE);
        }
        return transfer->file.IsValid();
    };
    request.body_sink = [transfer, runner](const char* bytes, size_t length) {
        while (transfer->paused->load(std::memory_order_acquire)
            && !transfer->canceled->load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (transfer->canceled->load(std::memory_order_acquire))
            return size_t(0);
        if (!transfer->file.WriteAtCurrentPosAndCheck(base::span(
                reinterpret_cast<const uint8_t*>(bytes), length)))
            return size_t(0);
        transfer->received += length;
        if (transfer->received - transfer->reported >= 256 * 1024) {
            transfer->reported = transfer->received;
            const size_t received = transfer->received;
            ApiDownloadItem* item = transfer->item;
            const int item_id = transfer->item_id;
            runner->PostTask(FROM_HERE, base::BindOnce(
                [](ApiDownloadItem* item, int id, size_t received) {
                    if (IdLiveDetect::get()->isLive(id))
                        item->updateProgress(received);
                }, item, item_id, received));
        }
        return length;
    };
    {
        base::AutoLock lock(m_brokerLock);
        auto& cancellations = m_brokerRequestCancellations[contents_id];
        cancellations.erase(std::remove_if(cancellations.begin(), cancellations.end(),
            [](const auto& pending) { return pending.expired(); }), cancellations.end());
        cancellations.push_back(request.canceled);
    }
    std::make_shared<BrokerRequestPipeline>(this, contents_id,
        std::move(request), [transfer, offer](base::Value::Dict result, std::string error) {
            transfer->file.Close();
            if (!transfer->item)
                offer({}, false);
            ApiDownloadItem* item = transfer->item;
            if (!item || !IdLiveDetect::get()->isLive(transfer->item_id)
                || item->m_done)
                return;
            item->updateProgress(transfer->received);
            bool completed = error.empty()
                && result.FindInt("status").value_or(0) >= 200
                && result.FindInt("status").value_or(0) < 300
                && !transfer->canceled->load(std::memory_order_acquire)
                && !transfer->temporary.empty()
                && base::ReplaceFile(transfer->temporary, transfer->destination, nullptr);
            item->finish(transfer->canceled->load(std::memory_order_acquire)
                    ? MINI_ELECTRON_LOADING_CANCELED
                    : completed ? MINI_ELECTRON_LOADING_SUCCEEDED : MINI_ELECTRON_LOADING_FAILED);
        })->Start();
}

class EngineWebRequestPipeline
    : public std::enable_shared_from_this<EngineWebRequestPipeline> {
public:
    EngineWebRequestPipeline(ApiSession* session, mini_electron_net_job job,
        std::string url, std::string method, std::string referrer,
        std::string resource_type,
        std::vector<std::pair<std::string, std::string>> headers)
        : session_(session)
        , job_(job)
        , url_(std::move(url))
        , method_(std::move(method))
        , referrer_(std::move(referrer))
        , resource_type_(std::move(resource_type))
        , headers_(std::move(headers))
    {
        static std::atomic<uint64_t> next_id { 1 };
        request_id_ = next_id.fetch_add(1, std::memory_order_relaxed);
    }

    void Start()
    {
        Invoke(&ApiWebRequest::m_beforeRequestCb, "onBeforeRequest",
            [self = shared_from_this()](base::Value::Dict response) {
                if (!self->ApplyResponse(response, false))
                    return;
                self->BeforeSendHeaders();
            });
    }

private:
    using ListenerMember = v8::Persistent<v8::Value> ApiWebRequest::*;
    struct CallbackState {
        std::function<void(base::Value::Dict)> callback;
        bool completed = false;
        v8::Global<v8::Function> function;
        scoped_refptr<base::SequencedTaskRunner> runner;
    };

    v8::Persistent<v8::Value>* Listener(
        ListenerMember member, const char* event_name)
    {
        return session_->m_webRequest
                && session_->m_webRequest->matchesListener(
                    event_name, url_, resource_type_)
            ? &(session_->m_webRequest->*member) : nullptr;
    }

    static void Complete(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        auto* state = static_cast<CallbackState*>(
            info.Data().As<v8::External>()->Value());
        if (state->completed)
            return;
        state->completed = true;
        base::Value::Dict response;
        if (info.Length() && info[0]->IsObject())
            gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &response);
        std::function<void(base::Value::Dict)> callback =
            std::move(state->callback);
        callback(std::move(response));
    }

    static void DeleteCallbackState(
        const v8::WeakCallbackInfo<CallbackState>& info)
    {
        CallbackState* state = info.GetParameter();
        if (!state->completed && state->callback) {
            std::function<void(base::Value::Dict)> callback =
                std::move(state->callback);
            state->runner->PostTask(FROM_HERE,
                base::BindOnce(
                    [](std::function<void(base::Value::Dict)> continuation) {
                        base::Value::Dict response;
                        response.Set("cancel", true);
                        continuation(std::move(response));
                    },
                    std::move(callback)));
        }
        state->function.Reset();
        delete state;
    }

    void Invoke(ListenerMember member, const char* event_name,
        std::function<void(base::Value::Dict)> continuation)
    {
        v8::Persistent<v8::Value>* listener = Listener(member, event_name);
        if (!listener || listener->IsEmpty()) {
            continuation({});
            return;
        }
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);
        v8::Local<v8::Function> function =
            listener->Get(isolate).As<v8::Function>();
        v8::Local<v8::Context> context =
            function->GetCreationContextChecked();
        v8::Context::Scope context_scope(context);
        auto* state = new CallbackState {
            std::move(continuation), false, {},
            base::SequencedTaskRunner::GetCurrentDefault()
        };
        v8::Local<v8::Function> callback = v8::Function::New(context,
            &Complete, v8::External::New(isolate, state)).ToLocalChecked();
        state->function.Reset(isolate, callback);
        state->function.SetWeak(state, &DeleteCallbackState,
            v8::WeakCallbackType::kParameter);
        base::Value::Dict details = Details();
        v8::Local<v8::Value> arguments[] = {
            gin_helper::Converter<base::Value::Dict>::ToV8(
                isolate, details),
            callback
        };
        v8::MaybeLocal<v8::Value> called = function->Call(context,
            v8::Undefined(isolate), 2, arguments);
        if (called.IsEmpty() && !state->completed) {
            state->completed = true;
            state->callback = {};
            Cancel();
        }
    }

    void Notify(ListenerMember member, const char* event_name)
    {
        v8::Persistent<v8::Value>* listener = Listener(member, event_name);
        if (!listener || listener->IsEmpty())
            return;
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);
        v8::Local<v8::Function> function =
            listener->Get(isolate).As<v8::Function>();
        v8::Local<v8::Context> context =
            function->GetCreationContextChecked();
        v8::Context::Scope context_scope(context);
        base::Value::Dict details = Details();
        v8::Local<v8::Value> argument =
            gin_helper::Converter<base::Value::Dict>::ToV8(
                isolate, details);
        function->Call(context, v8::Undefined(isolate), 1, &argument);
    }

    base::Value::Dict Details() const
    {
        base::Value::Dict request_headers;
        for (const auto& header : headers_)
            request_headers.Set(header.first, header.second);
        base::Value::Dict details;
        details.Set("id", static_cast<double>(request_id_));
        details.Set("url", url_);
        details.Set("method", method_);
        details.Set("referrer", referrer_);
        details.Set("resourceType", resource_type_);
        details.Set("requestHeaders", std::move(request_headers));
        return details;
    }

    static std::string HeaderValue(const base::Value& value)
    {
        if (const std::string* text = value.GetIfString())
            return *text;
        const base::Value::List* values = value.GetIfList();
        if (!values)
            return {};
        std::string combined;
        for (const base::Value& entry : *values) {
            if (const std::string* text = entry.GetIfString()) {
                if (!combined.empty())
                    combined += "\n";
                combined += *text;
            }
        }
        return combined;
    }

    bool ApplyResponse(const base::Value::Dict& response, bool headers_stage)
    {
        if (response.FindBool("cancel").value_or(false)) {
            Cancel();
            return false;
        }
        if (const std::string* redirect = response.FindString("redirectURL");
            redirect && GURL(*redirect).is_valid()) {
            url_ = *redirect;
        }
        if (headers_stage) {
            if (const base::Value::Dict* headers =
                    response.FindDict("requestHeaders")) {
                headers_.clear();
                for (const auto& item : *headers) {
                    std::string value = HeaderValue(item.second);
                    if (!value.empty())
                        headers_.emplace_back(item.first, std::move(value));
                }
            }
        }
        return true;
    }

    void BeforeSendHeaders()
    {
        Invoke(&ApiWebRequest::m_beforeSendHeadersCb,
            "onBeforeSendHeaders",
            [self = shared_from_this()](base::Value::Dict response) {
                if (!self->ApplyResponse(response, true))
                    return;
                self->Notify(&ApiWebRequest::m_sendHeadersCb,
                    "onSendHeaders");
                self->Commit();
            });
    }

    void Cancel()
    {
        if (completed_)
            return;
        completed_ = true;
        mini_electron_net_cancel_request(job_);
    }

    void Commit()
    {
        if (completed_)
            return;
        completed_ = true;
        mini_electron_net_change_request_url(job_, url_.c_str());
        for (const auto& header : headers_) {
            mini_electron_net_set_http_header_field(job_,
                ToEngineWide(header.first).c_str(),
                ToEngineWide(header.second).c_str(), FALSE);
        }
        mini_electron_net_continue_job(job_);
    }

    ApiSession* session_;
    mini_electron_net_job job_;
    uint64_t request_id_ = 0;
    std::string url_;
    std::string method_;
    std::string referrer_;
    std::string resource_type_;
    std::vector<std::pair<std::string, std::string>> headers_;
    bool completed_ = false;
};

void ApiSession::onLoadUrlBeginInBlinkThread(
    mini_electron_web_view, const char* url, mini_electron_net_job job)
{
    if (!m_webRequest)
        return;
    std::vector<std::pair<std::string, std::string>> headers;
    for (const mini_electron_slist* item =
            mini_electron_net_get_raw_http_head_in_blink_thread(job);
         item; item = item->next) {
        std::string line = item->data ? item->data : "";
        size_t separator = line.find(':');
        if (separator == std::string::npos)
            continue;
        std::string name = TrimHeader(line.substr(0, separator));
        std::string value = TrimHeader(line.substr(separator + 1));
        if (!name.empty())
            headers.emplace_back(std::move(name), std::move(value));
    }
    const char* raw_method =
        mini_electron_net_get_request_method_string(job);
    const char* raw_referrer = mini_electron_net_get_referrer(job);
    mini_electron_resource_type resource_type =
        mini_electron_net_get_resource_type(job);
    auto pipeline = std::make_shared<EngineWebRequestPipeline>(
        this, job, url ? url : "", raw_method ? raw_method : "GET",
        raw_referrer ? raw_referrer : "",
        BrokerResourceTypeName(static_cast<int>(resource_type)),
        std::move(headers));
    mini_electron_net_hold_job_to_asyn_commit(job);
    content::ThreadCall::callUiThreadAsync(FROM_HERE,
        [pipeline = std::move(pipeline)] { pipeline->Start(); });
}

void ApiSession::fromPartitionApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    std::string partition;
    if (!gin_helper::ConvertFromV8(args.GetIsolate(), args[0], &partition))
        return;
    ApiSession* session = SessionMgr::get()->findOrCreateSession(
        args.GetIsolate(), partition, true);
    if (session)
        args.GetReturnValue().Set(session->GetWrapper(args.GetIsolate()));
}

void ApiSession::getStoragePathApi(
    const v8::FunctionCallbackInfo<v8::Value>& args) const
{
    if (!m_persistent) {
        args.GetReturnValue().Set(v8::Null(args.GetIsolate()));
        return;
    }
    args.GetReturnValue().Set(
        gin_helper::StringToV8(args.GetIsolate(), m_path.AsUTF8Unsafe()));
}

void ApiSession::setPermissionRequestHandlerApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    m_permissionRequestHandler.Reset();
    if (args.Length() && args[0]->IsFunction())
        m_permissionRequestHandler.Reset(args.GetIsolate(), args[0].As<v8::Function>());
}

void ApiSession::setPermissionCheckHandlerApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    m_permissionCheckHandler.Reset();
    if (args.Length() && args[0]->IsFunction())
        m_permissionCheckHandler.Reset(args.GetIsolate(), args[0].As<v8::Function>());
}

void ApiSession::setDevicePermissionHandlerApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    m_devicePermissionHandler.Reset();
    if (args.Length() && args[0]->IsFunction())
        m_devicePermissionHandler.Reset(args.GetIsolate(), args[0].As<v8::Function>());
}

std::vector<std::string> ApiSession::getPreloadsApi()
{
    return m_preloadPaths;
}

void ApiSession::setPreloadsApi(const std::vector<std::string>& paths)
{
    m_preloadPaths = paths;
}

bool ApiSession::clearDirectories(
    const std::vector<const char*>& names, std::string* error)
{
    for (const char* name : names) {
        base::FilePath target = m_path.AppendASCII(name);
        if (!base::PathExists(target))
            continue;
        if (!base::DeletePathRecursively(target) && base::PathExists(target)) {
            *error = "Unable to clear " + target.AsUTF8Unsafe();
            return false;
        }
    }
    return true;
}

bool ApiSession::clearStorage(
    const std::vector<std::string>& storages, std::string* error)
{
    const auto requested = [&storages](const char* value) {
        return storages.empty() ||
            std::find(storages.begin(), storages.end(), value) != storages.end();
    };

    if (requested("cookies")) {
        base::FilePath cookiePath = m_path.AppendASCII("cookie.dat");
        if (m_brokerCookieShare) {
            CURL* clear = curl_easy_init();
            if (!clear) {
                *error = "Unable to initialize cookie clearing";
                return false;
            }
            curl_easy_setopt(clear, CURLOPT_SHARE,
                static_cast<CURLSH*>(m_brokerCookieShare));
            CURLcode clear_result =
                curl_easy_setopt(clear, CURLOPT_COOKIELIST, "ALL");
            curl_easy_setopt(clear, CURLOPT_SHARE, nullptr);
            curl_easy_cleanup(clear);
            if (clear_result != CURLE_OK) {
                *error = "Unable to clear active cookies";
                return false;
            }
        }
        std::vector<base::FilePath> cookieFiles = {
            cookiePath,
            m_path.AppendASCII("Cookies"),
            m_path.AppendASCII("Cookies-journal"),
            m_path.AppendASCII("Cookies-wal"),
            m_path.AppendASCII("Cookies-shm"),
            m_path.AppendASCII("Network").AppendASCII("Cookies"),
            m_path.AppendASCII("Network").AppendASCII("Cookies-journal"),
            m_path.AppendASCII("Network").AppendASCII("Cookies-wal"),
            m_path.AppendASCII("Network").AppendASCII("Cookies-shm")
        };
        for (const auto& file : cookieFiles) {
            if (base::PathExists(file) && !base::DeleteFile(file)) {
                *error = "Unable to clear cookie persistence";
                return false;
            }
        }
    }

    if (requested("localstorage")) {
        base::FileEnumerator files(m_path, false, base::FileEnumerator::FILES,
            FILE_PATH_LITERAL("*.localsto"));
        for (base::FilePath file = files.Next(); !file.empty(); file = files.Next()) {
            if (!base::DeleteFile(file)) {
                *error = "Unable to clear local storage";
                return false;
            }
        }
        if (!clearDirectories({ "Local Storage" }, error))
            return false;
        base::AutoLock lock(m_brokerLock);
        m_brokerLocalStorage.clear();
        m_brokerSessionStorage.clear();
        m_brokerStorageLoaded = true;
        if (m_persistent && !saveBrokerStorage(error))
            return false;
    }
    if (requested("indexdb") && !clearDirectories({ "IndexedDB" }, error))
        return false;
    if (requested("filesystem") && !clearDirectories({ "File System" }, error))
        return false;
    if (requested("serviceworkers") &&
        !clearDirectories({ "Service Worker" }, error))
        return false;
    if (requested("cachestorage")) {
        if (!clearDirectories({ "CacheStorage" }, error))
            return false;
        base::FilePath serviceWorkerCache =
            m_path.AppendASCII("Service Worker").AppendASCII("CacheStorage");
        if (base::PathExists(serviceWorkerCache) &&
            !base::DeletePathRecursively(serviceWorkerCache)) {
            *error = "Unable to clear service worker cache storage";
            return false;
        }
    }
    if (requested("websql") && !clearDirectories({ "databases", "WebSQL" }, error))
        return false;
    return true;
}

void ApiSession::clearStorageDataApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    gin_helper::Promise<void> promise(args.GetIsolate());
    v8::Local<v8::Promise> handle = promise.GetHandle();
    std::vector<std::string> storages;
    if (args.Length() && args[0]->IsObject()) {
        gin_helper::Dictionary options(args.GetIsolate(), args[0].As<v8::Object>());
        options.Get("storages", &storages);
    }
    std::string error;
    if (clearStorage(storages, &error))
        promise.Resolve();
    else
        promise.RejectWithErrorMessage(error);
    args.GetReturnValue().Set(handle);
}

void ApiSession::clearCacheApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    gin_helper::Promise<void> promise(args.GetIsolate());
    v8::Local<v8::Promise> handle = promise.GetHandle();
    std::string error;
    if (clearDirectories({ "Cache", "Code Cache", "GPUCache" }, &error))
        promise.Resolve();
    else
        promise.RejectWithErrorMessage(error);
    args.GetReturnValue().Set(handle);
}

void ApiSession::clearAuthCacheApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    gin_helper::Promise<void> promise(args.GetIsolate());
    v8::Local<v8::Promise> handle = promise.GetHandle();
    std::string error;
    if (clearDirectories({ "Auth Cache" }, &error))
        promise.Resolve();
    else
        promise.RejectWithErrorMessage(error);
    args.GetReturnValue().Set(handle);
}

bool ApiSession::loadBrokerStorage(std::string* error)
{
    if (m_brokerStorageLoaded)
        return true;
    m_brokerStorageLoaded = true;
    if (!m_persistent)
        return true;
    base::FilePath file = m_path.AppendASCII("BrokerStorage.json");
    if (!base::PathExists(file))
        return true;
    std::string json;
    if (!base::ReadFileToString(file, &json)) {
        *error = "Unable to read broker storage";
        return false;
    }
    auto parsed = base::JSONReader::Read(json);
    if (!parsed || !parsed->is_dict()) {
        *error = "Broker storage is corrupt";
        return false;
    }
    m_brokerLocalStorage = std::move(parsed->GetDict());
    return true;
}

bool ApiSession::saveBrokerStorage(std::string* error)
{
    if (!m_persistent)
        return true;
    std::string json;
    if (!base::JSONWriter::Write(m_brokerLocalStorage, &json)) {
        *error = "Unable to serialize broker storage";
        return false;
    }
    base::FilePath file = m_path.AppendASCII("BrokerStorage.json");
    base::FilePath temporary = m_path.AppendASCII("BrokerStorage.json.tmp");
    if (!base::WriteFile(temporary, json)) {
        *error = "Unable to write broker storage";
        return false;
    }
    bool committed = base::PathExists(file)
        ? base::ReplaceFile(temporary, file, nullptr)
        : base::Move(temporary, file);
    if (!committed) {
        base::DeleteFile(temporary);
        *error = "Unable to commit broker storage";
    }
    return committed;
}

void ApiSession::handleStorageBroker(int contentsId,
    const BrokerFrameContext& frameContext,
    const base::Value::Dict& payload, BrokerReply reply)
{
    const std::string* operation = payload.FindString("operation");
    const std::string* storage_type = payload.FindString("storageType");
    if (!operation || !storage_type
        || (*storage_type != "local" && *storage_type != "session")) {
        reply({}, "Invalid storage broker request");
        return;
    }
    if (*operation == "clone-session") {
        const std::string* source = payload.FindString("namespaceId");
        const std::string* target = payload.FindString("targetNamespaceId");
        if (*storage_type != "session" || !source || source->empty()
            || !target || target->empty() || !frameContext.frame_id) {
            reply({}, "Invalid session storage clone");
            return;
        }
        const std::string source_key =
            base::NumberToString(contentsId) + ":" + *source;
        const std::string target_key =
            base::NumberToString(contentsId) + ":" + *target;
        base::AutoLock lock(m_brokerLock);
        auto found = m_brokerSessionStorage.find(source_key);
        m_brokerSessionStorage[target_key] =
            found == m_brokerSessionStorage.end()
            ? base::Value::Dict() : found->second.Clone();
        auto& namespaces = m_sessionStorageNamespaces[contentsId];
        if (std::find(namespaces.begin(), namespaces.end(), target_key)
            == namespaces.end()) {
            namespaces.push_back(target_key);
        }
        reply({}, {});
        return;
    }

    const std::string* claimed_origin = payload.FindString("origin");
    const std::string* namespace_id = payload.FindString("namespaceId");
    GURL origin_url(frameContext.committed_origin);
    if (!frameContext.frame_id || !origin_url.is_valid()
        || !origin_url.has_scheme() || frameContext.committed_origin.empty()
        || (claimed_origin
            && *claimed_origin != frameContext.committed_origin)) {
        reply({}, "Storage request has no authorized frame origin");
        return;
    }
    const std::string& origin = frameContext.committed_origin;

    base::Value::Dict result;
    std::string error;
    {
        base::AutoLock lock(m_brokerLock);
        base::Value::Dict* origins = nullptr;
        if (*storage_type == "local") {
            if (!loadBrokerStorage(&error)) {
                reply({}, error);
                return;
            }
            origins = &m_brokerLocalStorage;
        } else {
            const std::string namespace_key =
                base::NumberToString(contentsId) + ":" + *namespace_id;
            origins = &m_brokerSessionStorage[namespace_key];
            auto& namespaces = m_sessionStorageNamespaces[contentsId];
            if (std::find(namespaces.begin(), namespaces.end(), namespace_key)
                == namespaces.end()) {
                namespaces.push_back(namespace_key);
            }
        }
        base::Value::Dict* values = origins->FindDict(origin);
        if (!values) {
            origins->Set(origin, base::Value::Dict());
            values = origins->FindDict(origin);
        }

        const std::string* key = payload.FindString("key");
        if (*operation == "get") {
            if (!key) {
                error = "Storage get requires a key";
            } else if (const std::string* value = values->FindString(*key)) {
                result.Set("found", true);
                result.Set("value", *value);
            } else {
                result.Set("found", false);
            }
        } else if (*operation == "set") {
            const std::string* value = payload.FindString("value");
            if (!key || !value) {
                error = "Storage set requires string key and value";
            } else {
                size_t usage = 0;
                for (const auto& item : *values) {
                    const std::string* existing = item.second.GetIfString();
                    if (existing)
                        usage += item.first.size() + existing->size();
                }
                if (const std::string* previous = values->FindString(*key))
                    usage -= key->size() + previous->size();
                if (key->size() + value->size()
                    > mini_electron::kPerStorageAreaQuota
                        - std::min(usage,
                            mini_electron::kPerStorageAreaQuota)) {
                    error = "Storage quota exceeded";
                } else {
                    values->Set(*key, *value);
                }
            }
        } else if (*operation == "remove") {
            if (!key)
                error = "Storage remove requires a key";
            else
                values->Remove(*key);
        } else if (*operation == "clear") {
            values->clear();
        } else if (*operation == "keys") {
            base::Value::List keys;
            for (const auto& item : *values)
                keys.Append(item.first);
            result.Set("keys", std::move(keys));
        } else {
            error = "Unsupported storage operation";
        }
        if (error.empty() && *storage_type == "local"
            && *operation != "get" && *operation != "keys") {
            saveBrokerStorage(&error);
        }
    }
    reply(std::move(result), std::move(error));
}

bool ApiSession::authorizeUploadPaths(int contents_id,
    const std::vector<std::string>& paths, std::string* error)
{
    std::vector<std::string> canonical_paths;
    canonical_paths.reserve(paths.size());
    for (const std::string& raw_path : paths) {
        base::FilePath path = base::FilePath::FromUTF8Unsafe(raw_path);
        base::FilePath absolute = base::MakeAbsoluteFilePath(path);
        base::FilePath canonical;
        if (absolute.empty()
            || !base::NormalizeFilePath(absolute, &canonical)) {
            *error = "Upload file is unreadable or exceeds platform limits";
            return false;
        }
        if (canonical.empty() || base::DirectoryExists(canonical)
            || !base::PathIsReadable(canonical)) {
            *error = "Upload file is unreadable or exceeds platform limits";
            return false;
        }
        std::optional<int64_t> size = base::GetFileSize(canonical);
        if (!size || *size < 0
            || *size > std::numeric_limits<int>::max()) {
            *error = "Upload file is unreadable or exceeds platform limits";
            return false;
        }
        canonical_paths.push_back(canonical.AsUTF8Unsafe());
    }
    base::AutoLock lock(m_brokerLock);
    for (const std::string& path : canonical_paths)
        ++m_pendingUploadGrants[{ contents_id, path }];
    return true;
}

std::string ApiSession::authorizeFile(
    int contentsId, const base::FilePath& path)
{
    constexpr int64_t kMaximumAuthorizedFileSize =
        std::numeric_limits<int>::max();
    base::FilePath absolute = base::MakeAbsoluteFilePath(path);
    base::FilePath canonical;
    if (contentsId <= 0 || absolute.empty()
        || !base::NormalizeFilePath(absolute, &canonical)
        || !base::PathExists(canonical)
        || !base::PathIsReadable(canonical)
        || base::DirectoryExists(canonical)) {
        return std::string();
    }
    std::optional<int64_t> size = base::GetFileSize(canonical);
    if (!size || *size < 0 || *size > kMaximumAuthorizedFileSize)
        return std::string();
    base::AutoLock lock(m_brokerLock);
    std::string token;
    do {
        token = base::HexEncode(base::RandBytesAsVector(16));
    } while (m_authorizedFiles.contains(token));
    m_authorizedFiles[token] = { canonical, *size, contentsId };
    return token;
}

base::Value::List ApiSession::authorizeSelectedFiles(int contentsId,
    const std::vector<std::string>& paths, std::string* error)
{
    constexpr size_t kMaximumSelectedFiles = 10000;
    constexpr int64_t kMaximumTotalBytes = 4LL * 1024 * 1024 * 1024;
    struct Selection {
        base::FilePath path;
        int64_t size;
        base::Time modified;
        std::string relative_path;
    };
    std::vector<Selection> selections;
    int64_t total_bytes = 0;
    const auto add_file = [&](const base::FilePath& input,
                              std::string relative_path) {
        base::FilePath absolute = base::MakeAbsoluteFilePath(input);
        base::FilePath canonical;
        base::File::Info info;
        if (absolute.empty() || base::IsLink(absolute)
            || !base::NormalizeFilePath(absolute, &canonical)
            || canonical != absolute || !base::PathIsReadable(canonical)
            || !base::GetFileInfo(canonical, &info) || info.is_directory
            || info.is_symbolic_link || info.size < 0
            || info.size > std::numeric_limits<int>::max()) {
            return false;
        }
        if (selections.size() == kMaximumSelectedFiles
            || info.size > kMaximumTotalBytes - total_bytes) {
            *error = "Selected folder exceeds file count or size limits";
            return false;
        }
        total_bytes += info.size;
        selections.push_back({ std::move(canonical), info.size,
            info.last_modified, std::move(relative_path) });
        return true;
    };

    for (const std::string& raw_path : paths) {
        base::FilePath absolute = base::MakeAbsoluteFilePath(
            base::FilePath::FromUTF8Unsafe(raw_path));
        base::FilePath canonical;
        base::File::Info info;
        if (absolute.empty() || base::IsLink(absolute)
            || !base::NormalizeFilePath(absolute, &canonical)
            || canonical != absolute
            || !base::GetFileInfo(canonical, &info)) {
            *error = "Selected path is unreadable or is a symbolic link";
            return {};
        }
        if (!info.is_directory) {
            if (!add_file(canonical, {})) {
                if (error->empty())
                    *error = "Selected file is unreadable or exceeds platform limits";
                return {};
            }
            continue;
        }
        base::FileEnumerator files(
            canonical, true, base::FileEnumerator::FILES);
        for (base::FilePath child = files.Next(); !child.empty();
             child = files.Next()) {
            if (base::IsLink(child))
                continue;
            base::FilePath relative;
            if (!canonical.AppendRelativePath(child, &relative)) {
                *error = "Selected folder traversal is invalid";
                return {};
            }
            std::string relative_path =
                canonical.BaseName().Append(relative).AsUTF8Unsafe();
            std::replace(relative_path.begin(), relative_path.end(),
                '\\', '/');
            if (!add_file(child, std::move(relative_path))) {
                if (error->empty())
                    continue;
                return {};
            }
        }
    }
    base::Value::List files;
    for (const Selection& selection : selections) {
        std::string token = authorizeFile(contentsId, selection.path);
        if (token.empty()) {
            for (const base::Value& file : files) {
                if (const std::string* granted =
                        file.GetDict().FindString("token")) {
                    revokeFile(*granted);
                }
            }
            *error = "Selected file changed before authorization";
            return {};
        }
        base::Value::Dict file;
        file.Set("name", selection.path.BaseName().AsUTF8Unsafe());
        file.Set("path", selection.path.AsUTF8Unsafe());
        file.Set("token", std::move(token));
        file.Set("size", static_cast<int>(selection.size));
        file.Set("lastModified",
            selection.modified.InMillisecondsFSinceUnixEpochIgnoringNull());
        if (!selection.relative_path.empty())
            file.Set("relativePath", selection.relative_path);
        files.Append(std::move(file));
    }
    return files;
}

void ApiSession::revokeFile(const std::string& token)
{
    base::AutoLock lock(m_brokerLock);
    m_authorizedFiles.erase(token);
}

void ApiSession::handleFileBroker(int contentsId,
    const base::Value::Dict& payload, BrokerReply reply)
{
    const std::string* operation = payload.FindString("operation");
    if (!operation) {
        reply({}, "Invalid file broker request");
        return;
    }
    if (*operation == "authorize-upload") {
        const std::string* requested_path =
            payload.FindString("requestedPath");
        base::FilePath canonical;
        if (requested_path) {
            base::FilePath absolute = base::MakeAbsoluteFilePath(
                base::FilePath::FromUTF8Unsafe(*requested_path));
            base::NormalizeFilePath(absolute, &canonical);
        }
        if (!requested_path || canonical.empty()) {
            reply({}, "Upload path is not authorized");
            return;
        }
        {
            base::AutoLock lock(m_brokerLock);
            auto key = std::make_pair(contentsId,
                canonical.AsUTF8Unsafe());
            auto grant = m_pendingUploadGrants.find(key);
            if (grant == m_pendingUploadGrants.end() || !grant->second) {
                reply({}, "Upload path is not authorized by the browser");
                return;
            }
            if (!--grant->second)
                m_pendingUploadGrants.erase(grant);
        }
        std::string token = authorizeFile(contentsId, canonical);
        std::optional<int64_t> size;
        if (!token.empty())
            size = base::GetFileSize(canonical);
        if (!size || *size > std::numeric_limits<int>::max()) {
            if (!token.empty())
                revokeFile(token);
            reply({}, "Authorized upload changed before use");
            return;
        }
        base::Value::Dict result;
        result.Set("token", token);
        result.Set("size", static_cast<int>(*size));
        result.Set("displayName", canonical.BaseName().AsUTF8Unsafe());
        result.Set("path", canonical.AsUTF8Unsafe());
        reply(std::move(result), {});
        return;
    }
    const std::string* token = payload.FindString("token");
    std::optional<int> offsetValue = payload.FindInt("offset");
    std::optional<int> lengthValue = payload.FindInt("length");
    if (*operation != "read" || !token || !offsetValue || !lengthValue
        || *offsetValue < 0 || *lengthValue < 0
        || *lengthValue > 16 * 1024 * 1024) {
        reply({}, "Invalid file broker request");
        return;
    }
    base::FilePath path;
    int64_t authorizedSize = 0;
    {
        base::AutoLock lock(m_brokerLock);
        auto found = m_authorizedFiles.find(*token);
        if (found == m_authorizedFiles.end()
            || found->second.contents_id != contentsId) {
            reply({}, "File token is not authorized");
            return;
        }
        path = found->second.path;
        authorizedSize = found->second.size;
    }
    std::optional<int64_t> currentSize = base::GetFileSize(path);
    int64_t requestedEnd = static_cast<int64_t>(*offsetValue) + *lengthValue;
    if (!currentSize || *currentSize != authorizedSize ||
        requestedEnd > authorizedSize) {
        reply({}, "Authorized file changed or read exceeds its grant");
        return;
    }
    base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
    if (!file.IsValid()) {
        reply({}, "Authorized file is no longer readable");
        return;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(*lengthValue));
    std::optional<size_t> read = file.Read(
        static_cast<int64_t>(*offsetValue), base::span(bytes));
    if (!read) {
        reply({}, "Unable to read authorized file");
        return;
    }
    bytes.resize(*read);
    base::Value::Dict result;
    result.Set("data", base::Value(std::move(bytes)));
    bool eof = static_cast<int64_t>(*offsetValue + *read)
        >= authorizedSize;
    result.Set("eof", eof);
    if (eof)
        revokeFile(*token);
    reply(std::move(result), {});
}

void ApiSession::handleNetworkBroker(int contentsId,
    const BrokerFrameContext& frameContext,
    const base::Value::Dict& payload, BrokerReply reply)
{
    EnsureBrokerCurlInitialized();
    const std::string* operation = payload.FindString("operation");
    if (!operation) {
        reply({}, "Network broker operation is missing");
        return;
    }
    if (*operation == "request") {
        const std::string* url = payload.FindString("url");
        const std::string* method = payload.FindString("method");
        const base::Value::Dict* headers = payload.FindDict("headers");
        std::optional<int> request_mode = payload.FindInt("requestMode");
        std::optional<int> credentials_mode =
            payload.FindInt("credentialsMode");
        const std::string* resource_type = payload.FindString("resourceType");
        if (!url || !method || method->empty() || method->size() > 32
            || !headers || !request_mode || *request_mode < 0
            || *request_mode > 4 || !credentials_mode
            || *credentials_mode < 0 || *credentials_mode > 2
            || !resource_type) {
            reply({}, "Invalid network broker request");
            return;
        }
        std::string normalized_method = base::ToUpperASCII(*method);
        if (normalized_method != *method
            || normalized_method == "CONNECT" || normalized_method == "TRACE"
            || normalized_method == "TRACK"
            || !IsHttpToken(normalized_method)) {
            reply({}, "Network method is not authorized");
            return;
        }
        const bool is_document =
            *resource_type == "mainFrame" || *resource_type == "subFrame";
        const bool approved_navigation =
            is_document && frameContext.browser_approved_navigation
            && *url == frameContext.approved_navigation_url
            && normalized_method == frameContext.approved_navigation_method;
        const bool trusted_browser_navigation =
            approved_navigation && *resource_type == "mainFrame"
            && frameContext.browser_initiated_navigation;
        if (*request_mode == 4 && !approved_navigation) {
            reply({}, "Navigation request is not browser-approved");
            return;
        }
        GURL committed_document(frameContext.committed_url);
        const bool authenticated_opaque_document =
            frameContext.frame_id && frameContext.committed_origin.empty()
            && committed_document.is_valid()
            && (committed_document.SchemeIs("data")
                || committed_document.SchemeIs("about"));
        if (!frameContext.frame_id
            || (!approved_navigation
                && frameContext.committed_origin.empty()
                && !authenticated_opaque_document)) {
            reply({}, "Network request has no authorized document");
            return;
        }
        BrokerNetworkRequest request;
        request.url = *url;
        request.method = normalized_method;
        static constexpr const char* kRequestModes[] = {
            "same-origin", "no-cors", "cors", "cors", "navigate"
        };
        static constexpr const char* kCredentialsModes[] = {
            "omit", "same-origin", "include"
        };
        request.mode = trusted_browser_navigation
            ? "navigate"
            : (approved_navigation
                    ? "cors" : kRequestModes[*request_mode]);
        request.force_preflight = !trusted_browser_navigation
            && *request_mode == 3;
        request.credentials_mode = trusted_browser_navigation
            ? "include" : kCredentialsModes[*credentials_mode];
        request.resource_type = *resource_type;
        request.initiator_origin = approved_navigation
            ? (frameContext.navigation_initiator_origin.empty()
                    ? "null"
                    : frameContext.navigation_initiator_origin)
            : (authenticated_opaque_document
                    ? "null" : frameContext.committed_origin);
        request.initiator_is_local_application =
            IsLocalApplicationOrigin(GURL(request.initiator_origin))
            || (approved_navigation
                && IsLocalApplicationOrigin(committed_document));
        GURL initiator_url(frameContext.committed_url);
        GURL target_url(request.url);
        if (initiator_url.SchemeIsHTTPOrHTTPS() && target_url.is_valid()
            && SameOrigin(initiator_url, target_url)) {
            request.referrer = initiator_url.spec();
        }
        for (const auto& item : *headers) {
            const std::string* value = item.second.GetIfString();
            if (!value || !IsHttpToken(item.first)
                || value->find_first_of("\r\n") != std::string::npos) {
                reply({}, "Invalid network request header");
                return;
            }
            if (IsForbiddenRequestHeader(item.first))
                continue;
            request.headers.emplace_back(item.first, *value);
        }
        if (const base::Value::BlobStorage* body = payload.FindBlob("body")) {
            if (body->size() > kMaximumBrokerBodySize) {
                reply({}, "Network request body exceeds broker limit");
                return;
            }
            request.body.assign(body->begin(), body->end());
        }
        request.excluded_file_roots = {
            SessionMgr::get()->getRootDir(), m_path
        };
        request.application_resource_roots =
            frameContext.application_resource_roots;
        request.web_security = frameContext.web_security;
        request.allow_running_insecure_content =
            frameContext.allow_running_insecure_content;
        request.canceled = std::make_shared<std::atomic_bool>(false);
        {
            base::AutoLock lock(m_brokerLock);
            auto& cancellations = m_brokerRequestCancellations[contentsId];
            cancellations.erase(std::remove_if(
                cancellations.begin(), cancellations.end(),
                [](const auto& pending) { return pending.expired(); }),
                cancellations.end());
            cancellations.push_back(request.canceled);
        }
        request.cookie_share = static_cast<CURLSH*>(brokerCookieShare());
        std::make_shared<BrokerRequestPipeline>(this, contentsId,
            std::move(request), std::move(reply))->Start();
        return;
    }

    std::optional<double> raw_socket_id = payload.FindDouble("socketId");
    if (!raw_socket_id || *raw_socket_id < 1
        || *raw_socket_id > 9007199254740991.0
        || *raw_socket_id != static_cast<uint64_t>(*raw_socket_id)) {
        reply({}, "Invalid WebSocket identifier");
        return;
    }
    uint64_t socket_id = static_cast<uint64_t>(*raw_socket_id);
    auto key = std::make_pair(contentsId, socket_id);
    if (*operation == "websocket-open") {
        const std::string* url = payload.FindString("url");
        GURL parsed(url ? *url : std::string());
        if (!url || !parsed.is_valid()
            || (!parsed.SchemeIs("ws") && !parsed.SchemeIs("wss"))) {
            reply({}, "Invalid WebSocket URL");
            return;
        }
        GURL committed_document(frameContext.committed_url);
        const bool authenticated_opaque_document =
            frameContext.frame_id && frameContext.committed_origin.empty()
            && committed_document.is_valid()
            && (committed_document.SchemeIs("data")
                || committed_document.SchemeIs("about"));
        GURL initiator(frameContext.committed_origin);
        const bool local_application_origin =
            IsLocalApplicationOrigin(committed_document)
            || IsLocalApplicationOrigin(initiator);
        if (!frameContext.frame_id
            || (frameContext.committed_origin.empty()
                && !authenticated_opaque_document)
            || (!authenticated_opaque_document && !initiator.is_valid())
            || (frameContext.web_security
                && IsPotentiallyPrivateHost(parsed)
                && !(local_application_origin
                    && IsPotentiallyPrivateHost(parsed))
                && !IsPotentiallyPrivateHost(initiator))) {
            reply({}, "WebSocket target is not authorized for this frame");
            return;
        }
        {
            base::AutoLock lock(m_brokerSocketRegistry->lock);
            if (m_brokerSocketRegistry->sockets.contains(key)) {
                reply({}, "WebSocket identifier is already active");
                return;
            }
        }
        const std::string* protocol = payload.FindString("protocol");
        if (protocol && protocol->find_first_of("\r\n")
                != std::string::npos) {
            reply({}, "Invalid WebSocket protocol");
            return;
        }
        const std::string websocket_origin = authenticated_opaque_document
            ? "null" : frameContext.committed_origin;
        auto socket = std::make_shared<BrokerWebSocket>(contentsId,
            socket_id, *url, protocol ? *protocol : std::string(),
            websocket_origin,
            frameContext.web_security
                && !(local_application_origin
                    && IsPotentiallyPrivateHost(parsed))
                && !IsPotentiallyPrivateHost(initiator),
            authenticated_opaque_document
                ? nullptr : static_cast<CURLSH*>(brokerCookieShare()),
            base::SequencedTaskRunner::GetCurrentDefault(),
            m_brokerSocketRegistry);
        {
            base::AutoLock lock(m_brokerSocketRegistry->lock);
            m_brokerSocketRegistry->sockets.emplace(key, socket);
        }
        socket->Start();
        reply({}, {});
        return;
    }

    std::shared_ptr<BrokerWebSocket> socket;
    {
        base::AutoLock lock(m_brokerSocketRegistry->lock);
        auto found = m_brokerSocketRegistry->sockets.find(key);
        if (found != m_brokerSocketRegistry->sockets.end())
            socket = found->second;
    }
    if (!socket) {
        reply({}, "WebSocket is not active");
        return;
    }
    if (*operation == "websocket-send") {
        bool binary = payload.FindBool("binary").value_or(false);
        std::vector<uint8_t> data;
        if (binary) {
            const base::Value::BlobStorage* value =
                payload.FindBlob("data");
            if (!value || value->size() > kMaximumBrokerBodySize) {
                reply({}, "Invalid WebSocket binary payload");
                return;
            }
            data.assign(value->begin(), value->end());
        } else {
            const std::string* value = payload.FindString("data");
            if (!value || value->size() > kMaximumBrokerBodySize) {
                reply({}, "Invalid WebSocket text payload");
                return;
            }
            data.assign(value->begin(), value->end());
        }
        socket->Queue(binary, std::move(data));
        reply({}, {});
    } else if (*operation == "websocket-close") {
        int code = payload.FindInt("code").value_or(1000);
        const std::string* reason = payload.FindString("reason");
        if (code < 1000 || code > 4999
            || (reason && reason->size() > 123)) {
            reply({}, "Invalid WebSocket close payload");
            return;
        }
        socket->Close(code, reason ? *reason : std::string());
        reply({}, {});
    } else {
        reply({}, "Unsupported network broker operation");
    }
}

void ApiSession::onWebContentsDestroyed(int contentsId)
{
    std::vector<std::shared_ptr<BrokerWebSocket>> sockets;
    if (m_brokerSocketRegistry) {
        base::AutoLock lock(m_brokerSocketRegistry->lock);
        for (auto it = m_brokerSocketRegistry->sockets.begin();
             it != m_brokerSocketRegistry->sockets.end();) {
            if (it->first.first == contentsId) {
                sockets.push_back(it->second);
                it = m_brokerSocketRegistry->sockets.erase(it);
            } else {
                ++it;
            }
        }
    }
    {
        base::AutoLock lock(m_brokerLock);
        auto cancellations = m_brokerRequestCancellations.find(contentsId);
        if (cancellations != m_brokerRequestCancellations.end()) {
            for (const auto& weak : cancellations->second) {
                if (auto canceled = weak.lock())
                    canceled->store(true, std::memory_order_release);
            }
            m_brokerRequestCancellations.erase(cancellations);
        }
        for (auto it = m_pendingUploadGrants.begin();
             it != m_pendingUploadGrants.end();) {
            if (it->first.first == contentsId)
                it = m_pendingUploadGrants.erase(it);
            else
                ++it;
        }
        for (auto it = m_authorizedFiles.begin();
             it != m_authorizedFiles.end();) {
            if (it->second.contents_id == contentsId)
                it = m_authorizedFiles.erase(it);
            else
                ++it;
        }
        auto namespaces = m_sessionStorageNamespaces.find(contentsId);
        if (namespaces != m_sessionStorageNamespaces.end()) {
            for (const std::string& name : namespaces->second)
                m_brokerSessionStorage.erase(name);
            m_sessionStorageNamespaces.erase(namespaces);
        }
    }
    for (const auto& socket : sockets)
        socket->Stop();
}

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
void ApiSession::handleDevToolsResource(
    const base::Value::Dict& payload, BrokerReply reply)
{
    const std::string* request_url = payload.FindString("url");
    const std::string* method = payload.FindString("method");
    if (!request_url || !method || *method != "GET") {
        reply({}, "Invalid DevTools resource request");
        return;
    }
    GURL url(*request_url);
    constexpr std::string_view kPrefix = "/bundled/";
    if (!url.is_valid() || !url.SchemeIs("devtools") ||
        url.host() != "devtools" ||
        !base::StartsWith(url.path_piece(), kPrefix) ||
        url.path_piece().find('%') != std::string_view::npos ||
        url.path_piece().find('\\') != std::string_view::npos) {
        reply({}, "Invalid DevTools resource URL");
        return;
    }

    std::string relative(url.path_piece().substr(kPrefix.size()));
    base::FilePath relative_path = base::FilePath::FromUTF8Unsafe(relative);
    if (relative_path.empty() || relative_path.IsAbsolute() ||
        relative_path.ReferencesParent()) {
        reply({}, "DevTools resource path escapes its bundle");
        return;
    }
    base::FilePath executable_dir;
    if (!base::PathService::Get(base::DIR_EXE, &executable_dir)) {
        reply({}, "DevTools resource directory is unavailable");
        return;
    }
    base::FilePath root;
#if BUILDFLAG(IS_MAC)
    base::FilePath bundle_root = executable_dir.DirName()
        .Append(FILE_PATH_LITERAL("Resources"))
        .Append(FILE_PATH_LITERAL("devtools-frontend"))
        .Append(FILE_PATH_LITERAL("front_end"));
    if (base::DirectoryExists(bundle_root))
        root = std::move(bundle_root);
#endif
    if (root.empty()) {
        root = executable_dir.Append(FILE_PATH_LITERAL("resources"))
            .Append(FILE_PATH_LITERAL("devtools-frontend"))
            .Append(FILE_PATH_LITERAL("front_end"));
    }
    base::FilePath canonical_root = base::MakeAbsoluteFilePath(root);
    base::FilePath resource =
        base::MakeAbsoluteFilePath(root.Append(relative_path));
    if (canonical_root.empty() || resource.empty()
        || !canonical_root.IsParent(resource)) {
        reply({}, "DevTools resource path escapes its bundle");
        return;
    }
    constexpr int64_t kMaximumResourceSize = 64 * 1024 * 1024;
    if (!base::PathExists(resource) || base::DirectoryExists(resource)) {
        reply({}, "DevTools resource was not staged");
        return;
    }
    std::optional<int64_t> size = base::GetFileSize(resource);
    if (!size || *size < 0 || *size > kMaximumResourceSize) {
        reply({}, "DevTools resource was not staged");
        return;
    }
    std::string contents;
    if (!base::ReadFileToString(resource, &contents)) {
        reply({}, "DevTools resource could not be read");
        return;
    }

    std::string mime = BrokerResourceMimeType(resource);

    base::Value::BlobStorage body(contents.begin(), contents.end());
    base::Value::Dict result;
    result.Set("body", base::Value(std::move(body)));
    result.Set("statusCode", 200);
    result.Set("statusText", "OK");
    result.Set("mimeType", mime);
    reply(std::move(result), {});
}
#endif


std::vector<RendererPrivilegedScheme> ApiSession::getPrivilegedSchemes() const
{
    ProtocolInterface* protocol = ProtocolInterface::inst();
    std::vector<RendererPrivilegedScheme> schemes =
        protocol ? protocol->getPrivilegedSchemes()
                 : std::vector<RendererPrivilegedScheme>();
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    schemes.push_back(
        {"devtools", true, true, true, true, false});
#endif
    return schemes;
}

void ApiSession::handleBrokerRequest(int contentsId,
    const BrokerFrameContext& frameContext,
    const base::Value::Dict& payload, BrokerReply reply)
{
    const std::string* kind = payload.FindString("kind");
    if (!kind) {
        reply({}, "Broker request has no kind");
    } else if (*kind == "storage") {
        handleStorageBroker(
            contentsId, frameContext, payload, std::move(reply));
    } else if (*kind == "file") {
        if (!frameContext.frame_id) {
            reply({}, "File request has no authorized frame");
            return;
        }
        handleFileBroker(contentsId, payload, std::move(reply));
    } else if (*kind == "network") {
        if (!m_protocol)
            m_protocol = ProtocolInterface::inst();
        const std::string* request_url = payload.FindString("url");
        GURL url(request_url ? *request_url : std::string());
        if (m_protocol && m_protocol->isProtocolHandled(url.scheme())) {
            m_protocol->handleBrokerRequest(contentsId,
                frameContext.frame_id, payload, std::move(reply));
        } else {
            handleNetworkBroker(
                contentsId, frameContext, payload, std::move(reply));
        }
    } else if (*kind == "protocol") {
        const std::string* request_url = payload.FindString("url");
        const std::string* method = payload.FindString("method");
        GURL url(request_url ? *request_url : std::string());
        const bool approved_navigation = request_url && method
            && frameContext.browser_approved_navigation
            && *request_url == frameContext.approved_navigation_url
            && *method == frameContext.approved_navigation_method;
        if (!approved_navigation
            && (!frameContext.frame_id
                || frameContext.committed_origin.empty())) {
            reply({}, "Protocol request has no authorized document");
            return;
        }
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
        if (url.SchemeIs("devtools")) {
            handleDevToolsResource(payload, std::move(reply));
            return;
        }
#endif
        if (!m_protocol)
            m_protocol = ProtocolInterface::inst();
        if (!m_protocol) {
            reply({}, "Protocol service is not initialized");
            return;
        }
        m_protocol->handleBrokerRequest(
            contentsId, frameContext.frame_id, payload,
            std::move(reply));
    } else {
        reply({}, "Broker request kind is not authorized");
    }
}

bool ApiSession::checkPermission(int contentsId,
    const std::string& permission, const base::Value::Dict& details)
{
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (!isolate || m_permissionCheckHandler.IsEmpty())
        return false;
    v8::HandleScope scope(isolate);
    v8::Local<v8::Function> handler = m_permissionCheckHandler.Get(isolate);
    v8::Local<v8::Context> context = handler->GetCreationContextChecked();
    v8::Context::Scope contextScope(context);
    v8::Local<v8::Value> argv[] = {
        v8::Integer::New(isolate, contentsId),
        gin_helper::ConvertToV8(isolate, permission),
        gin_helper::ConvertToV8(isolate, details)
    };
    v8::Local<v8::Value> result;
    return handler->Call(context, v8::Undefined(isolate), 3, argv)
        .ToLocal(&result) && result->BooleanValue(isolate);
}

bool ApiSession::checkDevicePermission(
    int contentsId, const base::Value::Dict& details)
{
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (!isolate || m_devicePermissionHandler.IsEmpty())
        return false;
    v8::HandleScope scope(isolate);
    v8::Local<v8::Function> handler = m_devicePermissionHandler.Get(isolate);
    v8::Local<v8::Context> context = handler->GetCreationContextChecked();
    v8::Context::Scope contextScope(context);
    base::Value::Dict input = details.Clone();
    input.Set("contentsId", contentsId);
    v8::Local<v8::Value> argument = gin_helper::ConvertToV8(isolate, input);
    v8::Local<v8::Value> result;
    return handler->Call(context, v8::Undefined(isolate), 1, &argument)
        .ToLocal(&result) && result->BooleanValue(isolate);
}

void ApiSession::requestPermission(int contentsId,
    const std::string& permission, const base::Value::Dict& details,
    std::function<void(bool)> callback)
{
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (!isolate || m_permissionRequestHandler.IsEmpty()) {
        callback(false);
        return;
    }
    struct PermissionReply {
        std::function<void(bool)> callback;
        bool completed = false;
        v8::Global<v8::Function> function;
        scoped_refptr<base::SequencedTaskRunner> runner;
    };
    auto* pending = new PermissionReply {
        std::move(callback), false, {},
        base::SequencedTaskRunner::GetCurrentDefault()
    };
    v8::HandleScope scope(isolate);
    v8::Local<v8::Function> handler = m_permissionRequestHandler.Get(isolate);
    v8::Local<v8::Context> context = handler->GetCreationContextChecked();
    v8::Context::Scope contextScope(context);
    v8::Local<v8::Function> respond = v8::Function::New(context,
        [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            auto* pending = static_cast<PermissionReply*>(
                args.Data().As<v8::External>()->Value());
            if (pending->completed)
                return;
            pending->completed = true;
            bool allowed = args.Length()
                && args[0]->BooleanValue(args.GetIsolate());
            std::function<void(bool)> callback =
                std::move(pending->callback);
            callback(allowed);
        }, v8::External::New(isolate, pending)).ToLocalChecked();
    pending->function.Reset(isolate, respond);
    pending->function.SetWeak(pending,
        [](const v8::WeakCallbackInfo<PermissionReply>& info) {
            PermissionReply* pending = info.GetParameter();
            if (!pending->completed && pending->callback) {
                pending->runner->PostTask(FROM_HERE,
                    base::BindOnce([](std::function<void(bool)> callback) {
                        callback(false);
                    }, std::move(pending->callback)));
            }
            pending->function.Reset();
            delete pending;
        }, v8::WeakCallbackType::kParameter);
    v8::Local<v8::Value> argv[] = {
        v8::Integer::New(isolate, contentsId),
        gin_helper::ConvertToV8(isolate, permission),
        respond,
        gin_helper::ConvertToV8(isolate, details)
    };
    bool failed =
        handler->Call(context, v8::Undefined(isolate), 4, argv).IsEmpty();
    if (failed && !pending->completed) {
        pending->completed = true;
        std::function<void(bool)> callback =
            std::move(pending->callback);
        callback(false);
    }
}

SessionMgr* SessionMgr::m_inst = nullptr;
const char* ApiSession::kDefaultSessionName = "";
const char* ApiSession::kDefaultDir = "profiles";

SessionMgr::SessionMgr() = default;

SessionMgr* SessionMgr::get()
{
    static SessionMgr instance;
    m_inst = &instance;
    return m_inst;
}

bool SessionMgr::ensureRootDir()
{
    if (m_rootDir.empty()) {
#if BUILDFLAG(IS_WIN)
        base::FilePath module;
        if (!base::PathService::Get(base::DIR_MODULE, &module))
            return false;
        m_rootDir = mini_electron::migrateProfileDirectory(
            module, ApiSession::kDefaultDir, "minieleses");
#else
        base::FilePath appData;
        if (!base::PathService::Get(base::DIR_APP_DATA, &appData))
            return false;
        m_rootDir = mini_electron::migrateProfileDirectory(
            appData, "mini-electron", "miniblink132");
#endif
    }
    return base::CreateDirectory(m_rootDir) &&
        base::DirectoryExists(m_rootDir) &&
        base::PathIsWritable(m_rootDir);
}

bool SessionMgr::setRootDir(const base::FilePath& root)
{
    if (root.empty())
        return false;
    base::AutoLock lock(m_lock);
    if (!m_map.empty())
        return root == m_rootDir;
    if (!base::CreateDirectory(root) || !base::DirectoryExists(root) ||
        !base::PathIsWritable(root))
        return false;
    m_rootDir = root;
    return true;
}

base::FilePath SessionMgr::getRootDir() const
{
    base::AutoLock lock(m_lock);
    return m_rootDir;
}

ApiSession* SessionMgr::findOrCreateSession(v8::Isolate* isolate,
    const std::string& partition, bool createIfNotExist)
{
    base::AutoLock lock(m_lock);
    auto found = m_map.find(partition);
    if (found != m_map.end())
        return found->second;
    if (!createIfNotExist || !isolate || !ensureRootDir())
        return nullptr;

    bool persistent = false;
    base::FilePath path = createSessionDirname(partition, &persistent);
    if (!persistent
        && !base::CreateNewTempDirectory(
            FILE_PATH_LITERAL("mini-electron-session-"), &path)) {
        return nullptr;
    }
    if (persistent && !MigrateLegacyMiniElectronProfile(partition, path))
        return nullptr;
    std::string migrationError;
    if (persistent && !mini_electron::MigrateStockElectronProfile(
            m_rootDir, path, &migrationError))
        return nullptr;
    ApiSession* session = ApiSession::create(
        isolate, partition, path, persistent);
    if (!session) {
        if (!persistent)
            base::DeletePathRecursively(path);
        return nullptr;
    }
    m_map.emplace(partition, session);
    return session;
}

gin_helper::WrapperInfo ApiSession::kWrapperInfo = { gin_helper::GinEmbedder::kEmbedderNativeGin };
v8::Persistent<v8::Function> ApiSession::constructor;

void initializeBrowserSessionApi(v8::Local<v8::Object> exports, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, void* priv)
{
    ApiSession::init(context->GetIsolate(), exports);
}

static const char BrowserSessionName[] = "console.log('BrowserSessionNative');;";
static NodeNative BrowserSessionNative { "Screen", BrowserSessionName, sizeof(BrowserSessionName) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_session, initializeBrowserSessionApi, &BrowserSessionNative)

} // atom namespace
