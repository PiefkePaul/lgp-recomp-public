// WebSocket client stream (see ldn_ws.hpp). Client frames are masked (RFC
// 6455 5.3); server frames are read masked or not. Data frames (binary, text
// and their continuations) are handed out as one byte stream, which is all
// RyuLink needs: its own header delimits the packets.
#include "ldn_ws.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <vector>

namespace lgp::hle::ldn {
namespace {

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;
constexpr size_t kMaxFrame = 1u << 20;  // larger frames end the connection
constexpr size_t kMaxResponse = 16384;  // upgrade response headers

std::array<uint8_t, 20> sha1(const std::string& text) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> m(text.begin(), text.end());
    const uint64_t bits = static_cast<uint64_t>(m.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56)
        m.push_back(0);
    for (int i = 7; i >= 0; --i)
        m.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    const auto rol = [](uint32_t v, int n) { return v << n | v >> (32 - n); };
    for (size_t block = 0; block < m.size(); block += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t{m[block + i * 4]} << 24 | uint32_t{m[block + i * 4 + 1]} << 16 |
                   uint32_t{m[block + i * 4 + 2]} << 8 | uint32_t{m[block + i * 4 + 3]};
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 20; ++i)
        out[i] = static_cast<uint8_t>(h[i / 4] >> (24 - (i % 4) * 8));
    return out;
}

std::string base64(const uint8_t* data, size_t size) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t v = uint32_t{data[i]} << 16 | (i + 1 < size ? uint32_t{data[i + 1]} << 8 : 0) |
                           (i + 2 < size ? uint32_t{data[i + 2]} : 0);
        out += abc[v >> 18 & 63];
        out += abc[v >> 12 & 63];
        out += i + 1 < size ? abc[v >> 6 & 63] : '=';
        out += i + 2 < size ? abc[v & 63] : '=';
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// The value of header `name` (lowercase) in a response, "" if missing.
std::string header(const std::string& response, const std::string& name) {
    size_t at = response.find("\r\n");
    while (at != std::string::npos && at + 2 < response.size()) {
        const size_t end = response.find("\r\n", at + 2);
        const std::string line = response.substr(at + 2, end - at - 2);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && lower(line.substr(0, colon)) == name) {
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                ++v;
            size_t e = line.size();
            while (e > v && (line[e - 1] == ' ' || line[e - 1] == '\t'))
                --e;
            return line.substr(v, e - v);
        }
        at = end;
    }
    return {};
}

class WsStream final : public platform::ILanStream {
public:
    WsStream(std::unique_ptr<platform::ILanStream> tcp, std::vector<uint8_t> pending)
        : tcp_(std::move(tcp)), in_(std::move(pending)), random_(std::random_device{}()) {}

    bool send_all(const void* data, size_t size) override {
        return send_frame(kOpBinary, static_cast<const uint8_t*>(data), size);
    }

    int recv(void* data, size_t capacity, int timeout_ms) override {
        if (closed_)
            return -1;
        if (capacity == 0)
            return 0;
        size_t got = 0;
        auto* out = static_cast<uint8_t*>(data);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
        for (;;) {
            // Payload of the current data frame first.
            while (got < capacity && payload_left_ != 0 && pos_ < in_.size()) {
                const size_t n = std::min({capacity - got, payload_left_, in_.size() - pos_});
                for (size_t i = 0; i < n; ++i)
                    out[got + i] = in_[pos_ + i] ^ mask_[(payload_done_ + i) & 3];
                pos_ += n;
                got += n;
                payload_left_ -= n;
                payload_done_ += n;
            }
            if (got != 0)
                break;
            if (payload_left_ == 0) {
                const int r = parse_frame_header();
                if (r < 0) {
                    closed_ = true;
                    return -1;
                }
                if (r > 0)
                    continue;  // a frame header (or a control frame) was taken
            }
            compact();
            int wait = -1;
            if (timeout_ms >= 0) {
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                wait = static_cast<int>(std::max<int64_t>(left.count(), 0));
            }
            uint8_t chunk[16384];
            const int n = tcp_->recv(chunk, sizeof(chunk), wait);
            if (n < 0) {
                closed_ = true;
                return -1;
            }
            if (n == 0)
                return 0;
            in_.insert(in_.end(), chunk, chunk + n);
        }
        return static_cast<int>(got);
    }

