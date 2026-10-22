// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_PUBLIC_CPP_COOKIE_MANAGER_SHARED_MOJOM_TRAITS_H_
#define SERVICES_NETWORK_PUBLIC_CPP_COOKIE_MANAGER_SHARED_MOJOM_TRAITS_H_

#include "mojo/public/cpp/bindings/enum_traits.h"
#include "mojo/public/cpp/bindings/struct_traits.h"
#include "net/base/schemeful_site.h"
#if !defined(MINI_UI_QUICKJS)
#include "net/cookies/cookie_inclusion_status.h"
#endif
#include "net/cookies/site_for_cookies.h"
#include "services/network/public/cpp/schemeful_site_mojom_traits.h"
#if !defined(MINI_UI_QUICKJS)
#include "services/network/public/mojom/cookie_manager.mojom-shared.h"
#endif
#include "services/network/public/mojom/site_for_cookies.mojom-shared.h"

namespace mojo {

template <> struct COMPONENT_EXPORT(NETWORK_CPP_COOKIES) StructTraits<network::mojom::SiteForCookiesDataView, net::SiteForCookies> {
    static const net::SchemefulSite& site(const net::SiteForCookies& input)
    {
        return input.site();
    }

    static bool schemefully_same(const net::SiteForCookies& input)
    {
        return input.schemefully_same();
    }

    static bool Read(network::mojom::SiteForCookiesDataView data, net::SiteForCookies* out);
};

#if !defined(MINI_UI_QUICKJS)
template <> struct COMPONENT_EXPORT(NETWORK_CPP_COOKIES) EnumTraits<network::mojom::CookieExemptionReason, net::CookieInclusionStatus::ExemptionReason> {
    static network::mojom::CookieExemptionReason ToMojom(net::CookieInclusionStatus::ExemptionReason input);

    static bool FromMojom(network::mojom::CookieExemptionReason input, net::CookieInclusionStatus::ExemptionReason* output);
};

template <> struct COMPONENT_EXPORT(NETWORK_CPP_COOKIES) StructTraits<network::mojom::CookieInclusionStatusDataView, net::CookieInclusionStatus> {
    static uint32_t exclusion_reasons(const net::CookieInclusionStatus& s)
    {
        return static_cast<uint32_t>(s.exclusion_reasons().to_ulong());
    }
    static uint32_t warning_reasons(const net::CookieInclusionStatus& s)
    {
        return static_cast<uint32_t>(s.warning_reasons().to_ulong());
    }
    static net::CookieInclusionStatus::ExemptionReason exemption_reason(const net::CookieInclusionStatus& s)
    {
        return s.exemption_reason();
    }
    static bool Read(network::mojom::CookieInclusionStatusDataView status, net::CookieInclusionStatus* out);
};
#endif

} // namespace mojo

#endif // SERVICES_NETWORK_PUBLIC_CPP_COOKIE_MANAGER_SHARED_MOJOM_TRAITS_H_
