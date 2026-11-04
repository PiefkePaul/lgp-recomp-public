// TLS client over a connected TCP stream (ILan::connect_tls, lan_tls.cpp).
#pragma once

#include <memory>
#include <string>

#include "lgp/platform/lan.hpp"

namespace lgp::platform {

// Runs the TLS handshake with `host` (SNI and certificate name) over tcp
// within timeout_ms and checks the server's certificate: against ca_file
// (PEM) when given, else against the system's roots (Apple: its trust
// store; Windows: the ROOT store; Linux, Android: the CA files and folders).
// Returns the encrypted stream, or nullptr with the reason in *error.
std::unique_ptr<ILanStream> tls_client(std::unique_ptr<ILanStream> tcp, const std::string& host, int timeout_ms,
                                       const std::string& ca_file, std::string* error);

}  // namespace lgp::platform