    void shutdown() override {
        if (!closed_) {
            const uint8_t code[2] = {0x03, 0xE8};  // 1000, normal closure
            send_frame(kOpClose, code, sizeof(code));
        }
        tcp_->shutdown();
    }

private:
    // 1: took a frame header or a whole control frame, 0: needs more bytes,
    // -1: closed or broken.
    int parse_frame_header() {
        const size_t avail = in_.size() - pos_;
        if (avail < 2)
            return 0;
        const uint8_t* p = in_.data() + pos_;
        const uint8_t op = p[0] & 0x0F;
        const bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;
        size_t head = 2;
        if (len == 126) {
            if (avail < 4)
                return 0;
            len = uint64_t{p[2]} << 8 | p[3];
            head = 4;
        } else if (len == 127) {
            if (avail < 10)
                return 0;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = len << 8 | p[2 + i];
            head = 10;
        }
        if (len > kMaxFrame)
            return -1;
        std::array<uint8_t, 4> mask{};
        if (masked) {
            if (avail < head + 4)
                return 0;
            std::memcpy(mask.data(), p + head, 4);
            head += 4;
        }
        if (op == kOpContinuation || op == kOpText || op == kOpBinary) {
            pos_ += head;
            payload_left_ = static_cast<size_t>(len);
            payload_done_ = 0;
            mask_ = mask;
            return 1;
        }
        // Control frames come whole (at most 125 bytes).
        if (avail < head + len)
            return 0;
        std::vector<uint8_t> body(p + head, p + head + len);
        for (size_t i = 0; i < body.size(); ++i)
            body[i] ^= mask[i & 3];
        pos_ += head + static_cast<size_t>(len);
        if (op == kOpPing) {
            send_frame(kOpPong, body.data(), body.size());
            return 1;
        }
        if (op == kOpPong)
            return 1;
        if (op == kOpClose) {
            send_frame(kOpClose, body.data(), std::min<size_t>(body.size(), 2));
            return -1;
        }
        return -1;  // unknown opcode
    }

    void compact() {
        if (pos_ != 0) {
            in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(pos_));
            pos_ = 0;
        }
    }

    bool send_frame(uint8_t op, const uint8_t* data, size_t size) {
        std::lock_guard lock(send_mutex_);
        std::vector<uint8_t> f;
        f.reserve(size + 14);
        f.push_back(static_cast<uint8_t>(0x80 | op));
        if (size < 126) {
            f.push_back(static_cast<uint8_t>(0x80 | size));
        } else if (size <= 0xFFFF) {
            f.push_back(0x80 | 126);
            f.push_back(static_cast<uint8_t>(size >> 8));
            f.push_back(static_cast<uint8_t>(size));
        } else {
            f.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i)
                f.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> (i * 8)));
        }
        const uint32_t m = static_cast<uint32_t>(random_());
        const uint8_t mask[4] = {static_cast<uint8_t>(m), static_cast<uint8_t>(m >> 8), static_cast<uint8_t>(m >> 16),
                                 static_cast<uint8_t>(m >> 24)};
        f.insert(f.end(), mask, mask + 4);
        for (size_t i = 0; i < size; ++i)
            f.push_back(data[i] ^ mask[i & 3]);
        return tcp_->send_all(f.data(), f.size());
    }

    std::unique_ptr<platform::ILanStream> tcp_;
    std::vector<uint8_t> in_;  // received, not yet handed out (from pos_)
    size_t pos_ = 0;
    size_t payload_left_ = 0;  // of the current data frame
    size_t payload_done_ = 0;
    std::array<uint8_t, 4> mask_{};
    std::atomic<bool> closed_{false};
    std::mutex send_mutex_;
    std::mt19937 random_;  // under send_mutex_
};

}  // namespace

std::string websocket_accept(const std::string& key) {
    const std::array<uint8_t, 20> d = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64(d.data(), d.size());
}

std::unique_ptr<platform::ILanStream> websocket_connect(std::unique_ptr<platform::ILanStream> tcp,
                                                        const std::string& host, uint16_t port, bool tls,
                                                        const std::string& path, int timeout_ms, std::string* error) {
    std::random_device device;
    uint8_t nonce[16];
    for (uint8_t& b : nonce)
        b = static_cast<uint8_t>(device());
    const std::string key = base64(nonce, sizeof(nonce));
    std::string request = "GET " + (path.empty() ? std::string("/") : path) + " HTTP/1.1\r\nHost: " + host;
    if (port != (tls ? 443 : 80))  // the scheme's default port stays out
        request += ":" + std::to_string(port);
    request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
               "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: lgp-recomp\r\n\r\n";
    if (!tcp->send_all(request.data(), request.size())) {
        *error = "the connection closed before the upgrade";
        return nullptr;
    }
    std::vector<uint8_t> in;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    size_t end = std::string::npos;
    while (end == std::string::npos) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            *error = "no answer to the upgrade";
            return nullptr;
        }
        uint8_t chunk[4096];
        const int n = tcp->recv(chunk, sizeof(chunk), static_cast<int>(left));
        if (n < 0) {
            *error = "the connection closed during the upgrade";
            return nullptr;
        }
        in.insert(in.end(), chunk, chunk + n);
        const std::string text(in.begin(), in.end());
        end = text.find("\r\n\r\n");
        if (end == std::string::npos && in.size() > kMaxResponse) {
            *error = "the upgrade answer is too long";
            return nullptr;
        }
    }
    const std::string response(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(end + 2));
    const std::string status = response.substr(0, response.find("\r\n"));
    if (status.size() < 12 || status.compare(0, 5, "HTTP/") != 0 || status.find(" 101") == std::string::npos) {
        *error = "the server answered \"" + status + "\" instead of 101";
        return nullptr;
    }
    if (header(response, "sec-websocket-accept") != websocket_accept(key)) {
        *error = "wrong Sec-WebSocket-Accept";
        return nullptr;
    }
    // Bytes after the headers already belong to the first frames.
    std::vector<uint8_t> pending(in.begin() + static_cast<std::ptrdiff_t>(end + 4), in.end());
    return std::make_unique<WsStream>(std::move(tcp), std::move(pending));
}

}  // namespace lgp::hle::ldn
