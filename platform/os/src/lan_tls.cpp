// TLS for wss:// to the RyuLDN server (lan_tls.hpp): Mbed TLS 3.6 on top of
// the host's TCP stream, on every platform with sockets. The certificate
// check takes the system's roots: Apple through its trust store
// (apple/src/tls_trust_apple.cpp, as Mbed TLS cannot read the iOS store),
// Windows from the ROOT certificate store, Linux and Android from the usual
// CA files and folders. A ca_file (PEM) replaces them, e.g. for an own CA.
#include "lan_tls.hpp"

#if defined(LGP_HAVE_MBEDTLS)
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif
#if defined(__APPLE__)
#include "../../apple/src/tls_trust_apple.hpp"
#endif
#endif

namespace lgp::platform {

#if defined(LGP_HAVE_MBEDTLS)

bool lan_tls_available() { return true; }

namespace {

constexpr int kErrSend = -0x004E;  // as MBEDTLS_ERR_NET_SEND_FAILED

std::string mbed_error(int code) {
    char text[160];
    mbedtls_strerror(code, text, sizeof(text));
    return text;
}

#if !defined(__APPLE__)  // Apple asks SecTrust instead
// The system's trusted roots into chain; false when none were found.
bool load_system_roots(mbedtls_x509_crt* chain) {
#if defined(_WIN32)
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr)
        return false;
    int n = 0;
    for (PCCERT_CONTEXT c = CertEnumCertificatesInStore(store, nullptr); c != nullptr;
         c = CertEnumCertificatesInStore(store, c)) {
        if (c->dwCertEncodingType == X509_ASN_ENCODING &&
            mbedtls_x509_crt_parse_der(chain, c->pbCertEncoded, c->cbCertEncoded) == 0)
            ++n;
    }
    CertCloseStore(store, 0);
    return n > 0;
#else
    // Debian/Ubuntu, Fedora/RHEL, openSUSE, Alpine/macOS-style bundles; then
    // folders of single certificates (Android: system and the updatable
    // Conscrypt module of Android 14+).
    static const char* const kFiles[] = {
        "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"};
    for (const char* f : kFiles) {
        // parse_file returns the number of certificates it could not parse
        // (> 0) when it read the others; a bundle with a few odd entries is fine.
        if (mbedtls_x509_crt_parse_file(chain, f) >= 0 && chain->raw.len != 0)
            return true;
    }
    static const char* const kDirs[] = {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts",
                                        "/etc/ssl/certs"};
    for (const char* d : kDirs) {
        if (mbedtls_x509_crt_parse_path(chain, d) >= 0 && chain->raw.len != 0)
            return true;
    }
    return false;
#endif
}
#endif

// One TLS connection. The reader thread waits on the TCP stream without the
// lock (wait_readable) and takes it only to decrypt, so a send never waits
// for a quiet server.
class TlsStream final : public ILanStream {
public:
    TlsStream() {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&ca_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
    }
    ~TlsStream() override {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }

    bool start(std::unique_ptr<ILanStream> tcp, const std::string& host, int timeout_ms, const std::string& ca_file,
               std::string* error) {
        tcp_ = std::move(tcp);
        static const bool psa_ok = psa_crypto_init() == PSA_SUCCESS;  // TLS 1.3 needs PSA crypto
        if (!psa_ok)
            return fail(error, "PSA crypto init failed");
        static const char kPers[] = "lgp ldn tls";
        int r = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                                      reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1);
        if (r != 0)
            return fail(error, "random seed: " + mbed_error(r));
        r = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
        if (r != 0)
            return fail(error, "config: " + mbed_error(r));
        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
        bool system_trust = false;  // Apple: the trust store checks after the handshake
        if (!ca_file.empty()) {
            if (mbedtls_x509_crt_parse_file(&ca_, ca_file.c_str()) < 0 || ca_.raw.len == 0)
                return fail(error, "cannot read CA file " + ca_file);
        } else {
#if defined(__APPLE__)
            system_trust = true;
#else
            if (!load_system_roots(&ca_))
                return fail(error, "no trusted root certificates found on this system");
#endif
        }
        if (system_trust) {
            mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
        } else {
            mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
        }
        r = mbedtls_ssl_setup(&ssl_, &conf_);
        if (r != 0)
            return fail(error, "setup: " + mbed_error(r));
        r = mbedtls_ssl_set_hostname(&ssl_, host.c_str());
        if (r != 0)
            return fail(error, "host name: " + mbed_error(r));
        mbedtls_ssl_set_bio(&ssl_, this, &TlsStream::bio_send, nullptr, &TlsStream::bio_recv);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            read_timeout_ms_ = remaining(deadline);
            if (read_timeout_ms_ == 0)
                return fail(error, "TLS handshake with " + host + " timed out");
            r = mbedtls_ssl_handshake(&ssl_);
            if (r == 0)
                break;
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_TIMEOUT)
                continue;
            if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                char why[256];
                mbedtls_x509_crt_verify_info(why, sizeof(why), "", mbedtls_ssl_get_verify_result(&ssl_));
                std::string w = why;
                while (!w.empty() && (w.back() == '\n' || w.back() == ' '))
                    w.pop_back();
                return fail(error, "certificate of " + host + " not trusted: " + w);
            }
            return fail(error, "TLS handshake with " + host + ": " + mbed_error(r));
        }
