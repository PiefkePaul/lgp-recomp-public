// boot::ProcessControl (P7, app lifecycle): pause/resume reach the kernel of
// the running process, a pause before the process starts applies at attach,
// and pauses nest (pause menu plus app in the background).
// The guest side of Scheduler::pause is tested in test_threads_svc.cpp.
#include "check.hpp"
#include "lgp/boot/boot.hpp"
#include "lgp/runtime/kernel.hpp"

using namespace lgp;

namespace {

void test_control() {
    rt::ProcessConfig c;
    c.custom_layout = true;
    c.layout = rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    rt::Kernel k(c);

    boot::ProcessControl control;
    CHECK(!control.paused());
    CHECK(control.pause(0));  // no process yet: applies once one runs
    CHECK(control.paused());
    control.attach(&k);
    CHECK(k.scheduler().paused());
    control.resume();
    CHECK(!control.paused());
    CHECK(!k.scheduler().paused());
    CHECK(control.pause(100));  // no guest thread runs: at rest at once
    CHECK(k.scheduler().paused());
    control.resume();
    // Nested: pause menu open, then the app goes to the background and back.
    CHECK(control.pause(0));
    CHECK(control.pause(0));
    CHECK(control.pause_count() == 2);
    control.resume();
    CHECK(control.paused());
    CHECK(k.scheduler().paused());
    control.resume();
    CHECK(!control.paused());
    CHECK(!k.scheduler().paused());
    control.resume();  // unmatched: ignored, a later pause still holds
    CHECK(control.pause_count() == 0);
    CHECK(control.pause(0));
    CHECK(k.scheduler().paused());
    control.resume();
    control.attach(nullptr);
    CHECK(!k.scheduler().paused());
    rt::set_active_kernel(nullptr);
}

// The app ends while it is paused in the background: close() lets the guest
// run on to its last present, and no later pause (a late lifecycle event)
// stops it again.
void test_close_while_paused() {
    rt::ProcessConfig c;
    c.custom_layout = true;
    c.layout = rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    rt::Kernel k(c);

    boot::ProcessControl control;
    control.attach(&k);
    CHECK(control.pause(0));
    CHECK(control.pause(0));  // pause menu and background
    CHECK(k.scheduler().paused());
    CHECK(!control.closing());
    control.close();
    CHECK(control.closing());
    CHECK(!control.paused());
    CHECK(!k.scheduler().paused());
    CHECK(!control.pause(0));
    CHECK(!k.scheduler().paused());
    control.resume();  // the foreground event after the end: nothing to do
    CHECK(!k.scheduler().paused());
    control.close();  // twice is fine
    control.attach(nullptr);
    control.attach(&k);  // a process started after close() is not paused
    CHECK(!k.scheduler().paused());
    control.attach(nullptr);
    rt::set_active_kernel(nullptr);
}

}  // namespace

int main() {
    RUN(test_control);
    RUN(test_close_while_paused);
    return finish();
}
