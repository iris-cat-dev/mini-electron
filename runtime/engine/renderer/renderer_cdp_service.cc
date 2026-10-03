// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/engine/renderer/renderer_cdp_service.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/stringprintf.h"
#include "ui/gfx/geometry/point.h"
#include "runtime/engine/browser/web_view_host.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/core/exported/web_dev_tools_agent_impl.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/inspector_protocol/crdtp/json.h"
#include "third_party/inspector_protocol/crdtp/span.h"

#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
#error "RendererCdpService requires the real Blink DevTools implementation"
#endif

namespace content {

namespace {

constexpr char kProtocolVersion[] = "1.3";
constexpr char kTargetClosedError[] = "Debugger target closed";

std::string ProtocolErrorMessage(const base::Value::Dict& error) {
  const std::string* message = error.FindString("message");
  std::string text = message ? *message : "DevTools protocol error";
  if (std::optional<int> code = error.FindInt("code")) {
    text = base::StringPrintf("%s (%d)", text.c_str(), *code);
  }
  return text;
}

base::Value::Dict LocalProtocolError(const std::string& message) {
  base::Value::Dict error;
  error.Set("code", -32000);
  error.Set("message", message);
  return error;
}

}  // namespace

RendererCdpService::RendererCdpService(
    WebViewHost* web_view_host,
    EventCallback event_callback,
    DetachedCallback detached_callback)
    : web_view_host_(web_view_host),
      event_callback_(std::move(event_callback)),
      detached_callback_(std::move(detached_callback)) {}

RendererCdpService::~RendererCdpService() {
  // Do not publish a detach event while the renderer itself is tearing down;
  // RendererServer owns the process-lifecycle notification.
  detached_callback_ = {};
  Detach(kTargetClosedError);
  agent_host_receiver_.reset();
  agent_remote_.reset();
}

void RendererCdpService::SetUploadBroker(UploadBroker upload_broker) {
  upload_broker_ = std::move(upload_broker);
}

bool RendererCdpService::EnsureAgent(std::string* error) {
  if (agent_remote_.is_bound()) {
    return true;
  }
  if (!web_view_host_) {
    *error = "Debugger target is unavailable";
    return false;
  }

  blink::WebView* web_view = web_view_host_->getWebView();
  if (!web_view || !web_view->MainFrame() ||
      !web_view->MainFrame()->IsWebLocalFrame()) {
    *error = "Debugger target has no local main frame";
    return false;
  }

  auto* frame = static_cast<blink::WebLocalFrameImpl*>(
      web_view->MainFrame()->ToWebLocalFrame());
  frame = frame->LocalRoot();
  blink::WebDevToolsAgentImpl* agent =
      frame->DevToolsAgentImpl(/*create_if_necessary=*/true);
  if (!agent) {
    *error = "Blink DevTools agent is unavailable";
    return false;
  }

  mojo::PendingRemote<blink::mojom::blink::DevToolsAgent> pending_agent;
  mojo::PendingReceiver<blink::mojom::blink::DevToolsAgent> agent_receiver =
      pending_agent.InitWithNewPipeAndPassReceiver();
  mojo::PendingRemote<blink::mojom::blink::DevToolsAgentHost> pending_host;
  agent_host_receiver_.Bind(pending_host.InitWithNewPipeAndPassReceiver());
  agent->BindReceiver(std::move(pending_host), std::move(agent_receiver));
  agent_remote_.Bind(std::move(pending_agent));
  agent_remote_.set_disconnect_handler(
      base::BindOnce(&RendererCdpService::OnAgentDisconnected,
                     weak_factory_.GetWeakPtr()));
  return true;
}

bool RendererCdpService::Attach(const std::string& protocol_version,
                                std::string* error) {
  if (attached_) {
    *error = "Debugger is already attached";
    return false;
  }
  if (!protocol_version.empty() && protocol_version != kProtocolVersion) {
    *error = "Unsupported debugger protocol version: " + protocol_version;
    return false;
  }
  if (!EnsureAgent(error)) {
    return false;
  }

  mojo::PendingAssociatedRemote<blink::mojom::blink::DevToolsSessionHost>
      pending_host;
  session_host_receiver_.Bind(
      pending_host.InitWithNewEndpointAndPassReceiver());

  mojo::PendingAssociatedRemote<blink::mojom::blink::DevToolsSession>
      pending_session;
  mojo::PendingAssociatedReceiver<blink::mojom::blink::DevToolsSession>
      session_receiver =
          pending_session.InitWithNewEndpointAndPassReceiver();
  mojo::PendingRemote<blink::mojom::blink::DevToolsSession> pending_io_session;
  mojo::PendingReceiver<blink::mojom::blink::DevToolsSession>
      io_session_receiver = pending_io_session.InitWithNewPipeAndPassReceiver();

  agent_remote_->AttachDevToolsSession(
      std::move(pending_host), std::move(session_receiver),
      std::move(io_session_receiver), nullptr,
      /*client_expects_binary_responses=*/false,
      /*client_is_trusted=*/true, WTF::String(),
      /*session_waits_for_debugger=*/false);

  session_remote_.Bind(std::move(pending_session));
  io_session_remote_.Bind(std::move(pending_io_session));
  session_remote_.set_disconnect_handler(
      base::BindOnce(&RendererCdpService::OnSessionDisconnected,
                     weak_factory_.GetWeakPtr()));
  attached_ = true;
  return true;
}

void RendererCdpService::Detach(const std::string& reason) {
  if (!attached_ || detaching_) {
    return;
  }
  detaching_ = true;
  attached_ = false;
  RejectPending(reason.empty() ? "Debugger detached" : reason);

  // Closing the host endpoint is the canonical Blink detach operation. It runs
  // DevToolsSession::Detach(), disposes every domain agent and restores state.
  session_host_receiver_.reset();
  session_remote_.reset();
  io_session_remote_.reset();

  detaching_ = false;
  if (detached_callback_) {
    detached_callback_(reason);
  }
}

bool RendererCdpService::InspectElement(int x, int y, std::string* error) {
  if (!attached_ || !agent_remote_.is_bound()) {
    *error = "Debugger is not attached";
    return false;
  }
  agent_remote_->InspectElement(gfx::Point(x, y));
  return true;
}

bool RendererCdpService::Dispatch(int32_t command_id,
                                  const std::string& method,
                                  base::Value::Dict params,
                                  CommandCallback callback,
                                  std::string* error) {
  if (!attached_ || !session_remote_.is_bound()) {
    *error = "Debugger is not attached";
    return false;
  }
  if (command_id <= 0) {
    *error = "DevTools command id must be positive";
    return false;
  }
  if (method.empty() || method.find('.') == std::string::npos) {
    *error = "Invalid DevTools protocol method";
    return false;
  }
  if (!callback) {
    *error = "DevTools command callback is required";
    return false;
  }
  if (pending_commands_.contains(command_id)) {
    *error = "Duplicate DevTools command id";
    return false;
  }

  pending_commands_.emplace(command_id, std::move(callback));

  if (method == "DOM.setFileInputFiles") {
    const base::Value::List* files = params.FindList("files");
    if (files) {
      std::vector<std::string> requested_paths;
      requested_paths.reserve(files->size());
      for (const base::Value& file : *files) {
        if (!file.is_string()) {
          return DispatchEncoded(command_id, method, std::move(params), error);
        }
        requested_paths.push_back(file.GetString());
      }

      if (!upload_broker_) {
        RejectCommand(
            command_id,
            "DOM.setFileInputFiles requires the sandbox upload broker");
        return true;
      }

      auto authorized_params =
          std::make_shared<base::Value::Dict>(std::move(params));
      upload_broker_(
          std::move(requested_paths),
          [weak = weak_factory_.GetWeakPtr(), command_id, method,
           authorized_params](std::vector<std::string> broker_urls,
                              std::string broker_error) mutable {
            if (weak) {
              weak->FinishUploadAuthorization(
                  command_id, method, std::move(*authorized_params),
                  std::move(broker_urls), std::move(broker_error));
            }
          });
      return true;
    }
  }

  return DispatchEncoded(command_id, method, std::move(params), error);
}

bool RendererCdpService::DispatchEncoded(int32_t command_id,
                                         const std::string& method,
                                         base::Value::Dict params,
                                         std::string* error) {
  base::Value::Dict command;
  command.Set("id", command_id);
  command.Set("method", method);
  command.Set("params", std::move(params));
  std::string json;
  if (!base::JSONWriter::Write(command, &json)) {
    *error = "Could not serialize DevTools command";
    RejectCommand(command_id, *error);
    return true;
  }

  std::vector<uint8_t> cbor;
  crdtp::Status status =
      crdtp::json::ConvertJSONToCBOR(crdtp::SpanFrom(json), &cbor);
  if (!status.ok()) {
    *error = "Could not encode DevTools command: " + status.ToASCIIString();
    RejectCommand(command_id, *error);
    return true;
  }

  session_remote_->DispatchProtocolCommand(
      command_id, WTF::String::FromUTF8(method),
      base::span<const uint8_t>(cbor.data(), cbor.size()));
  return true;
}

void RendererCdpService::FinishUploadAuthorization(
    int32_t command_id,
    std::string method,
    base::Value::Dict params,
    std::vector<std::string> broker_urls,
    std::string broker_error) {
  if (!pending_commands_.contains(command_id))
    return;

  if (!broker_error.empty()) {
    RejectCommand(command_id, broker_error);
    return;
  }

  const base::Value::List* requested_files = params.FindList("files");
  if (!requested_files || requested_files->size() != broker_urls.size()) {
    RejectCommand(command_id, "Upload broker returned an invalid file list");
    return;
  }

  base::Value::List authorized_files;
  authorized_files.reserve(broker_urls.size());
  for (std::string& broker_url : broker_urls) {
    constexpr char kBrokerScheme[] = "mini-electron-broker://";
    if (broker_url.size() <= sizeof(kBrokerScheme) - 1 ||
        broker_url.rfind(kBrokerScheme, 0) != 0) {
      RejectCommand(command_id,
                    "Upload broker returned a non-broker file path");
      return;
    }
    authorized_files.Append(std::move(broker_url));
  }
  params.Set("files", std::move(authorized_files));

  std::string dispatch_error;
  DispatchEncoded(command_id, method, std::move(params), &dispatch_error);
}

void RendererCdpService::RejectCommand(int32_t command_id,
                                       const std::string& error) {
  auto pending = pending_commands_.find(command_id);
  if (pending == pending_commands_.end())
    return;
  CommandCallback callback = std::move(pending->second);
  pending_commands_.erase(pending);
  callback(LocalProtocolError(error), error);
}

void RendererCdpService::ChildTargetCreated(
    mojo::PendingRemote<blink::mojom::blink::DevToolsAgent>,
    mojo::PendingReceiver<blink::mojom::blink::DevToolsAgentHost>,
    const blink::KURL&,
    const WTF::String&,
    const base::UnguessableToken&,
    bool,
    blink::mojom::blink::DevToolsExecutionContextType) {
  // Electron's WebContents debugger is scoped to its page target.
}

void RendererCdpService::MainThreadDebuggerPaused() {}

void RendererCdpService::MainThreadDebuggerResumed() {}

void RendererCdpService::BringToForeground() {}

std::optional<base::Value::Dict> RendererCdpService::ParseMessage(
    const blink::mojom::blink::DevToolsMessage& message) {
  std::string json(reinterpret_cast<const char*>(message.data.data()),
                   message.data.size());
  std::optional<base::Value> parsed = base::JSONReader::Read(json);
  if (!parsed || !parsed->is_dict()) {
    return std::nullopt;
  }
  return std::move(*parsed).TakeDict();
}

void RendererCdpService::DispatchProtocolResponse(
    blink::mojom::blink::DevToolsMessagePtr message,
    int32_t call_id,
    blink::mojom::blink::DevToolsSessionStatePtr) {
  auto pending = pending_commands_.find(call_id);
  if (pending == pending_commands_.end()) {
    return;
  }
  CommandCallback callback = std::move(pending->second);
  pending_commands_.erase(pending);

  if (!message) {
    constexpr char kError[] = "Empty DevTools protocol response";
    callback(LocalProtocolError(kError), kError);
    return;
  }
  std::optional<base::Value::Dict> response = ParseMessage(*message);
  if (!response) {
    constexpr char kError[] = "Invalid DevTools protocol response";
    callback(LocalProtocolError(kError), kError);
    return;
  }
  if (const base::Value::Dict* protocol_error = response->FindDict("error")) {
    base::Value::Dict error_copy = protocol_error->Clone();
    callback(std::move(error_copy), ProtocolErrorMessage(*protocol_error));
    return;
  }
  const base::Value::Dict* result = response->FindDict("result");
  callback(result ? result->Clone() : base::Value::Dict(), std::string());
}

void RendererCdpService::DispatchProtocolNotification(
    blink::mojom::blink::DevToolsMessagePtr message,
    blink::mojom::blink::DevToolsSessionStatePtr) {
  if (!attached_ || !message || !event_callback_) {
    return;
  }
  std::optional<base::Value::Dict> notification = ParseMessage(*message);
  if (!notification) {
    return;
  }
  const std::string* method = notification->FindString("method");
  if (!method) {
    return;
  }
  const base::Value::Dict* params = notification->FindDict("params");
  event_callback_(*method,
                  params ? params->Clone() : base::Value::Dict());
}

void RendererCdpService::OnAgentDisconnected() {
  agent_remote_.reset();
  agent_host_receiver_.reset();
  Detach(kTargetClosedError);
}

void RendererCdpService::OnSessionDisconnected() {
  Detach("DevTools session closed");
}

void RendererCdpService::RejectPending(const std::string& error) {
  std::map<int32_t, CommandCallback> pending;
  pending.swap(pending_commands_);
  for (auto& entry : pending) {
    entry.second(LocalProtocolError(error), error);
  }
}

}  // namespace content
