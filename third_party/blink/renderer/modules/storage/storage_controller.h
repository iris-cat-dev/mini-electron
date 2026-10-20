// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_STORAGE_STORAGE_CONTROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_STORAGE_STORAGE_CONTROLLER_H_

#include "base/functional/callback.h"
#include "base/sequence_checker.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/common/dom_storage/session_storage_namespace_id.h"
#include "third_party/blink/public/mojom/dom_storage/dom_storage.mojom-blink.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/modules/storage/storage_area.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_set.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace blink {

class CachedStorageArea;
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS) || !defined(_WIN32)
class InspectorDOMStorageAgent;
#endif
class LocalDOMWindow;
class LocalFrame;
class StorageNamespace;

// Singleton that manages the creation and accounting of DOMStorage objects.
// SessionStorage namespaces are tracked weakly by namespace id. LocalStorage
// namespaces are owned per Page and tracked weakly here for accounting and
// connection resets.
//
// The total cache size is exposed with `TotalCacheSize()`, and
// `ClearAreasIfNeeded()` clears unreferenced cache areas when the limit is
// exceeded.
class MODULES_EXPORT StorageController : public mojom::blink::DomStorageClient {
    USING_FAST_MALLOC(StorageController);

public:
    // Returns the one global StorageController instance.
    static StorageController* GetInstance();

    static bool CanAccessStorageArea(LocalFrame* frame, StorageArea::StorageType type);

    // Visible for testing.
    struct DomStorageConnection {
        mojo::Remote<mojom::blink::DomStorage> dom_storage_remote;
        mojo::PendingReceiver<mojom::blink::DomStorageClient> client_receiver;
    };
    StorageController(DomStorageConnection connection, size_t total_cache_limit);

    // Creates a MakeGarbageCollected<StorageNamespace> for Session storage, and
    // holds a weak reference for accounting & clearing. If there is already a
    // StorageNamespace created for the given id, it is returned.
    StorageNamespace* CreateSessionStorageNamespace(Page& page, const String& namespace_id);

    // Returns the total size of all cached areas in namespaces this controller
    // knows of.
    size_t TotalCacheSize() const;

    // Cleans up unused areas if the total cache size is over the cache limit.
    void ClearAreasIfNeeded();

    // Delegates LocalStorage access to the namespace owned by the window's Page.
    scoped_refptr<CachedStorageArea> GetLocalStorageArea(LocalDOMWindow* local_dom_window,
        mojo::PendingRemote<mojom::blink::StorageArea> local_storage_area = {},
        StorageNamespace::StorageContext context = StorageNamespace::StorageContext::kStandard);
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS) || !defined(_WIN32)
    void AddLocalStorageInspectorStorageAgent(InspectorDOMStorageAgent* agent);
    void RemoveLocalStorageInspectorStorageAgent(InspectorDOMStorageAgent* agent);
#endif

    mojom::blink::DomStorage* dom_storage() const
    {
        return dom_storage_remote_.get();
    }

private:
    StorageNamespace* GetOrCreateLocalStorageNamespace(LocalDOMWindow*);

    // mojom::blink::DomStorageClient:
    void ResetStorageAreaAndNamespaceConnections() override;

    Persistent<HeapHashMap<String, WeakMember<StorageNamespace>>> namespaces_;
    Persistent<HeapHashSet<WeakMember<StorageNamespace>>> local_storage_namespaces_;
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS) || !defined(_WIN32)
    Persistent<HeapHashSet<WeakMember<InspectorDOMStorageAgent>>> local_storage_inspector_agents_;
#endif
    size_t total_cache_limit_;

    mojo::Remote<mojom::blink::DomStorage> dom_storage_remote_;
    mojo::Receiver<mojom::blink::DomStorageClient> dom_storage_client_receiver_ { this };

    SEQUENCE_CHECKER(sequence_checker_);
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_MODULES_STORAGE_STORAGE_CONTROLLER_H_
