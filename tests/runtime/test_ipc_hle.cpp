// P3.3: HIPC parsing, CMIF sessions, domains and sm:, driven through the
// kernel's svcs with raw registers (ipc_client.hpp).
#include "ipc_client.hpp"
#include "ipc_test_service.hpp"

using namespace lgp_test;
namespace kr = kernel_result;

namespace {

// Kernel + sm: with the test service registered.
struct TestEnv : Env {
    TestEnv()
        : Env([](ServiceManager& sm) {
              sm.register_service("lgp:test", [] { return std::make_shared<TestService>(); });
          }) {}
};

// --- tests ------------------------------------------------------------------------

void test_parse_descriptors() {
    Msg m;
    m.send_pid = true;
    m.copy = {0x11};
    m.move = {0x22, 0x33};
    m.x = {{0x0000'0123'4567'8000ull, 0x40, 5}};
    m.a = {{0x0000'0012'3456'7000ull, 0x1'2345'6789ull, 1}};
    m.b = {{0x0000'0034'5678'9000ull, 0x10, 0}};
    m.c = {{0x0000'5678'9abc'd000ull, 0x200}};
    m.command = 7;
    m.arg<uint32_t>(0xabcdef01);
    const std::vector<uint8_t> bytes = build(m);
    const std::optional<HipcRequest> r = parse_hipc(bytes.data(), static_cast<uint32_t>(bytes.size()));
    CHECK(r.has_value());
    if (!r)
        return;
    CHECK_EQ(r->type, 4u);
    CHECK(r->send_pid);
    CHECK_EQ(r->pid, 0xdeadbeefull);
    CHECK(r->copy_handles == std::vector<uint32_t>{0x11});
    CHECK(r->move_handles == (std::vector<uint32_t>{0x22, 0x33}));
    CHECK_EQ(r->x.size(), 1u);
    CHECK_EQ(r->x[0].addr, 0x0000'0123'4567'8000ull);
    CHECK_EQ(r->x[0].size, 0x40u);
    CHECK_EQ(r->x[0].index_or_mode, 5u);
    CHECK_EQ(r->a.size(), 1u);
    CHECK_EQ(r->a[0].addr, 0x0000'0012'3456'7000ull);
    CHECK_EQ(r->a[0].size, 0x1'2345'6789ull);
    CHECK_EQ(r->a[0].index_or_mode, 1u);
    CHECK_EQ(r->b[0].addr, 0x0000'0034'5678'9000ull);
    CHECK_EQ(r->c.size(), 1u);
    CHECK_EQ(r->c[0].addr, 0x0000'5678'9abc'd000ull);
    CHECK_EQ(r->c[0].size, 0x200u);
    // CMIF header at the first 16-byte boundary inside the raw data.
    CmifInHeader in{};
    std::memcpy(&in, bytes.data() + align16(r->data_offset), sizeof(in));
    CHECK_EQ(in.magic, kCmifInMagic);
    CHECK_EQ(in.command_id, 7u);

    // Truncated messages are rejected, not read past their end.
    for (uint32_t n : {0u, 4u, 12u, static_cast<uint32_t>(bytes.size()) - 9})
        CHECK(!parse_hipc(bytes.data(), n).has_value());
}

void test_response_writer() {
    std::array<uint8_t, 0x100> buf{};
    buf.fill(0xCC);
    const uint32_t off = write_hipc_response(buf.data(), sizeof(buf), 0x18, {0x5}, {0x6, 0x7});
    CHECK_EQ(off, 0x20u);  // 8 + 4 + 3 handles = 0x18 -> 0x20
    uint32_t w1 = 0;
    std::memcpy(&w1, buf.data() + 4, 4);
    CHECK_EQ(w1 >> 31, 1u);
    CHECK_EQ(w1 & 0x3FF, (0x20u - 0x18u + 0x18u) / 4u);
    uint32_t sh = 0;
    std::memcpy(&sh, buf.data() + 8, 4);
    CHECK_EQ(sh, (1u << 1) | (2u << 5));
    CHECK_EQ(buf[0x20], 0u);       // raw data zeroed
    CHECK_EQ(buf[0x38], 0xCCu);    // nothing written past it
    CHECK_EQ(write_hipc_response(buf.data(), 0x20, 0x18, {}, {}), 0u);  // 0x10 + 0x18 does not fit
}

void test_sm() {
    TestEnv e;
    e.connect("nope:", kr::kNotFound);
    const Handle sm_handle = e.connect("sm:");
    CHECK(sm_handle != kInvalidHandle);

    Msg get;
    get.command = 1;
    get.arg<uint64_t>(name_u64("lgp:test"));
    CHECK_EQ(e.send(sm_handle, get)->result, sm_result::kInvalidClient.raw);  // before RegisterClient

    Msg reg;
    reg.send_pid = true;
    reg.arg<uint64_t>(0);
    CHECK_EQ(e.send(sm_handle, reg)->result, 0u);

    Msg unknown = get;
    unknown.payload.clear();
    unknown.arg<uint64_t>(name_u64("fsp-srv"));
    const Reply nr = *e.send(sm_handle, unknown);
    CHECK_EQ(nr.result, sm_result::kNotRegistered.raw);
    CHECK(nr.move.empty());

    const Reply r = *e.send(sm_handle, get);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.move.size(), 1u);

    // A second connection has its own RegisterClient state.
    const Handle sm2 = e.connect("sm:");
    CHECK_EQ(e.send(sm2, get)->result, sm_result::kInvalidClient.raw);

    // Unknown sm command stops the guest instead of guessing.
    Msg bad;
    bad.command = 99;
    e.send(sm_handle, bad, SvcOutcome::Unimplemented);
}

void test_commands() {
    TestEnv e;
    const Handle h = e.open_test_service();

    Msg add;
    add.arg<uint32_t>(40).arg<uint32_t>(2);
    const Reply r = *e.send(h, add);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint32_t>(0), 42u);

    Msg pid;
    pid.command = 2;
    pid.send_pid = true;
    CHECK_EQ(e.send(h, pid)->get<uint64_t>(0), 0x1234u);  // filled in by the kernel

    // A -> B buffer copy through guest memory.
    const char text[] = "hello, ipc";
    CHECK(e.k.memory().copy_to_user(kBufA, text, sizeof(text)));
    Msg copy;
    copy.command = 3;
    copy.a = {{kBufA, sizeof(text), 0}};
    copy.b = {{kBufB, 0x100, 0}};
    CHECK_EQ(e.send(h, copy)->get<uint64_t>(0), sizeof(text));
    char back[sizeof(text)] = {};
    CHECK(e.k.memory().copy_from_user(back, kBufB, sizeof(back)));
    CHECK(std::memcmp(back, text, sizeof(text)) == 0);

    // X and C descriptors reach the service.
    Msg desc;
    desc.command = 4;
    desc.x = {{kBufA, 0x20, 3}};
    desc.c = {{kBufB, 0x80}};
    const Reply d = *e.send(h, desc);
    CHECK_EQ(d.get<uint64_t>(0), kBufA);
    CHECK_EQ(d.get<uint64_t>(8), 0x20u);
    CHECK_EQ(d.get<uint64_t>(16), kBufB);
    CHECK_EQ(d.get<uint64_t>(24), 0x80u);
    CHECK_EQ(d.get<uint32_t>(32), 3u);

    // Bad CMIF magic is answered with an error, the session stays usable.
    Msg bad_magic = add;
    bad_magic.magic = 0x12345678;
    CHECK_EQ(e.send(h, bad_magic)->result, sf_result::kInvalidInHeader.raw);
    CHECK_EQ(e.send(h, add)->get<uint32_t>(0), 42u);

    // Unknown command: guest stops (svc Unimplemented).
    Msg unknown;
    unknown.command = 1000;
    e.send(h, unknown, SvcOutcome::Unimplemented);

    // Sub-interface as a move handle on a non-domain session.
    Msg open;
    open.command = 1;
    const Reply o = *e.send(h, open);
    CHECK_EQ(o.move.size(), 1u);
    if (!o.move.empty())
        CHECK_EQ(e.send(o.move[0], add)->get<uint32_t>(0), 42u);

    // Control: QueryPointerBufferSize, CloneCurrentObject.
    Msg q;
    q.type = 5;
    q.command = 3;
    CHECK_EQ(e.send(h, q)->get<uint16_t>(0), 0x8000u);
    Msg clone;
    clone.type = 5;
    clone.command = 2;
    const Reply c = *e.send(h, clone);
    CHECK_EQ(c.move.size(), 1u);
    if (!c.move.empty())
        CHECK_EQ(e.send(c.move[0], add)->get<uint32_t>(0), 42u);

    // Close message: no reply, the client then closes the handle.
    Msg close;
    close.type = 2;
    const std::vector<uint8_t> bytes = build(close);
    CHECK(e.k.memory().copy_to_user(kMsg, bytes.data(), bytes.size()));
    CHECK_EQ(e.svc(kSvcSendSyncRequestWithUserBuffer, {kMsg, kMsgSize, h})[0], kSuccess.raw);
    CHECK_EQ(e.svc(kSvcCloseHandle, {h})[0], kSuccess.raw);
    CHECK_EQ(e.svc(kSvcSendSyncRequestWithUserBuffer, {kMsg, kMsgSize, h})[0], kr::kInvalidHandle.raw);

    // User buffer must be page aligned.
    CHECK_EQ(e.svc(kSvcSendSyncRequestWithUserBuffer, {kMsg + 0x10, kMsgSize, h})[0], kr::kInvalidAddress.raw);
}

void test_domain() {
    TestEnv e;
    const Handle h = e.open_test_service();

    Msg convert;
    convert.type = 5;
    convert.command = 0;  // ConvertCurrentObjectToDomain
    const Reply cv = *e.send(h, convert);
    CHECK_EQ(cv.result, 0u);
    const uint32_t root = cv.get<uint32_t>(0);
    CHECK_EQ(root, 1u);

    Msg add;
    add.domain = true;
    add.object_id = root;
    add.arg<uint32_t>(7).arg<uint32_t>(8);
    const Reply a = *e.send(h, add, SvcOutcome::Continue, true);
    CHECK_EQ(a.result, 0u);
    CHECK_EQ(a.get<uint32_t>(0), 15u);

    // Out object: a new domain id, no handle.
    Msg open;
    open.domain = true;
    open.object_id = root;
    open.command = 1;
    const Reply o = *e.send(h, open, SvcOutcome::Continue, true);
    CHECK_EQ(o.result, 0u);
    CHECK(o.move.empty());
    CHECK_EQ(o.object_ids.size(), 1u);
    const uint32_t sub = o.object_ids.empty() ? 0 : o.object_ids[0];
    CHECK_EQ(sub, 2u);

    Msg add_sub = add;
    add_sub.object_id = sub;
    CHECK_EQ(e.send(h, add_sub, SvcOutcome::Continue, true)->get<uint32_t>(0), 15u);

    // In objects follow the payload.
    Msg in;
    in.domain = true;
    in.object_id = root;
    in.command = 5;
    in.in_objects = {sub};
    CHECK_EQ(e.send(h, in, SvcOutcome::Continue, true)->get<uint32_t>(0), sub);

    // CopyFromCurrentDomain: a plain session handle to a domain object.
    Msg copy;
    copy.type = 5;
    copy.command = 1;
    copy.arg<uint32_t>(sub);
    const Reply cp = *e.send(h, copy);
    CHECK_EQ(cp.result, 0u);
    CHECK_EQ(cp.move.size(), 1u);
    if (!cp.move.empty()) {
        Msg plain_add;
        plain_add.arg<uint32_t>(1).arg<uint32_t>(2);
        CHECK_EQ(e.send(cp.move[0], plain_add)->get<uint32_t>(0), 3u);
    }

    // Clone of a domain session: same domain, the ids work on both, and an
    // object created through the clone is visible on the original.
    Msg clone;
    clone.type = 5;
    clone.command = 4;  // CloneCurrentObjectEx
    clone.arg<uint32_t>(0);
    const Reply cl = *e.send(h, clone);
    CHECK_EQ(cl.result, 0u);
    CHECK_EQ(cl.move.size(), 1u);
    if (!cl.move.empty()) {
        CHECK_EQ(e.send(cl.move[0], add_sub, SvcOutcome::Continue, true)->get<uint32_t>(0), 15u);
        const Reply o2 = *e.send(cl.move[0], open, SvcOutcome::Continue, true);
        CHECK_EQ(o2.object_ids.size(), 1u);
        Msg add_third = add;
        add_third.object_id = o2.object_ids.empty() ? 0 : o2.object_ids[0];
        CHECK_EQ(add_third.object_id, 3u);
        CHECK_EQ(e.send(h, add_third, SvcOutcome::Continue, true)->get<uint32_t>(0), 15u);
    }

    // Close the sub object; afterwards it is gone.
    Msg close = add_sub;
    close.domain_type = 2;
    close.payload.clear();
    e.send(h, close, SvcOutcome::Continue, true);
    CHECK_EQ(e.send(h, add_sub, SvcOutcome::Continue, true)->result, sf_result::kTargetNotFound.raw);
    CHECK_EQ(e.send(h, add, SvcOutcome::Continue, true)->get<uint32_t>(0), 15u);  // root still there
}

void test_process_id() {
    TestEnv e;
    const auto x = e.svc(kSvcGetProcessId, {0, kPseudoHandleCurrentProcess});
    CHECK_EQ(x[0], kSuccess.raw);
    CHECK_EQ(x[1], 0x1234u);
    CHECK_EQ(e.svc(kSvcGetProcessId, {0, 0x777})[0], kr::kInvalidHandle.raw);
}

}  // namespace

int main() {
    RUN(test_parse_descriptors);
    RUN(test_response_writer);
    RUN(test_sm);
    RUN(test_commands);
    RUN(test_domain);
    RUN(test_process_id);
    return finish();
}