#if defined(__APPLE__)
        if (system_trust) {
            std::vector<std::vector<unsigned char>> chain;
            for (const mbedtls_x509_crt* c = mbedtls_ssl_get_peer_cert(&ssl_); c != nullptr; c = c->next)
                chain.emplace_back(c->raw.p, c->raw.p + c->raw.len);
            std::string why;
            if (!apple_trust_chain(chain, host, &why))
                return fail(error, "certificate of " + host + " not trusted: " + why);
        }
#endif
        return true;
    }

    bool send_all(const void* data, size_t size) override {
        std::lock_guard lock(mutex_);
        const unsigned char* p = static_cast<const unsigned char*>(data);
        while (size != 0) {
            const int n = mbedtls_ssl_write(&ssl_, p, size);
            if (n > 0) {
                p += n;
                size -= static_cast<size_t>(n);
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            return false;
        }
        return true;
    }

    int recv(void* data, size_t capacity, int timeout_ms) override {
        const auto deadline = timeout_ms < 0 ? std::chrono::steady_clock::time_point::max()
                                             : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard lock(mutex_);
                if (mbedtls_ssl_check_pending(&ssl_) != 0 || mbedtls_ssl_get_bytes_avail(&ssl_) != 0) {
                    const int n = read_locked(data, capacity, 0);
                    if (n != 0)
                        return n;
                }
            }
            const int wait = timeout_ms < 0 ? -1 : remaining(deadline);
            if (wait == 0)
                return 0;
            const int ready = tcp_->wait_readable(wait);
            if (ready < 0)
                return -1;
            if (ready == 0)
                continue;  // the deadline check above returns 0
            std::lock_guard lock(mutex_);
            // A record may still be arriving: wait a little for the rest.
            const int n = read_locked(data, capacity, 1000);
            if (n != 0)
                return n;
        }
    }

    void shutdown() override { tcp_->shutdown(); }

    int wait_readable(int timeout_ms) override {
        {
            std::lock_guard lock(mutex_);
            if (mbedtls_ssl_check_pending(&ssl_) != 0 || mbedtls_ssl_get_bytes_avail(&ssl_) != 0)
                return 1;
        }
        return tcp_->wait_readable(timeout_ms);
    }

private:
    static int remaining(std::chrono::steady_clock::time_point deadline) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        return left <= 0 ? 0 : static_cast<int>(left);
    }

    bool fail(std::string* error, const std::string& text) {
        if (error != nullptr)
            *error = text;
        return false;
    }

    // > 0 bytes, 0 nothing yet (timeout, a handshake message such as a TLS
    // 1.3 session ticket), -1 closed or failed. Holds mutex_.
    int read_locked(void* data, size_t capacity, int timeout_ms) {
        read_timeout_ms_ = timeout_ms;
        const int n = mbedtls_ssl_read(&ssl_, static_cast<unsigned char*>(data), capacity);
        if (n > 0)
            return n;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT ||
            n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            return 0;
        return -1;  // 0 (close notify), MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY or an error
    }

    static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
        return static_cast<TlsStream*>(ctx)->tcp_->send_all(buf, len) ? static_cast<int>(len) : kErrSend;
    }

    static int bio_recv(void* ctx, unsigned char* buf, size_t len, uint32_t /*timeout*/) {
        TlsStream* self = static_cast<TlsStream*>(ctx);
        const int n = self->tcp_->recv(buf, len, self->read_timeout_ms_);
        if (n > 0)
            return n;
        return n == 0 ? MBEDTLS_ERR_SSL_TIMEOUT : 0;  // 0: end of stream
    }

    std::unique_ptr<ILanStream> tcp_;
    std::mutex mutex_;  // the ssl context: sends and the reader's decrypting
    int read_timeout_ms_ = 0;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_x509_crt ca_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
};

}  // namespace

std::unique_ptr<ILanStream> tls_client(std::unique_ptr<ILanStream> tcp, const std::string& host, int timeout_ms,
                                       const std::string& ca_file, std::string* error) {
    auto s = std::make_unique<TlsStream>();
    if (!s->start(std::move(tcp), host, timeout_ms, ca_file, error))
        return nullptr;
    return s;
}

#else

bool lan_tls_available() { return false; }

std::unique_ptr<ILanStream> tls_client(std::unique_ptr<ILanStream>, const std::string&, int, const std::string&,
                                       std::string* error) {
    if (error != nullptr)
        *error = "no TLS in this build (LGP_ENABLE_MBEDTLS off)";
    return nullptr;
}

#endif

}  // namespace lgp::platform
