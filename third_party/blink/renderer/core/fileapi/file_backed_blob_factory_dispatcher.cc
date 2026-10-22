
// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/fileapi/file_backed_blob_factory_dispatcher.h"

#if !defined(MINI_UI_QUICKJS)
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#endif
#include "third_party/blink/public/mojom/blob/file_backed_blob_factory.mojom-blink.h"
#if defined(MINI_UI_QUICKJS)
#include "runtime/ui/bindings_initializer.h"
#endif
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/public/platform/task_type.h"
#endif
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"
#endif

namespace blink {
namespace {

#if !defined(MINI_UI_QUICKJS)
mojom::blink::FileBackedBlobFactory* GetFileBackedBlobFactory(HeapMojoRemote<mojom::blink::FileBackedBlobFactory>& remote, ExecutionContext* execution_context)
{
    if (!remote.is_bound()) {
        mojo::PendingReceiver<mojom::blink::FileBackedBlobFactory> receiver
            = remote.BindNewPipeAndPassReceiver(execution_context->GetTaskRunner(blink::TaskType::kMiscPlatformAPI));
        execution_context->GetBrowserInterfaceBroker().GetInterface(std::move(receiver));
    }
    return remote.get();
}

mojom::blink::FileBackedBlobFactory* GetFileBackedBlobFactory(
    HeapMojoAssociatedRemote<mojom::blink::FileBackedBlobFactory>& remote, ExecutionContext* execution_context, LocalFrame* frame)
{
    if (!remote.is_bound()) {
        mojo::PendingAssociatedReceiver<mojom::blink::FileBackedBlobFactory> receiver
            = remote.BindNewEndpointAndPassReceiver(execution_context->GetTaskRunner(TaskType::kMiscPlatformAPI));
        frame->GetRemoteNavigationAssociatedInterfaces()->GetInterface(std::move(receiver));
    }
    return remote.get();
}
#endif

} // namespace

FileBackedBlobFactoryDispatcher::FileBackedBlobFactoryDispatcher(
    ExecutionContext& context)
    : Supplement<ExecutionContext>(context)
    , ExecutionContextClient(&context)
#if !defined(MINI_UI_QUICKJS)
    , frame_remote_(&context)
    , worker_remote_(&context)
#endif
{
}

// static
mojom::blink::FileBackedBlobFactory* FileBackedBlobFactoryDispatcher::GetFileBackedBlobFactory(ExecutionContext* context)
{
    if (!context) {
        return nullptr;
    }
    return From(*context)->GetFileBackedBlobFactory();
}

#if !defined(MINI_UI_QUICKJS)
void FileBackedBlobFactoryDispatcher::SetFileBackedBlobFactoryForTesting(mojo::PendingAssociatedRemote<mojom::blink::FileBackedBlobFactory> factory)
{
    auto* execution_context = GetExecutionContext();
    if (!execution_context) {
        return;
    }

    frame_remote_.Bind(std::move(factory), execution_context->GetTaskRunner(TaskType::kMiscPlatformAPI));
}

void FileBackedBlobFactoryDispatcher::FlushForTesting()
{
    if (frame_remote_.is_bound()) {
        frame_remote_.FlushForTesting();
    }
    if (worker_remote_.is_bound()) {
        worker_remote_.FlushForTesting();
    }
}
#endif

void FileBackedBlobFactoryDispatcher::Trace(Visitor* visitor) const
{
    Supplement<ExecutionContext>::Trace(visitor);
    ExecutionContextClient::Trace(visitor);
#if !defined(MINI_UI_QUICKJS)
    visitor->Trace(frame_remote_);
    visitor->Trace(worker_remote_);
#endif
}

// static
FileBackedBlobFactoryDispatcher* FileBackedBlobFactoryDispatcher::From(ExecutionContext& context)
{
    auto* dispatcher = Supplement<ExecutionContext>::From<FileBackedBlobFactoryDispatcher>(&context);
    if (!dispatcher) {
        dispatcher = MakeGarbageCollected<FileBackedBlobFactoryDispatcher>(context);
        Supplement<ExecutionContext>::ProvideTo(context, dispatcher);
    }
    return dispatcher;
}

mojom::blink::FileBackedBlobFactory* FileBackedBlobFactoryDispatcher::GetFileBackedBlobFactory()
{
    auto* execution_context = GetExecutionContext();
    if (!execution_context) {
        return nullptr;
    }
#if defined(MINI_UI_QUICKJS)
    auto resources = mini_ui::GetLocalResources(*execution_context);
    if (!resources) {
        native_factory_.reset();
        native_resources_.reset();
        return nullptr;
    }
    if (!native_factory_.is_bound() || native_resources_ != resources) {
        native_factory_.reset();
        native_resources_ = std::move(resources);
        native_factory_.Bind(
            BlobDataHandle::GetNativeFileBackedBlobFactoryRemote(
                native_resources_));
    }
    return native_factory_.get();
#else

    if (auto* window = DynamicTo<LocalDOMWindow>(execution_context)) {
        if (auto* frame = window->GetFrame()) {
            return blink::GetFileBackedBlobFactory(frame_remote_, execution_context, frame);
        } else {
            return nullptr;
        }
    }
    return blink::GetFileBackedBlobFactory(worker_remote_, execution_context);
#endif
}

// static
const char FileBackedBlobFactoryDispatcher::kSupplementName[] = "FileBackedBlobFactoryDispatcher";

} // namespace blink
