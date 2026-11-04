// WebSocket client stream for RyuLink (docs/knowledge/ldn-ueber-ryuldn.md):
// carries the RyuLDN byte stream as binary WebSocket messages (RFC 6455)
// through a bridge in front of the server, so a Cloudflare tunnel or another
// HTTP proxy can reach it. Plain ws:// only (no TLS); the bridge is
// tools/ldn/ws_bridge.py.
#pragma once

#include <memory>
#include <string>

#include "lgp/platform/lan.hpp"

namespace lgp::hle::ldn {

// Sends the upgrade request over `tcp` and waits up to timeout_ms for the
// server's 101 with the matching Sec-WebSocket-Accept. Returns the stream that
// frames sends and unframes receives (answers pings, ends on close), or
// nullptr with the reason in *error. tls (wss://) only decides the default
// port the Host header leaves out (443 instead of 80).
std::unique_ptr<platform::ILanStream> websocket_connect(std::unique_ptr<platform::ILanStream> tcp,
                                                        const std::string& host, uint16_t port, bool tls,
                                                        const std::string& path, int timeout_ms, std::string* error);

// For the tests: base64 of SHA-1 of key + the RFC 6455 GUID.
std::string websocket_accept(const std::string& key);

}  // namespace lgp::hle::ldn
