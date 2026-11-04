// Host work on a guest thread that does not need its scheduler slot (P7.3).
//
// In the core baton mode a guest thread keeps its core's slot while it runs
// host code, such as recording and submitting GPU work in a renderer call.
// Another guest thread of the same core waits meanwhile, although on the
// Switch it would run beside the GPU. With enable_host_work_release(true) a
// HostWorkScope gives the slot up for its lifetime and queues for it again
// at the end (at the front, ahead of equal priorities). Off by default
// (LGP_RENDER_RELEASE_SLOT in lgp_boot); other modes ignore it: the single
// baton stays reproducible, the free mode has no slots.
//
// Rule for users: code inside a HostWorkScope must not wait for anything a
// slot holder may hold. A host mutex that guest threads also take while they
// hold their slot (the NVN-HLE lock) has to be taken with the slot given up
// too when it is contended, else the two threads wait for each other.
#pragma once

namespace lgp::rt {

void enable_host_work_release(bool on);
bool host_work_release_enabled();

class HostWorkScope {
public:
    HostWorkScope() : HostWorkScope(false) {}
    // force: give the slot up whatever enable_host_work_release() says, for a
    // guest thread that only waits on the host (an HLE request waiting for a
    // network server: on Horizon the client blocks in the IPC and its core
    // runs other threads). The rule above holds the same: end the scope with
    // no host lock held that a slot holder may take.
    explicit HostWorkScope(bool force);
    ~HostWorkScope();
    HostWorkScope(const HostWorkScope&) = delete;
    HostWorkScope& operator=(const HostWorkScope&) = delete;

private:
    bool left_ = false;
};

}  // namespace lgp::rt
