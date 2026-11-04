// Server certificate check for TLS on Apple platforms (tls_trust_apple.hpp):
// SecTrust with the SSL policy for the host name, i.e. the same roots and
// rules as the system's own TLS (Security framework, C API).
#include "tls_trust_apple.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace lgp::platform {

bool apple_trust_chain(const std::vector<std::vector<unsigned char>>& chain, const std::string& host,
                       std::string* error) {
    if (chain.empty()) {
        *error = "the server sent no certificate";
        return false;
    }
    CFMutableArrayRef certs = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (const std::vector<unsigned char>& der : chain) {
        CFDataRef data = CFDataCreate(nullptr, der.data(), static_cast<CFIndex>(der.size()));
        SecCertificateRef cert = data != nullptr ? SecCertificateCreateWithData(nullptr, data) : nullptr;
        if (data != nullptr)
            CFRelease(data);
        if (cert == nullptr) {
            CFRelease(certs);
            *error = "unreadable certificate";
            return false;
        }
        CFArrayAppendValue(certs, cert);
        CFRelease(cert);
    }
    CFStringRef name = CFStringCreateWithCString(nullptr, host.c_str(), kCFStringEncodingUTF8);
    SecPolicyRef policy = SecPolicyCreateSSL(true, name);
    SecTrustRef trust = nullptr;
    bool ok = SecTrustCreateWithCertificates(certs, policy, &trust) == errSecSuccess && trust != nullptr;
    if (ok) {
        CFErrorRef why = nullptr;
        ok = SecTrustEvaluateWithError(trust, &why);
        if (!ok) {
            *error = "rejected by the system";
            if (why != nullptr) {
                CFStringRef text = CFErrorCopyDescription(why);
                char buf[256];
                if (text != nullptr && CFStringGetCString(text, buf, sizeof(buf), kCFStringEncodingUTF8))
                    *error = buf;
                if (text != nullptr)
                    CFRelease(text);
                CFRelease(why);
            }
        }
    } else {
        *error = "cannot evaluate the certificate";
    }
    if (trust != nullptr)
        CFRelease(trust);
    if (policy != nullptr)
        CFRelease(policy);
    if (name != nullptr)
        CFRelease(name);
    CFRelease(certs);
    return ok;
}

}  // namespace lgp::platform
