// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_FILEAPI_FILE_BACKED_BLOB_FACTORY_DISPATCHER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_FILEAPI_FILE_BACKED_BLOB_FACTORY_DISPATCHER_H_

#if defined(MINI_UI_QUICKJS)
#include <memory>
#include "mojo/public/cpp/bindings/remote.h"
namespace mini_ui {
class LocalResources;
}
#endif

#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#endif
#include "third_party/blink/public/mojom/blob/file_backed_blob_factory.mojom-blink.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/execution_context/execution_context_lifecycle_observer.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/platform/mojo/heap_mojo_associated_remote.h"
#include "third_party/blink/renderer/platform/mojo/heap_mojo_remote.h"
#endif
#include "third_party/blink/renderer/platform/supplementable.h"

namespace blink {

class CORE_EXPORT FileBackedBlobFactoryDispatcher : public GarbageCollected<FileBackedBlobFactoryDispatcher>,
                                                    public Supplement<ExecutionContext>,
                                                    public ExecutionContextClient {
public:
    virtual ~FileBackedBlobFactoryDispatcher() = default;

    explicit FileBackedBlobFactoryDispatcher(ExecutionContext& context);

    static mojom::blink::FileBackedBlobFactory* GetFileBackedBlobFactory(ExecutionContext* context);

#if !defined(MINI_UI_QUICKJS)
    void SetFileBackedBlobFactoryForTesting(mojo::PendingAssociatedRemote<mojom::blink::FileBackedBlobFactory> factory);

    void FlushForTesting();
#endif

    // GC
    void Trace(Visitor* visitor) const override;

    static const char kSupplementName[];

private:
    friend class FileBackedBlobFactoryTestHelper;

    static FileBackedBlobFactoryDispatcher* From(ExecutionContext& context);

    mojom::blink::FileBackedBlobFactory* GetFileBackedBlobFactory();

#if defined(MINI_UI_QUICKJS)
    std::shared_ptr<mini_ui::LocalResources> native_resources_;
    mojo::Remote<mojom::blink::FileBackedBlobFactory> native_factory_;
#else
    HeapMojoAssociatedRemote<mojom::blink::FileBackedBlobFactory> frame_remote_;

    HeapMojoRemote<mojom::blink::FileBackedBlobFactory> worker_remote_;
#endif
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_CORE_FILEAPI_FILE_BACKED_BLOB_FACTORY_DISPATCHER_H_
