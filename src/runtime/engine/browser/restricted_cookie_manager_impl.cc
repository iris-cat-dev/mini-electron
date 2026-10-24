#include "src/runtime/engine/browser/restricted_cookie_manager_impl.h"

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "mojo/public/cpp/base/shared_memory_version.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/parsed_cookie.h"
#include "net/cookies/site_for_cookies.h"
#include "src/runtime/engine/browser/web_view_host.h"
#include "src/runtime/engine/common/live_id_detect.h"
#include "src/runtime/network/cookies/web_cookie_jar_curl_impl.h"
#include "src/runtime/network/loader/web_url_loader_manager.h"
#include "src/runtime/network/loader/web_url_loader_manager_util.h"
#include "services/network/public/mojom/restricted_cookie_manager.mojom.h"
#include "third_party/blink/renderer/platform/wtf/threading_primitives.h"
#include <algorithm>
#include <set>
#include <vector>

// The in-process bindings omit the generated proxy implementation. Keep its
// synchronous entry points functional by dispatching to the service overloads.
bool network::mojom::blink::RestrictedCookieManager::SetCookieFromString(const ::blink::KURL& url,
    const net::SiteForCookies& site_for_cookies, const scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin,
    net::StorageAccessApiStatus storage_access_api_status, const WTF::String& cookie)
{
    base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
    SetCookieFromString(url, site_for_cookies, top_frame_origin, storage_access_api_status, cookie, loop.QuitClosure());
    loop.Run();
    return true;
}

bool network::mojom::blink::RestrictedCookieManager::GetCookiesString(const ::blink::KURL& url,
    const net::SiteForCookies& site_for_cookies, const scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin,
    net::StorageAccessApiStatus storage_access_api_status, bool get_version_shared_memory, bool is_ad_tagged,
    bool force_disable_third_party_cookies, uint64_t* out_version, base::ReadOnlySharedMemoryRegion* out_version_buffer, WTF::String* out_cookies)
{
    base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
    GetCookiesString(url, site_for_cookies, top_frame_origin, storage_access_api_status, get_version_shared_memory, is_ad_tagged,
        force_disable_third_party_cookies, base::BindOnce([](uint64_t* version, base::ReadOnlySharedMemoryRegion* buffer,
            WTF::String* cookies, base::OnceClosure quit, uint64_t result_version, base::ReadOnlySharedMemoryRegion result_buffer,
            const WTF::String& result_cookies) {
            *version = result_version;
            *buffer = std::move(result_buffer);
            *cookies = result_cookies;
            std::move(quit).Run();
        }, out_version, out_version_buffer, out_cookies, loop.QuitClosure()));
    loop.Run();
    return true;
}

const char network::mojom::RestrictedCookieManager::Name_[] = "network.mojom.RestrictedCookieManager";

