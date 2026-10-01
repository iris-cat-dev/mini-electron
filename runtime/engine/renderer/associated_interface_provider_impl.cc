
#include "runtime/engine/renderer/associated_interface_provider_impl.h"
#include "runtime/engine/common/create_and_bind_templ.h"
#include "runtime/engine/renderer/screen_orientation_impl.h"
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "runtime/engine/renderer/blob_url_store_impl.h"
#include "runtime/engine/renderer/data_host_impl.h"
#include "runtime/engine/renderer/attribution_host_impl.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include <windows.h>

namespace content {

AssociatedInterfaceProviderImpl::AssociatedInterfaceProviderImpl(WebLocalFrameClientImpl* frameClient)
    : m_frameClient(frameClient)
{
}

void AssociatedInterfaceProviderImpl::GetAssociatedInterface(
    const std::string& name, ::mojo::PendingAssociatedReceiver<blink::mojom::AssociatedInterface> receiver)
{
    std::string output("AssociatedInterfaceProviderImpl::GetAssociatedInterface: ");
    output += name;
    output += "\n";
    OutputDebugStringA(output.c_str());

    if ("blink.mojom.BackForwardCacheControllerHost" == name) {
        //mojo::PendingAssociatedReceiver<::blink::mojom::blink::BackForwardCacheControllerHost> pendingReceiver(receiver.PassHandle());
        //m_backForwardCacheControllerHostReceiver.Bind(std::move(pendingReceiver));
        createAndBindInterface<::blink::mojom::blink::BackForwardCacheControllerHost, BackForwardCacheControllerHostImpl>(receiver.PassPipe());
    } else if ("blink.mojom.LocalFrameHost" == name) {
        //mojo::PendingAssociatedReceiver<::blink::mojom::blink::LocalFrameHost> pendingReceiver(receiver.PassHandle());
        //m_localFrameHostReceiver.Bind(std::move(pendingReceiver));
        createAndBindInterface<::blink::mojom::blink::LocalFrameHost, LocalFrameHostImpl>(receiver.PassPipe(), m_frameClient);
    } else if ("blink.mojom.LocalMainFrameHost" == name) {
        //mojo::PendingAssociatedReceiver<::blink::mojom::blink::LocalMainFrameHost> pendingReceiver(receiver.PassHandle());
        //m_localMainFrameHostReceiver.Bind(std::move(pendingReceiver));
        createAndBindInterface<::blink::mojom::blink::LocalMainFrameHost, LocalMainFrameHostImpl>(receiver.PassPipe(), m_frameClient);
    } else if ("blink.mojom.BroadcastChannelProvider" == name) {
        //mojo::PendingAssociatedReceiver<::blink::mojom::blink::BroadcastChannelProvider> pendingReceiver(receiver.PassHandle());
        //m_broadcastChannelProviderReceiver.Bind(std::move(pendingReceiver));
        createAndBindInterface<::blink::mojom::blink::BroadcastChannelProvider, BroadcastChannelProviderImpl>(receiver.PassPipe(), m_frameClient);
    } else if ("device.mojom.blink.ScreenOrientation" == name) {
        createAndBindInterface<::device::mojom::blink::ScreenOrientation, ScreenOrientationImpl>(receiver.PassPipe());
    } else if ("blink.mojom.blink.AttributionHost" == name) {
        createAndBindInterface<::blink::mojom::blink::AttributionHost, AttributionHostImpl>(receiver.PassPipe());
    } else if ("blink.mojom.BlobURLStore" == name) {
        ::scoped_refptr<const ::blink::SecurityOrigin> origin;
        if (m_frameClient && m_frameClient->getFrame()) {
            origin = m_frameClient->getFrame()->GetSecurityOrigin();
        }
        createAndBindInterface<::blink::mojom::blink::BlobURLStore, BlobURLStoreImpl>(std::move(receiver.PassPipe()), origin->ToRawString().Utf8());
    } else
        DebugBreak();
}

}