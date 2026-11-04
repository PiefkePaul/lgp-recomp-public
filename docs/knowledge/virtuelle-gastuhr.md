# Virtual guest clock: reproducible runs without a wall clock

Game-independent knowledge (status 2026-10-12). Implementation: `runtime/core/include/lgp/runtime/clock.hpp`,
`runtime/core/src/clock.cpp`, `scheduler.cpp` (timeouts, idle), `kernel.cpp` (cost per SVC),
`system.cpp` (counter reads), `runtime/boot/src/boot.cpp` (vsync at present, tick for `hid`/`audout`).
Switch in the boot program: `--clock host|virtual` (default `host`).

## Problem

With `--sched baton` only one guest thread runs at a time, and the next one is chosen by (priority, queue).
Still, two runs with the same `--press` script were not identical: on the PC the same trigger fired once at
frame 3347, once at 3423, and the frames differed. The cause is the guest time. It depended on the host clock everywhere:

| Place | Host clock dependency |
|---|---|
| `svcGetSystemTick`, `MRS CNTPCT_EL0`/`CNTVCT_EL0` | host `steady_clock` since program start |
| Timeouts (`SleepThread`, `WaitSynchronization`, `WaitProcessWideKeyAtomic`) | `condition_variable::wait_until` on the host clock |
| `hid` sampling, release of the `audout` buffers | own host thread every 5 ms of wall clock, i.e. at an arbitrary point in the guest run |
| `time` services (date) | host date at startup |
| Input script (`Ts` triggers) | `system_tick()` |

Each of these places changes when a thread wakes up or which values it sees, and with that the entire rest of the run.

## Solution

In virtual mode only the guest itself moves time:

1. **Cost per work step:** every SVC of a guest thread costs `kVirtualSvcCostNs` (1 µs), every read of the
   counter via `MRS` costs `kVirtualCounterReadCostNs` (100 ns). Loops that poll for a time or an event
   therefore see time pass and terminate. Pure computation without SVCs costs no guest time. The values are rough
   estimates; for reproducibility all that matters is that they are fixed.
2. **Idle is skipped:** if every guest thread is waiting (none running, none ready), the clock jumps to the
   next deadline: the end of a wait with timeout or the next periodic host callback. The thread that blocks
   last (or exits) makes this jump. A sleep phase of seconds thus takes only microseconds of
   host time (test: 8 s of guest time in about 10 ms).
3. **Vsync at present:** `nvnQueuePresentTexture` ends at the next 60 Hz vsync (times the window's present interval),
   like a swap on the console. A frame thus takes at least 16.7 ms of guest time, no matter how fast the
   host renders. Vsync k lies exactly at k/60 s.
4. **Periodic host callbacks instead of a host thread:** the `hid` and `audout` ticks (5 ms) run as
   `GuestClock::add_periodic`. They run on the guest thread that moves the clock past their deadline (at the SVC entry,
   on a counter read, at present) or that detects idle, with their deadline as the time.
5. **Fixed order when firing:** first all due callbacks (without the scheduler lock), then all expired
   timeouts, sorted by (deadline, start of the wait). Meanwhile the scheduler holds back the assignment of slots
   (`hold_dispatch_`); the woken threads only get their slot afterwards, together and by priority.
6. **Fixed date:** without `--time`, `--clock virtual` sets the POSIX start time to 2026-01-01 00:00 UTC
   (`boot::kVirtualClockPosixTime`), and without `--sched` the mode `baton`.

Only with `baton` is the run reproducible. With `core-baton` or `free` the virtual clock works too (the
tests check that), but threads of different cores run in parallel and add their costs in varying order.

## What the mode does not pin down

- **Host events without a guest connection:** real input (SDL, gamepad API) arrives by wall clock. For reproducible
  runs use only `--press`.
- **Asynchronous host decoders:** `WebVideo` (WebCodecs) in the browser delivers frames from a worker with wait-time limits;
  if a limit expires, the result depends on the host. FFmpeg on the desktop decodes synchronously and is uncritical.
- **Hangs:** if all threads wait without a deadline for an event that only a periodic callback could trigger,
  the clock keeps turning at idle (5 ms per callback each time) until something happens or `--watchdog` ends it. In
  host mode the same run would simply stand still.
- **Performance:** the costs are fixed and not measured. Games that couple their logic to the measured frame time
  run in virtual mode as if every frame had finished within one vsync. For performance measurements the
  host mode still applies.
- **Loading times cost no guest time:** file accesses (`fsp-srv`), decoding and shader translation run on the
  host and only cost the fixed SVC costs. Loading screens are therefore much shorter in virtual mode than in
  host mode. Frame numbers and `Ts` triggers therefore only apply to the clock mode in which they were measured; a
  `--press` script is re-measured per clock mode.
- **Combination with `--paced`:** first the present advances the virtual clock to the vsync, then the pacer
  (`FramePacer`) computes on the same guest time. It then practically no longer waits, because the frame is already at the vsync.
- **Host memory addresses** are invisible to the guest and play no role.

## Telling work from logic

Because SVCs and counter reads cost guest time, two runs with the same input but different guest work (for example
more draws, hence more SVCs in the render thread) see slightly different times inside a frame. That alone can change
the order of wake-ups and periodic callbacks, and with them the rest of the run, without any change in the game's
logic. `LGP_VCLOCK_LOG=FROM[:COUNT]` (default 600 lines) logs per present
`vclock: present N at T ns, D ns after vsync V (next W)`: D is the guest time the frame used before its present.
Two arms whose D values differ from early on can drift apart for this reason alone (test `boot.sdk_clock_virtual_log`).

## Verification

- `runtime.virtual_clock` (`tests/runtime/test_virtual_clock.cpp`): three sleeping threads, one thread waiting for an
  event signaled by a periodic callback, a poll loop on `GetSystemTick` and a
  CondVar timeout. Four runs with `baton` give the same trace down to the tick; the callbacks run
  exactly at their deadlines; 8 s of guest time take less than 4 s of host time (measured: milliseconds).
- `runtime.threads_svc_virtual`, `runtime.threads_lifted_virtual`, `runtime.ipc_lifted_virtual`: the existing
  thread, sync and IPC tests (including lifted programs) on the virtual clock.
- `boot.sdk_clock_virtual`: the SDK-like module pair with NVN HLE and `--press "2:a,0.02s:b"`; both triggers fire
  at frame 2 and 0.03 s (frame 2 is presented at 33.3 ms), every run gives the same output. In host mode
  the program ends before the `hid` tick even checks the triggers.
- On the real game (PC, 2026-10-12): two runs with `--clock virtual` were bit-identical, the triggers fell on
  identical frames, no hang, about 3× real time (`docs/analysis/17-intro-und-erste-ingame-szene.md`).
