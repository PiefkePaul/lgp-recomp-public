// "lgp:test": an own HLE service for the IPC tests (P3.3).
//   0 Add(u32 a, u32 b) -> u32
//   1 OpenSubInterface() -> object "lgp:test.sub" (same commands)
//   2 GetPid() -> u64 (pid from the request's special header)
//   3 CopyBuffer(A buffer 0 -> B buffer 0) -> u64 bytes copied
//   4 DescribeBuffers() -> X0 addr, X0 size, C0 addr, C0 size (u64 each), X0 index (u32)
//   5 GetInObject() -> u32 (first domain in-object id, 0 if none)
#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "lgp/hle/service.hpp"

namespace lgp_test {

class TestService final : public lgp::hle::Service {
public:
    explicit TestService(std::string name = "lgp:test") : Service(std::move(name)) {
        using lgp::hle::Request;
        using lgp::hle::Response;
        on(0, [](Request& req, Response& resp) {
            const uint32_t a = req.pop<uint32_t>();
            const uint32_t b = req.pop<uint32_t>();
            resp.push<uint32_t>(a + b);
        });
        on(1, [](Request&, Response& resp) { resp.push_object(std::make_shared<TestService>("lgp:test.sub")); });
        on(2, [](Request& req, Response& resp) { resp.push<uint64_t>(req.pid()); });
        on(3, [](Request& req, Response& resp) {
            const auto& h = req.hipc();
            if (h.a.empty() || h.b.empty()) {
                resp.result = lgp::rt::make_result(10, 1);
                return;
            }
            const uint64_t n = std::min(h.a[0].size, h.b[0].size);
            std::vector<uint8_t> tmp(n);
            auto& k = req.kernel();
            std::lock_guard lock(k.memory_mutex());
            if (!k.memory().copy_from_user(tmp.data(), h.a[0].addr, n) ||
                !k.memory().copy_to_user(h.b[0].addr, tmp.data(), n)) {
                resp.result = lgp::rt::kernel_result::kInvalidPointer;
                return;
            }
            resp.push<uint64_t>(n);
        });
        on(4, [](Request& req, Response& resp) {
            const auto& h = req.hipc();
            const lgp::hle::HipcBuffer x = h.x.empty() ? lgp::hle::HipcBuffer{} : h.x[0];
            const lgp::hle::HipcBuffer c = h.c.empty() ? lgp::hle::HipcBuffer{} : h.c[0];
            resp.push<uint64_t>(x.addr);
            resp.push<uint64_t>(x.size);
            resp.push<uint64_t>(c.addr);
            resp.push<uint64_t>(c.size);
            resp.push<uint32_t>(x.index_or_mode);
        });
        on(5, [](Request& req, Response& resp) {
            resp.push<uint32_t>(req.in_objects().empty() ? 0 : req.in_objects()[0]);
        });
    }
};

}  // namespace lgp_test
