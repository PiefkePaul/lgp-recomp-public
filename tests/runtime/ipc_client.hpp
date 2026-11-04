// Client side of the IPC tests (P3.3/P3.4): builds HIPC/CMIF requests like
// libnx's cmifMakeRequest, parses replies, and drives a kernel through the
// IPC svcs with raw registers (svcSendSyncRequestWithUserBuffer, so no guest
// thread is needed).
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "check.hpp"
#include "lgp/hle/hipc.hpp"
#include "lgp/hle/service.hpp"
#include "lgp/runtime/kernel.hpp"

namespace lgp_test {

using namespace lgp::rt;
using namespace lgp::hle;

inline constexpr uint64_t kMiB = 0x10'0000;
inline constexpr uint64_t kBase = 0x0800'0000;
inline constexpr uint64_t kMsg = kBase;            // message page (user buffer)
inline constexpr uint64_t kName = kBase + 0x1000;  // port names
inline constexpr uint64_t kBufA = kBase + 0x2000;  // send buffer
inline constexpr uint64_t kBufB = kBase + 0xA000;  // receive buffer
inline constexpr uint64_t kBufSize = 0x8000;       // each of A and B
inline constexpr uint64_t kMsgSize = 0x1000;
inline constexpr uint64_t kMappedSize = 0x12000;

inline ProcessConfig small_process() {
    ProcessConfig c;
    c.total_memory_size = 16 * kMiB;
    c.custom_layout = true;
    c.layout = Layout::packed(kBase, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 16 * kMiB);
    c.process_id = 0x1234;
    return c;
}

// --- message building (client side, as libnx does it) ---------------------------

struct Desc {
    uint64_t addr = 0;
    uint64_t size = 0;
    uint32_t index_or_mode = 0;
};

struct Msg {
    uint16_t type = 4;  // Request
    bool send_pid = false;
    std::vector<uint32_t> copy, move;
    std::vector<Desc> x, a, b, c;
    // CMIF
    uint32_t magic = kCmifInMagic;
    uint32_t command = 0;
    std::vector<uint8_t> payload;
    // Domain
    bool domain = false;
    uint8_t domain_type = 1;
    uint32_t object_id = 0;
    std::vector<uint32_t> in_objects;

