// The server certificate check of the TLS client (os/src/lan_tls.cpp) on
// Apple platforms: Mbed TLS cannot read the iOS/tvOS trust store, so the
// chain from the handshake goes to the system's trust evaluation.
#pragma once

#include <string>
#include <vector>

namespace lgp::platform {

// chain: DER certificates as the server sent them, leaf first. True when the
// system trusts the chain for host (SSL policy); else the reason in *error.
bool apple_trust_chain(const std::vector<std::vector<unsigned char>>& chain, const std::string& host,
                       std::string* error);

}  // namespace lgp::platform