namespace content {
namespace {
using CookieList = std::vector<net::CanonicalCookie>;
using CookieJar = mini_electron::WebCookieJarImpl;

std::set<RestrictedCookieManagerImpl*>& managers()
{
    static base::NoDestructor<std::set<RestrictedCookieManagerImpl*>> instances;
    return *instances;
}

CookieJar* getCookieJar(int64_t webviewId)
{
    auto* webview = static_cast<WebViewHost*>(common::LiveIdDetect::getWebViewIds()->getPtr(webviewId));
    if (webview)
        return webview->getWebCookieJarImpl();
    auto* manager = mini_electron::WebURLLoaderManager::sharedInstance();
    return manager ? manager->getShareCookieJar() : nullptr;
}

bool cookieUrlEnabled(const blink::KURL& url, CookieJar* jar)
{
    return jar && jar->getCurlShareHandle() && url.IsValid() && url.ProtocolIsInHTTPFamily();
}

// Read the actual shared curl store, not a second cookie database. Curl's
// Netscape format preserves host-only, HttpOnly, Secure, path and expiry.
bool readCookies(CookieJar* jar, CookieList& cookies)
{
    if (!jar || !jar->getCurlShareHandle())
        return false;
    CURL* handle = curl_easy_init();
    if (!handle)
        return false;
    curl_slist* list = nullptr;
    CURLcode result = curl_easy_setopt(handle, CURLOPT_SHARE, jar->getCurlShareHandle());
    if (result == CURLE_OK)
        result = curl_easy_getinfo(handle, CURLINFO_COOKIELIST, &list);
    curl_easy_cleanup(handle);
    if (result != CURLE_OK) {
        curl_slist_free_all(list);
        return false;
    }
    const base::Time now = base::Time::Now();
    for (auto* item = list; item; item = item->next) {
        auto fields = base::SplitStringPiece(item->data, "\t", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
        if (fields.size() != 7)
            continue;
        std::string domain(fields[0]);
        bool http_only = base::StartsWith(domain, "#HttpOnly_");
        if (http_only)
            domain.erase(0, 10);
        if (domain.empty())
            continue;
        if (fields[1] == "TRUE" && domain.front() != '.')
            domain.insert(0, ".");
        else if (fields[1] != "TRUE" && domain.front() == '.')
            domain.erase(0, 1);
        int64_t expiry_seconds = 0;
        if (!base::StringToInt64(fields[4], &expiry_seconds))
            continue;
        base::Time expiry = expiry_seconds ? base::Time::FromSecondsSinceUnixEpoch(expiry_seconds) : base::Time();
        if (!expiry.is_null() && expiry <= now)
            continue;
        bool secure = fields[3] == "TRUE";
        auto cookie = net::CanonicalCookie::FromStorage(std::string(fields[5]), std::string(fields[6]), std::move(domain),
            std::string(fields[2]), base::Time(), expiry, base::Time(), base::Time(), secure, http_only,
            net::CookieSameSite::UNSPECIFIED, net::COOKIE_PRIORITY_DEFAULT, std::nullopt,
            secure ? net::CookieSourceScheme::kSecure : net::CookieSourceScheme::kNonSecure,
            url::PORT_UNSPECIFIED, net::CookieSourceType::kOther);
        if (cookie)
            cookies.push_back(std::move(*cookie));
    }
    curl_slist_free_all(list);
    return true;
}

bool sameKey(const net::CanonicalCookie& a, const net::CanonicalCookie& b)
{
    return a.Name() == b.Name() && a.Domain() == b.Domain() && a.Path() == b.Path();
}

bool sameValue(const net::CanonicalCookie& a, const net::CanonicalCookie& b)
{
    return a.Value() == b.Value() && a.ExpiryDate() == b.ExpiryDate() && a.IsSecure() == b.IsSecure() && a.IsHttpOnly() == b.IsHttpOnly();
}

bool visibleCookie(const net::CanonicalCookie& cookie, const blink::KURL& url)
{
    const GURL gurl(url);
    return !cookie.IsHttpOnly() && cookie.IsDomainMatch(gurl.host()) && cookie.IsOnPath(gurl.path())
        && (!cookie.IsSecure() || gurl.SchemeIsCryptographic());
}

network::mojom::blink::CookieAccessResultPtr accessResult(const net::CanonicalCookie& cookie)
{
    // Curl does not persist SameSite metadata; describe its actual legacy access
    // semantics rather than inventing a modern policy decision.
    return network::mojom::blink::CookieAccessResult::New(network::mojom::CookieEffectiveSameSite::kNoRestriction,
        network::mojom::CookieAccessSemantics::LEGACY, net::CookieInclusionStatus(), cookie.IsSecure());
}

bool storeCookie(CookieJar* jar, const blink::KURL& url, const net::CanonicalCookie& cookie)
{
    const GURL gurl(url);
    if (!cookieUrlEnabled(url, jar) || !cookie.IsCanonical() || cookie.IsHttpOnly() || cookie.IsPartitioned()
        || !cookie.IsDomainMatch(gurl.host()) || (cookie.IsSecure() && !gurl.SchemeIsCryptographic()))
        return false;

    // Serialize the read/validate/write transaction with HTTP cookie updates.
    WTF::Locker<WTF::RecursiveMutex> lock(*mini_electron::sharedResourceMutex(CURL_LOCK_DATA_COOKIE));
    CookieList before;
    if (!readCookies(jar, before))
        return false;
    for (const auto& existing : before) {
        if ((sameKey(existing, cookie) && existing.IsHttpOnly())
            || (!gurl.SchemeIsCryptographic() && existing.IsSecure() && cookie.IsEquivalentForSecureCookieMatching(existing)))
            return false;
    }
    int64_t expiry = cookie.IsPersistent() ? std::max<int64_t>(1, (cookie.ExpiryDate() - base::Time::UnixEpoch()).InSeconds()) : 0;
    std::string line = cookie.Domain() + "\t" + (cookie.IsDomainCookie() ? "TRUE" : "FALSE") + "\t" + cookie.Path()
        + "\t" + (cookie.IsSecure() ? "TRUE" : "FALSE") + "\t" + base::NumberToString(expiry) + "\t" + cookie.Name() + "\t" + cookie.Value();
    CURL* handle = curl_easy_init();
    if (!handle)
        return false;
    CURLcode result = curl_easy_setopt(handle, CURLOPT_SHARE, jar->getCurlShareHandle());
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_COOKIELIST, line.c_str());
    // Preserve the jar's existing persistence path; an empty path is an
    // in-memory cookie store, not a reason to silently discard the mutation.
    const std::string path = jar->getCookieJarFullPath();
    if (result == CURLE_OK && !path.empty()) {
        result = curl_easy_setopt(handle, CURLOPT_COOKIEJAR, path.c_str());
        if (result == CURLE_OK)
            result = curl_easy_setopt(handle, CURLOPT_COOKIELIST, "FLUSH");
    }
    curl_easy_cleanup(handle);
    if (result != CURLE_OK)
        return false;
    CookieList after;
    if (!readCookies(jar, after))
        return false;
    const auto found = std::find_if(after.begin(), after.end(), [&](const auto& stored) { return sameKey(stored, cookie); });
    if (cookie.IsExpired(base::Time::Now()))
        return found == after.end();
    return found != after.end() && found->Value() == cookie.Value() && found->IsSecure() == cookie.IsSecure();
}
} // namespace

struct RestrictedCookieManagerImpl::ChangeListener {
    blink::KURL url;
    mojo::Remote<network::mojom::blink::CookieChangeListener> remote;
    CookieList cookies;
};

RestrictedCookieManagerImpl::RestrictedCookieManagerImpl(int64_t webviewId)
    : m_webviewId(webviewId)
{
    managers().insert(this);
}

RestrictedCookieManagerImpl::~RestrictedCookieManagerImpl()
{
    managers().erase(this);
}

void RestrictedCookieManagerImpl::GetAllForUrl(const blink::KURL& url, const net::SiteForCookies& site_for_cookies,
    const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin, net::StorageAccessApiStatus storage_access_api_status,
    network::mojom::blink::CookieManagerGetOptionsPtr options, bool is_ad_tagged, bool force_disable_third_party_cookies,
    GetAllForUrlCallback callback)
{
    WTF::Vector<network::mojom::blink::CookieWithAccessResultPtr> results;
    auto* jar = getCookieJar(m_webviewId);
    CookieList cookies;
    if (cookieUrlEnabled(url, jar) && (!force_disable_third_party_cookies || site_for_cookies.IsFirstParty(GURL(url))) && readCookies(jar, cookies)) {
        const std::string name = options ? options->name.Utf8() : std::string();
        for (const auto& cookie : cookies) {
            if (!visibleCookie(cookie, url))
                continue;
            if (options && (options->match_type == network::mojom::CookieMatchType::EQUALS
                    ? cookie.Name() != name : !base::StartsWith(cookie.Name(), name)))
                continue;
            results.push_back(network::mojom::blink::CookieWithAccessResult::New(cookie, accessResult(cookie)));
        }
    }
    std::move(callback).Run(std::move(results));
}

void RestrictedCookieManagerImpl::SetCanonicalCookie(const net::CanonicalCookie& cookie, const blink::KURL& url,
    const net::SiteForCookies& site_for_cookies, const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin,
    net::StorageAccessApiStatus storage_access_api_status, net::CookieInclusionStatus status, SetCanonicalCookieCallback callback)
{
    bool stored = status.IsInclude() && storeCookie(getCookieJar(m_webviewId), url, cookie);
    if (stored)
        NotifyCookieWrite();
    std::move(callback).Run(stored);
}

void RestrictedCookieManagerImpl::AddChangeListener(const blink::KURL& url, const net::SiteForCookies& site_for_cookies,
    const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin, net::StorageAccessApiStatus storage_access_api_status,
    mojo::PendingRemote<network::mojom::blink::CookieChangeListener> listener, AddChangeListenerCallback callback)
{
    auto entry = std::make_unique<ChangeListener>();
    auto* jar = getCookieJar(m_webviewId);
    if (listener.is_valid() && cookieUrlEnabled(url, jar) && readCookies(jar, entry->cookies)) {
        entry->url = url;
        entry->remote.Bind(std::move(listener));
        m_listeners.emplace(m_nextListenerId++, std::move(entry));
        // HTTP writes and expiry happen outside this interface. Poll the shared
        // native store only while an observer is registered; DOM writes also
        // refresh immediately so successive script writes are not coalesced.
        if (!m_changeTimer.IsRunning())
            m_changeTimer.Start(FROM_HERE, base::Milliseconds(100), this, &RestrictedCookieManagerImpl::PollChangeListeners);
    }
    if (callback)
        std::move(callback).Run();
}

void RestrictedCookieManagerImpl::NotifyCookieWrite()
{
    for (auto* manager : managers())
        manager->PollChangeListeners();
}

void RestrictedCookieManagerImpl::PollChangeListeners()
{
    if (m_listeners.empty())
        return;
    CookieList current;
    if (!readCookies(getCookieJar(m_webviewId), current))
        return;
    for (auto it = m_listeners.begin(); it != m_listeners.end();) {
        auto& entry = *it->second;
        if (!entry.remote.TryGet()) {
            it = m_listeners.erase(it);
            continue;
        }
        auto enqueue = [&](const net::CanonicalCookie& cookie, network::mojom::CookieChangeCause cause) {
            if (!visibleCookie(cookie, entry.url))
                return;
            auto change = network::mojom::blink::CookieChangeInfo::New(cookie, accessResult(cookie), cause);
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                base::BindOnce(&RestrictedCookieManagerImpl::DeliverChange, m_weakFactory.GetWeakPtr(), it->first, std::move(change)));
        };
        for (const auto& old : entry.cookies) {
            auto found = std::find_if(current.begin(), current.end(), [&](const auto& cookie) { return sameKey(old, cookie); });
            if (found == current.end())
                enqueue(old, old.IsExpired(base::Time::Now()) ? network::mojom::CookieChangeCause::EXPIRED : network::mojom::CookieChangeCause::EXPLICIT);
            else if (!sameValue(old, *found))
                enqueue(old, network::mojom::CookieChangeCause::OVERWRITE);
        }
        for (const auto& cookie : current) {
            auto found = std::find_if(entry.cookies.begin(), entry.cookies.end(), [&](const auto& old) { return sameKey(old, cookie); });
            if (found == entry.cookies.end() || !sameValue(*found, cookie))
                enqueue(cookie, network::mojom::CookieChangeCause::INSERTED);
        }
        entry.cookies = current;
        ++it;
    }
    if (m_listeners.empty())
        m_changeTimer.Stop();
}

