// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_RENDERER_CDP_SERVICE_H_
#define MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_RENDERER_CDP_SERVICE_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/devtools/devtools_agent.mojom-blink.h"

namespace content {

class WebViewHost;

// Owns the renderer endpoint of one Electron debugger session. Every method,
// callback, and Mojo endpoint is sequence-affine to the Blink main thread.
// Only JSON-compatible base::Value objects leave this class; V8 and Blink
// objects never cross the renderer process boundary.
class RendererCdpService final
    : public blink::mojom::blink::DevToolsAgentHost,
      public blink::mojom::blink::DevToolsSessionHost {
 public:
  using CommandCallback =
      std::function<void(base::Value::Dict result, std::string error)>;
  using EventCallback =
      std::function<void(std::string method, base::Value::Dict params)>;
  using DetachedCallback = std::function<void(std::string reason)>;
  using UploadBrokerReply =
      std::function<void(std::vector<std::string> broker_urls,
                         std::string error)>;
  using UploadBroker =
      std::function<void(std::vector<std::string> requested_paths,
                         UploadBrokerReply reply)>;

  RendererCdpService(WebViewHost* web_view_host,
                     EventCallback event_callback,
                     DetachedCallback detached_callback);
  RendererCdpService(const RendererCdpService&) = delete;
  RendererCdpService& operator=(const RendererCdpService&) = delete;
  ~RendererCdpService() override;

  // The Electron debugger protocol version is the stable CDP 1.3 schema.
  // Passing an empty version selects that schema, matching Electron.
  bool Attach(const std::string& protocol_version, std::string* error);
  void Detach(const std::string& reason);
  bool IsAttached() const { return attached_; }
  bool InspectElement(int x, int y, std::string* error);

  // Required for DOM.setFileInputFiles in the sandboxed renderer. The broker
  // must authorize paths in the owning WebContents and return opaque
  // mini-electron-broker URLs; direct host paths are never sent to Blink.
  void SetUploadBroker(UploadBroker upload_broker);

  // Dispatches an arbitrary protocol method. The callback receives the CDP
  // result object, or the complete CDP error object plus its message in error.
  bool Dispatch(int32_t command_id,
                const std::string& method,
                base::Value::Dict params,
                CommandCallback callback,
                std::string* error);

 private:
  // blink::mojom::blink::DevToolsAgentHost:
  void ChildTargetCreated(
      mojo::PendingRemote<blink::mojom::blink::DevToolsAgent>
          worker_devtools_agent,
      mojo::PendingReceiver<blink::mojom::blink::DevToolsAgentHost>
          worker_devtools_agent_host,
      const blink::KURL& url,
      const WTF::String& name,
      const base::UnguessableToken& devtools_worker_token,
      bool waiting_for_debugger,
      blink::mojom::blink::DevToolsExecutionContextType context_type) override;
  void MainThreadDebuggerPaused() override;
  void MainThreadDebuggerResumed() override;
  void BringToForeground() override;

  // blink::mojom::blink::DevToolsSessionHost:
  void DispatchProtocolResponse(
      blink::mojom::blink::DevToolsMessagePtr message,
      int32_t call_id,
      blink::mojom::blink::DevToolsSessionStatePtr updates) override;
  void DispatchProtocolNotification(
      blink::mojom::blink::DevToolsMessagePtr message,
      blink::mojom::blink::DevToolsSessionStatePtr updates) override;

  bool EnsureAgent(std::string* error);
  bool DispatchEncoded(int32_t command_id,
                       const std::string& method,
                       base::Value::Dict params,
                       std::string* error);
  void FinishUploadAuthorization(int32_t command_id,
                                 std::string method,
                                 base::Value::Dict params,
                                 std::vector<std::string> broker_urls,
                                 std::string broker_error);
  void RejectCommand(int32_t command_id, const std::string& error);
  void OnAgentDisconnected();
  void OnSessionDisconnected();
  void RejectPending(const std::string& error);
  static std::optional<base::Value::Dict> ParseMessage(
      const blink::mojom::blink::DevToolsMessage& message);

  WebViewHost* const web_view_host_;
  EventCallback event_callback_;
  DetachedCallback detached_callback_;
  UploadBroker upload_broker_;
  bool attached_ = false;
  bool detaching_ = false;

  mojo::Remote<blink::mojom::blink::DevToolsAgent> agent_remote_;
  mojo::Receiver<blink::mojom::blink::DevToolsAgentHost>
      agent_host_receiver_{this};
  mojo::AssociatedRemote<blink::mojom::blink::DevToolsSession>
      session_remote_;
  mojo::Remote<blink::mojom::blink::DevToolsSession> io_session_remote_;
  mojo::AssociatedReceiver<blink::mojom::blink::DevToolsSessionHost>
      session_host_receiver_{this};

  std::map<int32_t, CommandCallback> pending_commands_;
  base::WeakPtrFactory<RendererCdpService> weak_factory_{this};
};

}  // namespace content

#endif  // MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_RENDERER_CDP_SERVICE_H_
