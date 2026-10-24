// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_BROWSER_DEVTOOLS_GATEWAY_H_
#define RUNTIME_ELECTRON_BROWSER_DEVTOOLS_GATEWAY_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "net/server/http_server.h"

namespace atom {

class WebContents;

// Loopback-only, capability-addressed CDP endpoint consumed by the genuine
// Chromium DevTools frontend. The owning WebContents controls its lifetime.
class DevToolsGateway final : public net::HttpServer::Delegate {
 public:
  explicit DevToolsGateway(WebContents* contents);
  DevToolsGateway(const DevToolsGateway&) = delete;
  DevToolsGateway& operator=(const DevToolsGateway&) = delete;
  ~DevToolsGateway() override;

  bool Start(std::string* frontend_url, std::string* error);
  void Stop();
  bool IsRunning() const { return server_ != nullptr; }
  const std::string& frontend_url() const { return frontend_url_; }

  void ForwardProtocolMessage(const std::string& method,
                              const base::Value::Dict& params);
  void OnProtocolDetached(const std::string& reason);

 private:
  // net::HttpServer::Delegate:
  void OnConnect(int connection_id) override;
  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override;
  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override;
  void OnWebSocketMessage(int connection_id, std::string data) override;
  void OnClose(int connection_id) override;

  void SendResponse(int connection_id,
                    int command_id,
                    base::Value::Dict result,
                    std::string error);
  void SendJson(int connection_id, base::Value::Dict message);

  WebContents* const contents_;
  std::unique_ptr<net::HttpServer> server_;
  int websocket_connection_ = 0;
  std::string websocket_path_;
  std::string frontend_url_;
  bool owns_attachment_ = false;
  base::WeakPtrFactory<DevToolsGateway> weak_factory_{this};
};

}  // namespace atom

#endif  // RUNTIME_ELECTRON_BROWSER_DEVTOOLS_GATEWAY_H_