void RestrictedCookieManagerImpl::DeliverChange(size_t id, network::mojom::blink::CookieChangeInfoPtr change)
{
    auto it = m_listeners.find(id);
    if (it != m_listeners.end()) {
        if (auto* listener = it->second->remote.TryGet())
            listener->OnCookieChange(std::move(change));
    }
}

bool RestrictedCookieManagerImpl::SetCookieFromString(const blink::KURL& url, const net::SiteForCookies& site_for_cookies,
    const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin, net::StorageAccessApiStatus storage_access_api_status, const WTF::String& cookie)
{
    if (!cookieUrlEnabled(url, getCookieJar(m_webviewId)))
        return true;
    const std::string line = cookie.Utf8();
    net::ParsedCookie parsed(line);
    // Curl has no partition-key storage. Never silently turn a partitioned
    // script cookie into an unrestricted cookie.
    if (!parsed.IsValid() || parsed.IsPartitioned())
        return true;
    net::CookieInclusionStatus status;
    auto canonical = net::CanonicalCookie::Create(GURL(url), line, base::Time::Now(), std::nullopt, std::nullopt,
        net::CookieSourceType::kScript, &status);
    if (canonical && status.IsInclude() && storeCookie(getCookieJar(m_webviewId), url, *canonical))
        NotifyCookieWrite();
    // This bool reports transport completion, not cookie acceptance. The DOM
    // setter silently ignores rejected cookies, as does the void Mojo response.
    return true;
}

