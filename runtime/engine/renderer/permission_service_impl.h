// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef CONTENT_RENDERER_PERMISSION_SERVICE_IMPL_H_
#define CONTENT_RENDERER_PERMISSION_SERVICE_IMPL_H_

#include <memory>
#include <utility>

#include "gen/third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "runtime/engine/renderer/renderer_permission_broker.h"

class PermissionServiceImpl : public blink::mojom::blink::PermissionService {
public:
    void HasPermission(blink::mojom::blink::PermissionDescriptorPtr permission,
        HasPermissionCallback callback) override
    {
        Query(std::move(permission), false, false, std::move(callback));
    }

    void RegisterPageEmbeddedPermissionControl(
        WTF::Vector<blink::mojom::blink::PermissionDescriptorPtr>,
        mojo::PendingRemote<blink::mojom::blink::EmbeddedPermissionControlClient>) override
    {
    }

    void RequestPageEmbeddedPermission(
        blink::mojom::blink::EmbeddedPermissionRequestDescriptorPtr,
        RequestPageEmbeddedPermissionCallback callback) override
    {
        std::move(callback).Run(
            blink::mojom::blink::EmbeddedPermissionControlResult::kNotSupported);
    }

    void RequestPermission(
        blink::mojom::blink::PermissionDescriptorPtr permission,
        bool user_gesture, RequestPermissionCallback callback) override
    {
        Query(std::move(permission), user_gesture, true, std::move(callback));
    }

    void RequestPermissions(
        WTF::Vector<blink::mojom::blink::PermissionDescriptorPtr> permissions,
        bool user_gesture, RequestPermissionsCallback callback) override
    {
        if (permissions.empty()) {
            std::move(callback).Run({});
            return;
        }
        struct State {
            WTF::Vector<blink::mojom::blink::PermissionStatus> statuses;
            size_t remaining = 0;
            RequestPermissionsCallback callback;
        };
        auto state = std::make_shared<State>();
        state->statuses.resize(permissions.size());
        state->remaining = permissions.size();
        state->callback = std::move(callback);
        for (wtf_size_t index = 0; index < permissions.size(); ++index) {
            const int name = static_cast<int>(permissions[index]->name);
            content::RequestRendererPermission(
                content::RendererPermissionName(name), user_gesture, true,
                [state, index](bool granted) mutable {
                    state->statuses[index] = granted
                        ? blink::mojom::blink::PermissionStatus::GRANTED
                        : blink::mojom::blink::PermissionStatus::DENIED;
                    if (--state->remaining == 0)
                        std::move(state->callback).Run(state->statuses);
                });
        }
    }

    void RevokePermission(blink::mojom::blink::PermissionDescriptorPtr,
        RevokePermissionCallback callback) override
    {
        std::move(callback).Run(blink::mojom::blink::PermissionStatus::DENIED);
    }

    void AddPermissionObserver(
        blink::mojom::blink::PermissionDescriptorPtr,
        blink::mojom::blink::PermissionStatus,
        mojo::PendingRemote<blink::mojom::blink::PermissionObserver>) override
    {
    }

    void AddPageEmbeddedPermissionObserver(
        blink::mojom::blink::PermissionDescriptorPtr,
        blink::mojom::blink::PermissionStatus,
        mojo::PendingRemote<blink::mojom::blink::PermissionObserver>) override
    {
    }

    void NotifyEventListener(blink::mojom::blink::PermissionDescriptorPtr,
        const WTF::String&, bool) override
    {
    }

private:
    template <typename Callback>
    void Query(blink::mojom::blink::PermissionDescriptorPtr permission,
        bool user_gesture, bool is_request, Callback callback)
    {
        if (!permission) {
            std::move(callback).Run(
                blink::mojom::blink::PermissionStatus::DENIED);
            return;
        }
        auto saved = std::make_shared<Callback>(std::move(callback));
        content::RequestRendererPermission(
            content::RendererPermissionName(static_cast<int>(permission->name)),
            user_gesture, is_request,
            [saved](bool granted) mutable {
                std::move(*saved).Run(granted
                    ? blink::mojom::blink::PermissionStatus::GRANTED
                    : blink::mojom::blink::PermissionStatus::DENIED);
            });
    }
};

#endif // CONTENT_RENDERER_PERMISSION_SERVICE_IMPL_H_
