#include "lgp/hle/hipc.hpp"

namespace lgp::hle {
namespace {

class Reader {
public:
    Reader(const uint8_t* msg, uint32_t size) : msg_(msg), size_(size) {}
    bool word(uint32_t* out) {
        if (pos_ + 4 > size_)
            return false;
        std::memcpy(out, msg_ + pos_, 4);
        pos_ += 4;
        return true;
    }
    uint32_t pos() const { return pos_; }
    void seek(uint32_t pos) { pos_ = pos; }

private:
    const uint8_t* msg_;
    uint32_t size_;
    uint32_t pos_ = 0;
};

bool read_static(Reader& r, HipcBuffer* out) {
    uint32_t w0 = 0;
    uint32_t w1 = 0;
    if (!r.word(&w0) || !r.word(&w1))
        return false;
    out->index_or_mode = w0 & 0x3F;
    out->addr = w1 | (uint64_t{(w0 >> 12) & 0xF} << 32) | (uint64_t{(w0 >> 6) & 0x3F} << 36);
    out->size = w0 >> 16;
    return true;
}

bool read_buffer(Reader& r, HipcBuffer* out) {
    uint32_t size_low = 0;
    uint32_t addr_low = 0;
    uint32_t w2 = 0;
    if (!r.word(&size_low) || !r.word(&addr_low) || !r.word(&w2))
        return false;
    out->index_or_mode = w2 & 0x3;
    out->addr = addr_low | (uint64_t{(w2 >> 28) & 0xF} << 32) | (uint64_t{(w2 >> 2) & 0x3FFFFF} << 36);
    out->size = size_low | (uint64_t{(w2 >> 24) & 0xF} << 32);
    return true;
}

bool read_recv(Reader& r, HipcBuffer* out) {
    uint32_t addr_low = 0;
    uint32_t w1 = 0;
    if (!r.word(&addr_low) || !r.word(&w1))
        return false;
    out->addr = addr_low | (uint64_t{w1 & 0xFFFF} << 32);
    out->size = w1 >> 16;
    return true;
}

}  // namespace

std::optional<HipcRequest> parse_hipc(const uint8_t* msg, uint32_t size) {
    Reader r(msg, size);
    uint32_t w0 = 0;
    uint32_t w1 = 0;
    if (!r.word(&w0) || !r.word(&w1))
        return std::nullopt;
    HipcRequest req;
    req.type = static_cast<uint16_t>(w0 & 0xFFFF);
    const uint32_t num_x = (w0 >> 16) & 0xF;
    const uint32_t num_a = (w0 >> 20) & 0xF;
    const uint32_t num_b = (w0 >> 24) & 0xF;
    const uint32_t num_w = (w0 >> 28) & 0xF;
    const uint32_t num_data_words = w1 & 0x3FF;
    const uint32_t recv_static_mode = (w1 >> 10) & 0xF;
    const uint32_t recv_list_offset = (w1 >> 20) & 0x7FF;
    if (w1 >> 31) {
        uint32_t special = 0;
        if (!r.word(&special))
            return std::nullopt;
        req.send_pid = (special & 1) != 0;
        const uint32_t num_copy = (special >> 1) & 0xF;
        const uint32_t num_move = (special >> 5) & 0xF;
        if (req.send_pid) {
            uint32_t lo = 0;
            uint32_t hi = 0;
            if (!r.word(&lo) || !r.word(&hi))
                return std::nullopt;
            req.pid = lo | (uint64_t{hi} << 32);
        }
        for (uint32_t i = 0; i < num_copy; ++i) {
            uint32_t h = 0;
            if (!r.word(&h))
                return std::nullopt;
            req.copy_handles.push_back(h);
        }
        for (uint32_t i = 0; i < num_move; ++i) {
            uint32_t h = 0;
            if (!r.word(&h))
                return std::nullopt;
            req.move_handles.push_back(h);
        }
    }
    req.x.resize(num_x);
    for (HipcBuffer& b : req.x) {
        if (!read_static(r, &b))
            return std::nullopt;
    }
    for (auto [list, n] : {std::pair{&req.a, num_a}, std::pair{&req.b, num_b}, std::pair{&req.w, num_w}}) {
        list->resize(n);
        for (HipcBuffer& b : *list) {
            if (!read_buffer(r, &b))
                return std::nullopt;
        }
    }
    req.data_offset = r.pos();
    req.data_size = num_data_words * 4;
    if (req.data_offset + req.data_size > size)
        return std::nullopt;
    // Recv list (C buffers): mode 2 = one descriptor, n > 2 = n - 2 descriptors.
    if (recv_static_mode >= 2) {
        const uint32_t count = recv_static_mode == 2 ? 1 : recv_static_mode - 2;
        r.seek(recv_list_offset != 0 ? recv_list_offset * 4 : req.data_offset + req.data_size);
        req.c.resize(count);
        for (HipcBuffer& b : req.c) {
            if (!read_recv(r, &b))
                return std::nullopt;
        }
    }
    return req;
}

uint32_t write_hipc_response(uint8_t* msg, uint32_t size, uint32_t raw_size, const std::vector<uint32_t>& copy,
                             const std::vector<uint32_t>& move) {
    const bool special = !copy.empty() || !move.empty();
    uint32_t pos = 8 + (special ? 4 : 0) + 4 * static_cast<uint32_t>(copy.size() + move.size());
    const uint32_t raw = align16(pos);
    // Data words count from the first raw word, including the alignment padding.
    const uint32_t num_data_words = (raw - pos + raw_size + 3) / 4;
    if (raw + raw_size > size || num_data_words > 0x3FF)
        return 0;
    std::memset(msg, 0, raw + raw_size);
    const uint32_t w0 = 0;  // type 0, no buffers
    const uint32_t w1 = num_data_words | (special ? 0x80000000u : 0);
    std::memcpy(msg, &w0, 4);
    std::memcpy(msg + 4, &w1, 4);
    if (special) {
        const uint32_t sh = (static_cast<uint32_t>(copy.size()) << 1) | (static_cast<uint32_t>(move.size()) << 5);
        std::memcpy(msg + 8, &sh, 4);
        uint32_t p = 12;
        for (uint32_t h : copy) {
            std::memcpy(msg + p, &h, 4);
            p += 4;
        }
        for (uint32_t h : move) {
            std::memcpy(msg + p, &h, 4);
            p += 4;
        }
    }
    return raw;
}

}  // namespace lgp::hle