void RestrictedCookieManagerImpl::SetCookieFromString(const blink::KURL& url, const net::SiteForCookies& site_for_cookies,
    const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin, net::StorageAccessApiStatus storage_access_api_status,
    const WTF::String& cookie, SetCookieFromStringCallback callback)
{
    SetCookieFromString(url, site_for_cookies, top_frame_origin, storage_access_api_status, cookie);
    std::move(callback).Run();
}

bool RestrictedCookieManagerImpl::GetCookiesString(const blink::KURL& url, const net::SiteForCookies& site_for_cookies,
    const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin, net::StorageAccessApiStatus storage_access_api_status,
    bool get_version_shared_memory, bool is_ad_tagged, bool force_disable_third_party_cookies, uint64_t* out_version,
    base::ReadOnlySharedMemoryRegion* out_version_buffer, WTF::String* out_cookies)
{
    // No shared-memory invalidation is available for curl's HTTP-side writes.
    // Explicitly return the protocol's uncached mode instead of stale versions.
    *out_version = mojo::shared_memory_version::kInvalidVersion;
    *out_version_buffer = base::ReadOnlySharedMemoryRegion();
    *out_cookies = WTF::String::FromUTF8("");
    auto* jar = getCookieJar(m_webviewId);
    if (!cookieUrlEnabled(url, jar) || (force_disable_third_party_cookies && !site_for_cookies.IsFirstParty(GURL(url))))
        return true;
    CookieList cookies;
    if (!readCookies(jar, cookies))
        return false;
    std::stable_sort(cookies.begin(), cookies.end(), [](const auto& a, const auto& b) { return a.Path().size() > b.Path().size(); });
    std::string result;
    for (const auto& cookie : cookies) {
        if (!visibleCookie(cookie, url))
            continue;
        if (!result.empty())
            result += "; ";
        result += cookie.Name() + "=" + cookie.Value();
    }
    *out_cookies = WTF::String::FromUTF8(result);
    return true;
}

