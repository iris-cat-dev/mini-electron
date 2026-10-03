// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/browser/devtools_gateway.h"

#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/unguessable_token.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/server/http_server_request_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "runtime/electron/browser/api/web_contents.h"

namespace atom {
namespace {

constexpr net::NetworkTrafficAnnotationTag kDevToolsTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("mini_electron_devtools_gateway", R"(
      semantics {
        sender: "mini-electron DevTools Gateway"
        description:
          "Carries Chrome DevTools Protocol messages between the bundled "
          "Chromium DevTools frontend and the WebContents selected by the user."
        trigger: "The application opens DevTools for a WebContents."
        data: "Chrome DevTools Protocol commands, responses, and events."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "This endpoint exists only while DevTools is open."
        policy_exception_justification:
          "The random capability endpoint binds only to IPv4 loopback."
      })");

}  // namespace

DevToolsGateway::DevToolsGateway(WebContents* contents) : contents_(contents) {}

DevToolsGateway::~DevToolsGateway() {
  Stop();
}

bool DevToolsGateway::Start(std::string* frontend_url, std::string* error) {
  if (server_) {
    *frontend_url = frontend_url_;
    return true;
  }
  if (!contents_ || !contents_->isAlive() || !contents_->m_renderer) {
    *error = "DevTools target is unavailable";
    return false;
  }
  if (contents_->m_debugger_attached) {
    *error = "WebContents debugger is already attached";
    return false;
  }

  auto socket = std::make_unique<net::TCPServerSocket>(
      nullptr, net::NetLogSource());
  int listen_result =
      socket->Listen(
          net::IPEndPoint(net::IPAddress::IPv4Localhost(), 0), 1,
          std::nullopt);
  if (listen_result != net::OK) {
    *error = "Could not bind the DevTools loopback endpoint";
    return false;
  }

  auto server = std::make_unique<net::HttpServer>(std::move(socket), this);
  net::IPEndPoint endpoint;
  if (server->GetLocalAddress(&endpoint) != net::OK) {
    *error = "Could not query the DevTools loopback endpoint";
    return false;
  }

  websocket_path_ = "/devtools/page/" +
      base::UnguessableToken::Create().ToString();
  frontend_url_ = "devtools://devtools/bundled/devtools_app.html?ws="
      "127.0.0.1:" + std::to_string(endpoint.port()) + websocket_path_;

  base::Value::Dict attach;
  attach.Set("protocolVersion", "1.3");
  if (!contents_->m_renderer->Send("cdp.attach", std::move(attach))) {
    frontend_url_.clear();
    websocket_path_.clear();
    *error = "Could not attach the DevTools target";
    return false;
  }

  contents_->m_debugger_attached = true;
  owns_attachment_ = true;
  server_ = std::move(server);
  *frontend_url = frontend_url_;
  return true;
}

void DevToolsGateway::Stop() {
  weak_factory_.InvalidateWeakPtrs();
  if (server_ && websocket_connection_)
    server_->Close(websocket_connection_);
  websocket_connection_ = 0;
  server_.reset();
  frontend_url_.clear();
  websocket_path_.clear();

  if (owns_attachment_ && contents_ && contents_->m_renderer &&
      contents_->isAlive()) {
    contents_->m_renderer->Send("cdp.detach", {});
    contents_->m_debugger_attached = false;
  }
  owns_attachment_ = false;
}

void DevToolsGateway::ForwardProtocolMessage(
    const std::string& method,
    const base::Value::Dict& params) {
  if (!server_ || !websocket_connection_)
    return;
  base::Value::Dict message;
  message.Set("method", method);
  message.Set("params", params.Clone());
  SendJson(websocket_connection_, std::move(message));
}

void DevToolsGateway::OnProtocolDetached(const std::string& reason) {
  owns_attachment_ = false;
  if (server_ && websocket_connection_) {
    base::Value::Dict params;
    params.Set("reason", reason);
    base::Value::Dict message;
    message.Set("method", "Inspector.detached");
    message.Set("params", std::move(params));
    SendJson(websocket_connection_, std::move(message));
    server_->Close(websocket_connection_);
  }
  websocket_connection_ = 0;
}

void DevToolsGateway::OnConnect(int) {}

void DevToolsGateway::OnHttpRequest(
    int connection_id,
    const net::HttpServerRequestInfo&) {
  server_->Send404(connection_id, kDevToolsTrafficAnnotation);
}

void DevToolsGateway::OnWebSocketRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  if (info.path != websocket_path_ || websocket_connection_) {
    server_->Close(connection_id);
    return;
  }
  websocket_connection_ = connection_id;
  server_->AcceptWebSocket(connection_id, info, kDevToolsTrafficAnnotation);
}

void DevToolsGateway::OnWebSocketMessage(int connection_id,
                                         std::string data) {
  if (connection_id != websocket_connection_ || !contents_ ||
      !contents_->m_renderer)
    return;

  std::optional<base::Value> parsed = base::JSONReader::Read(data);
  if (!parsed || !parsed->is_dict())
    return;
  const base::Value::Dict& command = parsed->GetDict();
  std::optional<int> id = command.FindInt("id");
  if (!id) {
    if (std::optional<double> numeric_id = command.FindDouble("id"))
      id = static_cast<int>(*numeric_id);
  }
  const std::string* method = command.FindString("method");
  if (!id || *id <= 0 || !method)
    return;

  base::Value::Dict params;
  if (const base::Value::Dict* value = command.FindDict("params"))
    params = value->Clone();
  base::Value::Dict request;
  request.Set("method", *method);
  request.Set("params", std::move(params));
  if (!contents_->m_renderer->Request(
          "cdp.dispatch", std::move(request),
          [weak = weak_factory_.GetWeakPtr(), connection_id,
           command_id = *id](base::Value::Dict result,
                             std::string error) mutable {
            if (weak)
              weak->SendResponse(connection_id, command_id,
                                 std::move(result), std::move(error));
          })) {
    SendResponse(connection_id, *id, {},
                 "DevTools command could not be queued");
  }
}

void DevToolsGateway::OnClose(int connection_id) {
  if (connection_id == websocket_connection_)
    websocket_connection_ = 0;
}

void DevToolsGateway::SendResponse(int connection_id,
                                   int command_id,
                                   base::Value::Dict result,
                                   std::string error) {
  if (!server_ || connection_id != websocket_connection_)
    return;
  base::Value::Dict response;
  response.Set("id", command_id);
  if (error.empty()) {
    response.Set("result", std::move(result));
  } else {
    if (!result.Find("code"))
      result.Set("code", -32000);
    if (!result.FindString("message"))
      result.Set("message", error);
    response.Set("error", std::move(result));
  }
  SendJson(connection_id, std::move(response));
}

void DevToolsGateway::SendJson(int connection_id,
                               base::Value::Dict message) {
  std::string json;
  if (server_ && base::JSONWriter::Write(message, &json)) {
    server_->SendOverWebSocket(connection_id, json,
                               kDevToolsTrafficAnnotation);
  }
}

}  // namespace atom