    template <typename T>
    Msg& arg(const T& v) {
        const size_t at = (payload.size() + alignof(T) - 1) & ~(alignof(T) - 1);
        payload.resize(at + sizeof(T));
        std::memcpy(payload.data() + at, &v, sizeof(T));
        return *this;
    }
};

inline void put32(std::vector<uint8_t>& out, uint32_t v) {
    const size_t at = out.size();
    out.resize(at + 4);
    std::memcpy(out.data() + at, &v, 4);
}

inline std::vector<uint8_t> build(const Msg& m) {
    std::vector<uint8_t> out;
    const uint32_t cmif_size = 16 + static_cast<uint32_t>(m.payload.size());
    const uint32_t raw_size =
        (m.domain ? 16 + 4 * static_cast<uint32_t>(m.in_objects.size()) : 0) + cmif_size;
    const uint32_t num_data_words = (16 + raw_size + 3) / 4;  // 16: alignment padding
    const bool special = m.send_pid || !m.copy.empty() || !m.move.empty();
    const uint32_t recv_mode = m.c.empty() ? 0 : 2 + static_cast<uint32_t>(m.c.size());
    put32(out, m.type | static_cast<uint32_t>(m.x.size()) << 16 | static_cast<uint32_t>(m.a.size()) << 20 |
                   static_cast<uint32_t>(m.b.size()) << 24);
    put32(out, num_data_words | recv_mode << 10 | (special ? 0x80000000u : 0));
    if (special) {
        put32(out, (m.send_pid ? 1u : 0u) | static_cast<uint32_t>(m.copy.size()) << 1 |
                       static_cast<uint32_t>(m.move.size()) << 5);
        if (m.send_pid) {
            put32(out, 0xdeadbeef);  // the kernel overwrites it
            put32(out, 0);
        }
        for (uint32_t h : m.copy)
            put32(out, h);
        for (uint32_t h : m.move)
            put32(out, h);
    }
    for (const Desc& d : m.x) {
        put32(out, d.index_or_mode | static_cast<uint32_t>((d.addr >> 36) & 0x3F) << 6 |
                       static_cast<uint32_t>((d.addr >> 32) & 0xF) << 12 | static_cast<uint32_t>(d.size) << 16);
        put32(out, static_cast<uint32_t>(d.addr));
    }
    for (const auto* list : {&m.a, &m.b}) {
        for (const Desc& d : *list) {
            put32(out, static_cast<uint32_t>(d.size));
            put32(out, static_cast<uint32_t>(d.addr));
            put32(out, d.index_or_mode | static_cast<uint32_t>((d.addr >> 36) & 0x3FFFFF) << 2 |
                           static_cast<uint32_t>((d.size >> 32) & 0xF) << 24 |
                           static_cast<uint32_t>((d.addr >> 32) & 0xF) << 28);
        }
    }
    const uint32_t data_start = static_cast<uint32_t>(out.size());
    out.resize(data_start + num_data_words * 4, 0);
    uint32_t p = align16(data_start);
    if (m.domain) {
        CmifDomainInHeader dh{};
        dh.type = m.domain_type;
        dh.num_in_objects = static_cast<uint8_t>(m.in_objects.size());
        dh.data_size = static_cast<uint16_t>(cmif_size);
        dh.object_id = m.object_id;
        std::memcpy(out.data() + p, &dh, sizeof(dh));
        p += sizeof(dh);
    }
    const CmifInHeader in{m.magic, 0, m.command, 0};
    std::memcpy(out.data() + p, &in, sizeof(in));
    p += sizeof(in);
    if (!m.payload.empty())
        std::memcpy(out.data() + p, m.payload.data(), m.payload.size());
    p += static_cast<uint32_t>(m.payload.size());
    for (uint32_t id : m.in_objects) {
        std::memcpy(out.data() + p, &id, 4);
        p += 4;
    }
    for (const Desc& d : m.c) {
        put32(out, static_cast<uint32_t>(d.addr));
        put32(out, static_cast<uint32_t>((d.addr >> 32) & 0xFFFF) | static_cast<uint32_t>(d.size) << 16);
    }
    return out;
}

// --- response parsing ------------------------------------------------------------

struct Reply {
    uint32_t result = 0xFFFFFFFF;
    std::vector<uint32_t> copy, move;
    std::vector<uint8_t> data;        // after the SFCO header (domain ids included)
    std::vector<uint32_t> object_ids;  // domain replies
    uint32_t magic = 0;

