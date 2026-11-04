# Horizon threads and synchronization in our own runtime

Cross-game knowledge from P3.2 (as of 2026-10-11). Implementation: `runtime/core/` (`thread.hpp`, `scheduler.cpp`,
`synchronization.cpp`, `host_thread.cpp`), decision: ADR-0002. The semantics follow the public ABI documentation
(switchbrew, libnx) and the behavior of Atmosphère/Mesosphère as far as we know it; no code was
taken over. Where something is only our reading, it says so.

## Register ABI of the thread and sync SVCs

Result in `w0`, except where it says "no result".

| SVC | No. | Input | Output |
|---|---|---|---|
| `CreateThread` | 0x08 | `x1` entry, `x2` argument, `x3` stack top, `w4` priority, `w5` core (−2 = process default) | `w1` handle |
| `StartThread` | 0x09 | `w0` handle | – |
| `ExitThread` | 0x0A | – | does not return |
| `SleepThread` | 0x0B | `x0` ns (0, −1, −2 = yield variants) | no result |
| `GetThreadPriority` | 0x0C | `w1` handle | `w1` priority |
| `SetThreadPriority` | 0x0D | `w0` handle, `w1` priority | – |
| `GetThreadCoreMask` | 0x0E | `w2` handle | `w1` core, `x2` mask |
| `SetThreadCoreMask` | 0x0F | `w0` handle, `w1` core (−1 any, −2 process, −3 unchanged), `x2` mask | – |
| `GetCurrentProcessorNumber` | 0x10 | – | `w0` core (no result) |
| `SignalEvent` / `ClearEvent` | 0x11/0x12 | `w0` handle (Clear: writable or readable) | – |
| `ResetSignal` | 0x17 | `w0` readable event | – |
| `WaitSynchronization` | 0x18 | `x1` pointer to handles (u32), `w2` count (≤ 0x40), `x3` timeout ns | `w1` index |
| `CancelSynchronization` | 0x19 | `w0` thread handle | – |
| `ArbitrateLock` | 0x1A | `w0` owner handle, `x1` mutex address, `w2` own tag | – |
| `ArbitrateUnlock` | 0x1B | `x0` mutex address | – |
| `WaitProcessWideKeyAtomic` | 0x1C | `x0` mutex, `x1` condvar key, `w2` own tag, `x3` timeout ns | – |
| `SignalProcessWideKey` | 0x1D | `x0` key, `w1` count (≤ 0 = all) | no result |
| `WaitForAddress` | 0x34 | `x0` address (4-byte aligned), `w1` type, `w2` value, `x3` timeout ns | – |
| `SignalToAddress` | 0x35 | `x0` address, `w1` type, `w2` value, `w3` count (≤ 0 = all) | – |
| `GetSystemTick` | 0x1E | – | `x0` ticks (19.2 MHz, no result) |
| `GetThreadId` | 0x25 | `w1` handle | `x1` id |
| `CreateEvent` | 0x45 | – | `w1` writable, `w2` readable |

New threads start with `x0` = argument, `sp` = stack top, `tpidrro_el0` = TLS area (0x200 bytes, zeroed). The
main thread gets `x0 = 0` and `x1` = its own handle. Timeout −1 (all bits set) means "forever".

Result codes (module 1): `TimedOut` 117 = `0xEA01`, `Cancelled` 118 = `0xEC01`, `TerminationRequested` 59 = `0x7601`,
`InvalidPriority` 112 = `0xE001`, `InvalidCoreId` 113 = `0xE201`, `InvalidCombination` 116 = `0xE801`, `OutOfRange`
119 = `0xEE01`, `InvalidState` 125 = `0xFA01`.

## Mutex and condvar protocol in guest memory

Mutexes are a 32-bit word: 0 = free, otherwise the owner's handle; bit 30 (`0x40000000`) = others are waiting.
Uncontended lock and unlock run entirely in the guest (exclusive pairs); the kernel only gets involved under contention:

- **Lock:** CAS 0 → own handle. Taken: set bit 30, then `ArbitrateLock(owner, addr, own_handle)`.
  The kernel checks whether the word is still exactly `owner | 0x40000000`; otherwise it returns immediately with success
  (the caller tries again).
