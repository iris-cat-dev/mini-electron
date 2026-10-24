// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef content_viz_OffscreenDisplayClient_h
#define content_viz_OffscreenDisplayClient_h

#include <memory>

#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "components/viz/host/viz_host_export.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/viz/privileged/mojom/compositing/display_private.mojom.h"
#include "services/viz/privileged/mojom/compositing/layered_window_updater.mojom.h"
#include "ui/gfx/native_widget_types.h"

namespace content {

class OffscreenWindowUpdater;
class WebViewHost;

// mojom::DisplayClient implementation that relays calls to platform specific
// functions.
class OffscreenDisplayClient : public viz::mojom::DisplayClient {
public:
    explicit OffscreenDisplayClient(WebViewHost* mbwebview);

    OffscreenDisplayClient(const OffscreenDisplayClient&) = delete;
    OffscreenDisplayClient& operator=(const OffscreenDisplayClient&) = delete;

    ~OffscreenDisplayClient() override;

    mojo::PendingRemote<viz::mojom::DisplayClient> GetBoundRemote(scoped_refptr<base::SingleThreadTaskRunner> task_runner);

private:
    // mojom::DisplayClient implementation:

    void CreateLayeredWindowUpdater(mojo::PendingReceiver<viz::mojom::LayeredWindowUpdater> receiver) override;

// TODO(crbug.com/1052397): Revisit the macro expression once build flag switch
// of lacros-chrome is complete.

    void AddChildWindowToBrowser(::gpu::SurfaceHandle child_window) override;

    mojo::Receiver<viz::mojom::DisplayClient> m_receiver { this };

    int64_t m_engineViewId;
    gfx::AcceleratedWidget m_hwnd = gfx::kNullAcceleratedWidget;

    bool m_isAutoDrawToHwnd = true;

    std::unique_ptr<OffscreenWindowUpdater> m_offscreenWindowUpdater;
};

} // namespace content

#endif // content_viz_OffscreenDisplayClient_h
