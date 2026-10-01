
#ifndef content_renderer_AssociatedInterfaceProviderImpl_h
#define content_renderer_AssociatedInterfaceProviderImpl_h

#include "third_party/blink/public/mojom/associated_interfaces/associated_interfaces.mojom.h"
#include "runtime/engine/browser/local_frame_host_impl.h"
#include "runtime/engine/browser/back_forward_cache_controller_host_impl.h"
#include "runtime/engine/browser/local_main_frame_host_impl.h"
#include "runtime/engine/browser/broadcast_channel_provider_impl.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"

namespace content {

class WebLocalFrameClientImpl;

class AssociatedInterfaceProviderImpl : public blink::mojom::AssociatedInterfaceProvider {
public:
    AssociatedInterfaceProviderImpl(WebLocalFrameClientImpl* frameClient);

    void GetAssociatedInterface(const std::string& name, ::mojo::PendingAssociatedReceiver<blink::mojom::AssociatedInterface> receiver) override;

private:
    //mojo::AssociatedReceiver<::blink::mojom::blink::LocalFrameHost> m_localFrameHostReceiver;
    //mojo::AssociatedReceiver<::blink::mojom::blink::BackForwardCacheControllerHost> m_backForwardCacheControllerHostReceiver;
    //mojo::AssociatedReceiver<::blink::mojom::blink::LocalMainFrameHost> m_localMainFrameHostReceiver;
    //mojo::AssociatedReceiver<::blink::mojom::blink::BroadcastChannelProvider> m_broadcastChannelProviderReceiver;

    WebLocalFrameClientImpl* m_frameClient;
};

}

#endif // content_renderer_AssociatedInterfaceProviderImpl_h