void RestrictedCookieManagerImpl::GetCookiesString(const blink::KURL& url, const net::SiteForCookies& site_for_cookies,
    const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin, net::StorageAccessApiStatus storage_access_api_status,
    bool get_version_shared_memory, bool is_ad_tagged, bool force_disable_third_party_cookies, GetCookiesStringCallback callback)
{
    uint64_t version;
    base::ReadOnlySharedMemoryRegion buffer;
    WTF::String cookies;
    GetCookiesString(url, site_for_cookies, top_frame_origin, storage_access_api_status, get_version_shared_memory, is_ad_tagged,
        force_disable_third_party_cookies, &version, &buffer, &cookies);
    std::move(callback).Run(version, std::move(buffer), cookies);
}

bool RestrictedCookieManagerImpl::CookiesEnabledFor(const blink::KURL& url, const net::SiteForCookies& site_for_cookies,
    const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin, net::StorageAccessApiStatus storage_access_api_status, bool* out_cookies_enabled)
{
    *out_cookies_enabled = cookieUrlEnabled(url, getCookieJar(m_webviewId));
    return true;
}

void RestrictedCookieManagerImpl::CookiesEnabledFor(const blink::KURL& url, const net::SiteForCookies& site_for_cookies,
    const scoped_refptr<const blink::SecurityOrigin>& top_frame_origin, net::StorageAccessApiStatus storage_access_api_status, CookiesEnabledForCallback callback)
{
    bool enabled;
    CookiesEnabledFor(url, site_for_cookies, top_frame_origin, storage_access_api_status, &enabled);
    std::move(callback).Run(enabled);
}
} // namespace content
