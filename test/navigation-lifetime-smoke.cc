// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include <cstdio>
#include <memory>
#include <vector>

#include "base/at_exit.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "mojo/public/c/system/functions.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "src/runtime/engine/browser/web_frame_main.h"
#include "src/runtime/engine/common/thread_call.h"
#include "src/runtime/engine/renderer/web_local_frame_client_impl.h"
#include "src/runtime/network/loader/loader_factory_impl.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"

namespace {
class TrackedURLLoader final : public mini_electron::URLLoaderImpl {
public:
    explicit TrackedURLLoader(int* destroyed)
        : URLLoaderImpl(nullptr), destroyed_(destroyed) {}
    ~TrackedURLLoader() override { ++*destroyed_; }
private:
    int* destroyed_;
};

bool DetachedFramesRetireTheirRegistryEntries()
{
    std::vector<intptr_t> ids;
    std::vector<std::unique_ptr<content::WebLocalFrameClientImpl>> clients;
    std::vector<base::WeakPtr<blink::WeakWrapperResourceLoadInfoNotifier>> notifiers;
    for (int index = 0; index < 64; ++index) {
        const blink::LocalFrameToken token;
        const intptr_t id = blink::LocalFrameToken::Hasher()(token);
        clients.push_back(std::make_unique<content::WebLocalFrameClientImpl>(0, token));
        const auto notifier = clients.back()->getResourceLoadInfoNotifier();
        if (!notifier || notifier.get() != clients.back()->getResourceLoadInfoNotifier().get())
            return false;
        notifiers.push_back(notifier);
        new content::WebFrameMain(id, 0, true);
        ids.push_back(id);
    }
    base::RunLoop().RunUntilIdle();
    for (const auto id : ids) {
        if (!content::WebFrameMain::fromFrameId(id)) {
            std::fputs("FAIL: live frame missing from registry\n", stderr);
            return false;
        }
    }
    // FrameDetached clears m_frame before deleting its client. The frame token
    // remains valid even though the WebLocalFrame is no longer dereferenceable.
    clients.clear();
    base::RunLoop().RunUntilIdle();
    int retained = 0;
    int retainedNotifiers = 0;
    for (const auto& notifier : notifiers) {
        if (notifier)
            ++retainedNotifiers;
    }
    for (const auto id : ids) {
        if (content::WebFrameMain::fromFrameId(id)) {
            ++retained;
            content::WebFrameMain::destroy(id);
        }
    }
    base::RunLoop().RunUntilIdle();
    std::printf("frame-registry: live=64 retained-after-client-destruction=%d\n", retained);
    std::printf("frame-notifiers: retained-after-client-destruction=%d\n", retainedNotifiers);
    return retained == 0 && retainedNotifiers == 0;
}

bool StandaloneLoaderDiesWhenItsConsumerDisconnects()
{
    int destroyed = 0;
    auto* loader = new TrackedURLLoader(&destroyed);
    mojo::Remote<network::mojom::URLLoader> consumer(loader->m_urlLoader.BindNewPipeAndPassRemote());
    consumer.reset();
    base::RunLoop().RunUntilIdle();
    std::printf("standalone-loader: destroyed-after-peer-disconnect=%d\n", destroyed);
    const bool released = destroyed == 1;
    if (!released) delete loader;
    return released;
}
}

int main()
{
    base::AtExitManager at_exit;
    base::SingleThreadTaskExecutor executor(base::MessagePumpType::UI);
    WTF::Partitions::Initialize();
    WTF::Initialize();
    if (MojoInitialize(nullptr) != MOJO_RESULT_OK) return 2;
    content::ThreadCall::init(nullptr);
    const bool frames = DetachedFramesRetireTheirRegistryEntries();
    const bool loaders = StandaloneLoaderDiesWhenItsConsumerDisconnects();
    base::RunLoop().RunUntilIdle();
    return frames && loaders ? 0 : 1;
}
