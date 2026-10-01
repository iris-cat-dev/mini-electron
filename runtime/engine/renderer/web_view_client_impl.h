
#ifndef content_renderer_WebViewClientImpl_h
#define content_renderer_WebViewClientImpl_h

#include "third_party/blink/public/web/web_view_client.h"

namespace content {

class WebViewHost;

class WebViewClientImpl : public blink::WebViewClient {
public:
    WebViewClientImpl(WebViewHost* mbwebview)
    {
        m_engineViewHost = mbwebview;
    }

    ~WebViewClientImpl() override
    {
    }

    WebViewHost* getEngineViewHost() const
    {
        return m_engineViewHost;
    }

    void InvalidateContainer() override
    {
    }

    void DidAutoResize(const gfx::Size& new_size) override
    {
    }

    void DidFocus() override
    {
    }

    void OnDestruct() override
    {
    }

private:
    WebViewHost* m_engineViewHost;
};

}

#endif // content_renderer_WebViewClientImpl_h