    template <typename T>
    T get(uint32_t off) const {
        T v{};
        if (off + sizeof(T) <= data.size())
            std::memcpy(&v, data.data() + off, sizeof(T));
        return v;
    }
};

inline Reply parse_reply(const uint8_t* msg, bool domain) {
    Reply r;
    uint32_t w0 = 0;
    uint32_t w1 = 0;
    std::memcpy(&w0, msg, 4);
    std::memcpy(&w1, msg + 4, 4);
    CHECK_EQ(w0, 0u);
    uint32_t p = 8;
    if (w1 >> 31) {
        uint32_t sh = 0;
        std::memcpy(&sh, msg + 8, 4);
        p = 12;
        for (uint32_t i = 0; i < ((sh >> 1) & 0xF); ++i, p += 4)
            r.copy.push_back(*reinterpret_cast<const uint32_t*>(msg + p));
        for (uint32_t i = 0; i < ((sh >> 5) & 0xF); ++i, p += 4)
            r.move.push_back(*reinterpret_cast<const uint32_t*>(msg + p));
    }
    const uint32_t data_end = p + (w1 & 0x3FF) * 4;
    p = align16(p);
    uint32_t num_objects = 0;
    if (domain) {
        CmifDomainOutHeader dh{};
        std::memcpy(&dh, msg + p, sizeof(dh));
        num_objects = dh.num_out_objects;
        p += sizeof(dh);
    }
    CmifOutHeader out{};
    std::memcpy(&out, msg + p, sizeof(out));
    p += sizeof(out);
    r.magic = out.magic;
    r.result = out.result;
    if (data_end > p)
        r.data.assign(msg + p, msg + data_end);
    if (domain && num_objects != 0 && r.data.size() >= 4 * num_objects) {
        // Ids are the last words of the raw data (after any padding we cannot see,
        // the server writes them right after the payload).
        for (uint32_t i = 0; i < num_objects; ++i)
            r.object_ids.push_back(r.get<uint32_t>(static_cast<uint32_t>(r.data.size() - 4 * num_objects) + 4 * i));
    }
    return r;
}

// --- kernel plumbing ---------------------------------------------------------------

inline uint64_t name_u64(const char* s) {
    uint64_t v = 0;
    std::memcpy(&v, s, std::min<size_t>(std::strlen(s), 8));
    return v;
}

struct Env {
    Kernel k;
    ServiceManager sm;
    // setup registers the services under test before sm: is installed.
    explicit Env(const std::function<void(ServiceManager&)>& setup, const ProcessConfig& config = small_process())
        : k(config) {
        CHECK(k.map_owned(kBase, kMappedSize, MemState::CodeData, kPermRW).ok());
        setup(sm);
        sm.install(k);
    }

    std::array<uint64_t, 8> svc(uint32_t imm, std::initializer_list<uint64_t> args,
                                SvcOutcome expect = SvcOutcome::Continue) {
        std::array<uint64_t, 8> x{};
        size_t i = 0;
        for (uint64_t a : args)
            x[i++] = a;
        CHECK(k.svc(imm, x.data()) == expect);
        return x;
    }

    Handle connect(const char* name, Result expect = kSuccess) {
        char buf[16] = {};
        std::strncpy(buf, name, sizeof(buf) - 1);
        CHECK(k.memory().copy_to_user(kName, buf, sizeof(buf)));
        const auto x = svc(kSvcConnectToNamedPort, {0, kName});
        CHECK_EQ(x[0], expect.raw);
        return static_cast<Handle>(x[1]);
    }

    // Sends m on session h; returns the reply (or nullopt if the svc did not
    // return Continue + success).
    std::optional<Reply> send(Handle h, const Msg& m, SvcOutcome expect = SvcOutcome::Continue,
                              bool domain_reply = false) {
        std::vector<uint8_t> bytes = build(m);
        bytes.resize(kMsgSize, 0xCC);  // garbage after the message
        CHECK(k.memory().copy_to_user(kMsg, bytes.data(), bytes.size()));
        const auto x = svc(kSvcSendSyncRequestWithUserBuffer, {kMsg, kMsgSize, h}, expect);
        if (expect != SvcOutcome::Continue)
            return std::nullopt;
        CHECK_EQ(x[0], kSuccess.raw);
        std::vector<uint8_t> back(kMsgSize);
        CHECK(k.memory().copy_from_user(back.data(), kMsg, kMsgSize));
        Reply r = parse_reply(back.data(), domain_reply);
        CHECK_EQ(r.magic, kCmifOutMagic);
        return r;
    }

    // RegisterClient + GetServiceHandle(name) on a fresh sm: session.
    Handle open_service(const char* name) {
        const Handle sm_handle = connect("sm:");
        Msg reg;
        reg.send_pid = true;
        reg.arg<uint64_t>(0);
        CHECK_EQ(send(sm_handle, reg)->result, 0u);
        Msg get;
        get.command = 1;
        get.arg<uint64_t>(name_u64(name));
        const Reply r = *send(sm_handle, get);
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.move.size(), 1u);
        CHECK_EQ(svc(kSvcCloseHandle, {sm_handle})[0], kSuccess.raw);
        return r.move.empty() ? kInvalidHandle : r.move[0];
    }
    Handle open_test_service() { return open_service("lgp:test"); }
};

}  // namespace lgp_test
