
#ifndef content_browser_RestrictedCookieManagerImpl_h
#define content_browser_RestrictedCookieManagerImpl_h

#include "services/network/public/mojom/restricted_cookie_manager.mojom-blink.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include <map>
#include <memory>

namespace content {

class RestrictedCookieManagerImpl : public ::network::mojom::blink::RestrictedCookieManager {
public:
    RestrictedCookieManagerImpl(int64_t webviewId);
    ~RestrictedCookieManagerImpl() override;

    virtual void GetAllForUrl(const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status,
      ::network::mojom::blink::CookieManagerGetOptionsPtr options,
        bool is_ad_tagged, bool force_disable_third_party_cookies, GetAllForUrlCallback callback) override;

    void SetCanonicalCookie(const ::net::CanonicalCookie& cookie, const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status, ::net::CookieInclusionStatus status,
        SetCanonicalCookieCallback callback) override;

    void AddChangeListener(const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status, 
        ::mojo::PendingRemote<::network::mojom::blink::CookieChangeListener> listener, AddChangeListenerCallback callback) override;

    bool SetCookieFromString(const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status,
        const WTF::String& cookie) override;

    void SetCookieFromString(const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status,
        const WTF::String& cookie, SetCookieFromStringCallback callback) override;

    bool GetCookiesString(const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status,
        bool get_version_shared_memory, bool is_ad_tagged, bool force_disable_third_party_cookies, uint64_t* out_version,
        ::base::ReadOnlySharedMemoryRegion* out_version_buffer, WTF::String* out_cookies) override;

    void GetCookiesString(const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status,
        bool get_version_shared_memory, bool is_ad_tagged, bool force_disable_third_party_cookies, GetCookiesStringCallback callback) override;

    bool CookiesEnabledFor(const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status,
        bool* out_cookies_enabled) override;

    void CookiesEnabledFor(const ::blink::KURL& url, const ::net::SiteForCookies& site_for_cookies,
        const ::scoped_refptr<const ::blink::SecurityOrigin>& top_frame_origin, ::net::StorageAccessApiStatus storage_access_api_status,
        CookiesEnabledForCallback callback) override;

private:
    struct ChangeListener;
    void PollChangeListeners();
    void DeliverChange(size_t id, network::mojom::blink::CookieChangeInfoPtr change);
    static void NotifyCookieWrite();

    std::map<size_t, std::unique_ptr<ChangeListener>> m_listeners;
    size_t m_nextListenerId = 0;
    base::RepeatingTimer m_changeTimer;
    int64_t m_webviewId = 0;
    base::WeakPtrFactory<RestrictedCookieManagerImpl> m_weakFactory { this };
};

}

#endif // content_browser_RestrictedCookieManagerImpl_h