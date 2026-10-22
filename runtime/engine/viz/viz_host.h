// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_DEMO_HOST_DEMO_HOST_H_
#define COMPONENTS_VIZ_DEMO_HOST_DEMO_HOST_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/threading/thread.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "components/viz/common/surfaces/parent_local_surface_id_allocator.h"
#include "components/viz/host/host_display_client.h"
#include "components/viz/host/host_frame_sink_client.h"
#include "components/viz/host/host_frame_sink_manager.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/viz/privileged/mojom/compositing/display_private.mojom.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_manager.mojom.h"
#include "ui/gfx/native_widget_types.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "ui/display/types/display_constants.h"
#endif

namespace viz {
class HostDisplayClient;
}

namespace content {

class VizClient;
class OffscreenDisplayClient;
class WebViewHost;

// VizHost is the 'host', i.e. the privileged component, responsible for
// managing the service, and establishing the connection between the clients and
// the service.
class VizHost : public viz::HostFrameSinkClient {
public:
    VizHost(WebViewHost* mbwebview, bool isTransparent,
        //gfx::AcceleratedWidget widget,
        const gfx::Size& size, mojo::PendingReceiver<viz::mojom::FrameSinkManagerClient> client_receiver,
        mojo::PendingRemote<viz::mojom::FrameSinkManager> frame_sink_manager_remote, scoped_refptr<base::SingleThreadTaskRunner> work_runner,
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
        int64_t vsync_display_id,
#endif
        scoped_refptr<base::SingleThreadTaskRunner> service_runner);

    VizHost(const VizHost&) = delete;
    VizHost& operator=(const VizHost&) = delete;

    ~VizHost() override;
    void destroy(std::function<void(void)>&& callback);

    void resizeOnBlinkThread(const gfx::Size& size, float device_scale_factor);
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    void setVSyncDisplayIdOnBlinkThread(int64_t display_id);
    void setVisibleOnBlinkThread(bool visible);
#endif

    viz::HostFrameSinkManager* host_frame_sink_manager()
    {
        return &m_hostFrameSinkManager;
    }

    VizClient* getRootClient() const
    {
        return m_rootClient.get();
    }


private:
    class DisplayPrivateState;

    void onRootFrameCompletedOnHostThread(uint64_t frame_revision);
    void resizeOnHostThread(const gfx::Size& size, float device_scale_factor);
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    void setVSyncDisplayIdOnHostThread(int64_t display_id);
    void setVisibleOnHostThread(bool visible);
#endif

    void embedClients(VizClient* embedder_client, const gfx::Rect& child_bounds);

    void initialize(mojo::PendingReceiver<viz::mojom::FrameSinkManagerClient> receiver, mojo::PendingRemote<viz::mojom::FrameSinkManager> remote);

    // viz::HostFrameSinkClient:
    void OnFirstSurfaceActivation(const viz::SurfaceInfo& surface_info) override;
    void OnFrameTokenChanged(uint32_t frame_token, base::TimeTicks activation_time) override;

    const gfx::AcceleratedWidget m_widget;
    WebViewHost* m_engineViewHost = nullptr;
    bool m_isTransparent = false;

    viz::FrameSinkId m_frameSinkId;

    gfx::Size m_size;
    gfx::Size m_pendingDisplayResizeSize;
    uint64_t m_pendingDisplayResizeRevision = 0;
    viz::HostFrameSinkManager m_hostFrameSinkManager;
    scoped_refptr<DisplayPrivateState> m_displayPrivateState;
    std::unique_ptr</*viz::HostDisplayClient*/ OffscreenDisplayClient> m_displayClient;
    viz::ParentLocalSurfaceIdAllocator m_allocator;

    std::unique_ptr<VizClient> m_rootClient;
    std::vector<std::unique_ptr<VizClient>> m_embeddedClients;

    // The thread is created to demonstrate that the host can run on a separate
    // thread (or even a separate process), since it communicates with the service
    // and clients over mojo. The host does not need to have its own thread
    // though.
    //base::Thread m_thread;

    base::WaitableEvent m_initialized;
    // Created and first dereferenced on the host sequence. Callbacks arriving
    // on other sequences use BindPostTask before consulting this pointer.
    base::WeakPtr<VizHost> m_hostWeakPtr;
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    int64_t m_vsyncDisplayId = display::kInvalidDisplayId;
    bool m_visible = true;
#endif

    scoped_refptr<base::SingleThreadTaskRunner> m_workRunner;
    scoped_refptr<base::SingleThreadTaskRunner> m_serviceRunner;
    base::WeakPtrFactory<VizHost> m_hostWeakPtrFactory { this };
};

} // namespace demo

#endif // COMPONENTS_VIZ_DEMO_HOST_DEMO_HOST_H_