- **Important after `ArbitrateLock`:** the kernel hands the mutex over directly (writes the waiter's handle into the word).
  The guest must then check whether the owner field is its own handle, and then be **done**. Anyone who instead
  simply starts over sees a taken word (itself), sets bit 30 and waits for itself: deadlock.
  libnx does exactly this check; our first test program did not.
- **Unlock:** CAS own handle → 0. If that fails (bit 30 set), `ArbitrateUnlock(addr)`. The kernel picks the
  waiter with the highest priority, writes its tag (plus bit 30 if more are waiting) and wakes it.
- **Condvar wait:** `WaitProcessWideKeyAtomic` releases the mutex (like unlock), writes 1 into the key and sleeps.
  On a signal the kernel tries to take the mutex for the woken thread; if it is taken, it queues it as a mutex waiter.
  On **timeout** the caller returns **without** the mutex and must lock it again itself (libnx:
  `if (rc == 0xEA01) mutexLock(m)`). Timeout 0 releases the mutex and reports `TimedOut` immediately.
- **Condvar signal:** wakes up to `n` waiters by priority; if afterwards nobody is waiting on the key any more, the
  kernel writes 0 into it. (As far as we know, Mesosphère only writes 0 if there were fewer waiters than requested;
  the difference costs at most one superfluous SVC.)

Deviation in our runtime: waiters hang off the mutex **address**, not the owner thread. As a result there is no
priority inheritance. Not critical for LGP (almost all threads priority 44, P1.3); to be checked for other games.

## WaitForAddress / SignalToAddress

Address arbiter on a 32-bit word, without a mutex. LGP calls `WaitForAddress` when saving synchronously after a
link trade (PC run 2026-10-21: "unimplemented svc 0x34", both instances crashed). Types as in Horizon:

- Wait: 0 = wait if `*addr < value`; 1 = the same, decrementing the word by 1 in the process (test on the old value);
  2 = wait if `*addr == value`. If the condition does not hold: `InvalidState`. Timeout 0 with the condition met:
  `TimedOut`; the decrement of type 1 then stays in place.
- Signal: 0 = wakes up to count waiters; 1 = only if `*addr == value`, sets `value + 1`; 2 = only if
  `*addr == value`, sets depending on the waiters: none → `value + 1`, all are woken (count ≤ 0) → `value − 2`,
  all waiters fit into the count → `value − 1`, otherwise `value`. Word not equal: `InvalidState`.
- Errors: not aligned `InvalidAddress`, unknown type `InvalidEnumValue`, not writable
  `InvalidCurrentMemory`. Wake order: highest priority, then longest waiting.

Test and modification of the word run under the scheduler lock, so a signal cannot fall between a waiter's test and its
going to sleep. Source of the semantics: switchbrew (SVC). Test `runtime.threads_svc` (`test_address_arbiter`).

## WaitSynchronization, events, cancel

- Check order (Mesosphère): process termination requested → any object signaled (lowest index wins) →
  timeout 0 → cancel flag set → sleep.
- Signalable are readable events (stay set until `ClearEvent`/`ResetSignal`) and threads (once they end). A
  writable event handle in the list is `InvalidHandle`.
- `SignalEvent` on an already set event wakes nobody again. `ResetSignal` on an event that is not set
  returns `InvalidState`, `ClearEvent` never does.
- `CancelSynchronization`: if the thread is currently waiting in `WaitSynchronization`, the wait ends with `Cancelled`. Otherwise
  a flag is set, and the **next** `WaitSynchronization` ends immediately with `Cancelled` (only once).

## Scheduler model (ADR-0002) and what determinism needs

- Every guest thread is a host thread. A "slot" allows a thread to execute guest code: no slot (free), one
  slot per guest core (core baton) or a single slot (baton).
- A thread gives up its slot only at kernel boundaries: when blocking, on `SleepThread`, on YIELD hints, or
  when it makes a thread of higher priority ready in the same slot (preemption at the end of the SVC). Equal
  priorities are not time-sliced, as on Horizon.
- The next thread of a slot is always the one with the smallest (priority, enqueue order) value. The bookkeeping
  is always done by the waking thread under the scheduler lock, never by the woken host thread itself. That is why
  baton mode without timeouts is deterministic; timeouts run on the host clock and break it.
- Spin-waits without SVC: in baton mode one slot emulates several cores, so a spin can wait for a thread that
  would never get its turn. Countermeasure: `YIELD`/`WFE` hand the slot to **any** ready thread, even one of lower
  priority, and every 16384th loop back edge (`LGP_BACKEDGE`) does the same in baton mode. The back-edge count is
  the same per program path, so it stays deterministic.
- Process termination (`ExitProcess`, `Break`, crash of a thread): all waiting threads wake up with
  `TerminationRequested`, every SVC of a thread ends with thread exit, computing threads are stopped by the back-edge
  poll. After that all host threads are joined.
- If a thread entry function returns via `RET`, on Horizon it jumps to `x30` (nothing meaningful). The runtime
  treats that as a crash (`kHaltBadJump`); threads end regularly with `ExitThread`.
- **IPC is a scheduling point.** On Horizon the client blocks while another process serves the request;
  meanwhile lower-priority threads of its core run too. HLE services, in contrast, answer immediately in the guest thread.
  A thread that polls a service in a loop (LGP's audio thread, priority 43, core 0, queries `audout` without
  waiting) held its core slot forever in the baton modes and starved the main thread (44, core 0).
  Therefore the runtime hands the slot to the best ready thread of the core after every HLE request (`yield_hint`).
- Investigating hangs: `lgp_game --watchdog SEC` lists, after SEC seconds, every guest thread with state, core/mask,
  wait reason and frame chain (`Scheduler::thread_report`).
  Without aborting: if the presents stand still for 10 s after the first frame without the guest being paused, the
  boot program writes the same report once to the log, plus the HLE requests still running (`hle::requests_in_flight`),
  and the state of `audout` and `movie::`. The game keeps running. Threshold `--stall-report SEC`, off with `0`.
  For devices a debugger cannot reach (Apple TV, iPad); example in `docs/analysis/42-praesent-stillstand.md`.

## Pausing the process (app in the background)

On phones and tablets the game must stand still while the app is in the background: iOS terminates apps that
submit GPU work there, and Android otherwise drains the battery. Horizon likewise pauses an application in this situation; its
system time keeps running. `Scheduler::pause(timeout_ms)` does this from a host thread, `resume()` lifts it:

- Stopping points: entry and return of every SVC (`Kernel::svc`), every wake-up (`wait_for_running`) and the
  periodic poll at function entries (`LGP_BACKEDGE_ENTRY`, `backedge_poll`). In the default build back edges poll
  without a call (process termination only, `lifted-code-performance.md`), so they do not stop.
- `pause()` waits until no guest thread is running any more, i.e. each one waits on its condition variable in the scheduler
  (counter `active_`, `active_threads()`). A thread in pure guest code without SVCs and calls, or one that waits in
  host code for something else (an HLE service on a host thread), holds this up until the timeout; then
  the result is `false`, and the caller must not assume that nothing is running any more.
- Time: the guest clock keeps running (host mode). Timeouts during the pause wake the threads, which then wait at the
  next stopping point. The frame pacing (`FramePacer`) restarts after a gap of more than one frame.
- Process termination (`request_exit`) releases paused threads.
- Test: `test_pause` in `tests/runtime/test_threads_svc.cpp` (all three scheduler modes, host and virtual clock).

`Scheduler::pause` itself does not count. Multiple triggers (background, pause menu) go through
`boot::ProcessControl`, which counts pauses and only continues at the last `resume()`. The boot program side
(hooking into SDL's lifecycle events) is described in
[`ios-app-bauen.md`](ios-app-bauen.md) and [`android-ndk-build.md`](android-ndk-build.md).

## Platform pitfalls

- `std::thread` cannot set a stack size. Lifted code keeps the guest call depth on the host stack (ADR-0004),
  so every guest thread needs its own large host stack: `pthread_attr_setstacksize` or `_beginthreadex`.
  We take twice the guest stack (size of the memory block below the stack top), at least 1 MiB, at most
  16 MiB; the ratio is estimated, not measured.
- Emscripten: `-pthread` must apply to **all** objects (shared memory, atomics), i.e. globally. If
  `main()` blocks (waits for guest threads), it needs `-sPROXY_TO_PTHREAD` (main runs in a worker) and a
  pre-created pool (`-sPTHREAD_POOL_SIZE`), otherwise `pthread_create` hangs. Without `-sEXIT_RUNTIME=1` the pool keeps
  Node alive after `main()`, and every test runs into the timeout.
- ThreadSanitizer reports races on guest memory (exclusive reads are normal loads, the kernel writes
  mutex words atomically). These are guest races, as they also happen on the hardware; in the scheduler itself
  TSan found none.
- `llvm-mc`: global symbols get a relocation in the object; `BL`/`ADR` to them then appear in the raw `.text` as a
  jump to themselves. For test programs without a linker, keep the labels file-local.

## Open / not checked

- Our thread ids count from 1; Horizon assigns different, cross-process values.
- Core −1 ("any") gets the lowest allowed core as a fixed core with us.
- TLS is freed at thread exit, on Horizon only when the last handle is closed.
- The mode is chosen at process start (`ProcessConfig::sched_mode`); switching in a running process has not
  been built yet.
