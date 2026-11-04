// Own A64 test programs for the scheduler (P3.2) and IPC (P3.3). Assembled by
// gen_thread_programs.py into thread_program_code.hpp (llvm-mc, AArch64).
//
// One image at the start of the code region; each test runs one of the *_main
// functions as the main thread (Horizon entry ABI: x0 = 0, x1 = own handle).
// Every label without the .L prefix is a function the lifter lifts (labels
// stay file-local so llvm-mc resolves BL/ADR without relocations). Mutexes
// and condition variables follow the Horizon user-space protocol (libnx,
// nn::os): exclusive load/store fast paths, svcArbitrateLock/Unlock and
// svcWait/SignalProcessWideKey on contention.
//
// Data page D = 0x08010000 (mapped RW by the test):
//   +0x000 mutex          +0x004 cv "not empty"   +0x008 queue count
//   +0x00c cv "not full"  +0x010 queue tail       +0x014 queue head
//   +0x018 consumer sum   +0x01c log length       +0x020 done flag
//   +0x024 failing result +0x028 spin flag        +0x02c IPC failing step
//   +0x030 IPC result     +0x040 ring (4 words)   +0x060 port name "sm:"
//   +0x100 thread structs, 16 bytes each: handle (u32), parameter (u32)
//   +0x1000 log (u32 entries)
//   +0x8000 thread stacks, 0x2000 each (tops at +0xa000, +0xc000, +0xe000, +0x10000)

    .text
    .p2align 2

// --- library ---------------------------------------------------------------------

// mutex_lock(x0 = mutex, w1 = own handle)
mutex_lock:
    mov     x9, x0
    mov     w10, w1
.Llock_retry:
    ldaxr   w11, [x9]
    cbnz    w11, .Llock_busy
    stxr    w12, w10, [x9]
    cbnz    w12, .Llock_retry
    ret
.Llock_busy:
    tbnz    w11, #30, .Llock_wait
    orr     w13, w11, #0x40000000
    stxr    w12, w13, [x9]
    cbnz    w12, .Llock_retry
    mov     w11, w13
.Llock_wait:
    and     w0, w11, #0xbfffffff        // owner handle
    mov     x1, x9
    mov     w2, w10
    svc     #0x1a                       // ArbitrateLock
    ldar    w11, [x9]                   // handed over to us? (owner field == own handle)
    and     w11, w11, #0xbfffffff
    cmp     w11, w10
    b.ne    .Llock_retry
    ret

// mutex_unlock(x0 = mutex, w1 = own handle)
mutex_unlock:
    mov     x9, x0
.Lunlock_retry:
    ldxr    w11, [x9]
    cmp     w11, w1
    b.ne    .Lunlock_slow               // waiters bit set
    stlxr   w12, wzr, [x9]
    cbnz    w12, .Lunlock_retry
    ret
.Lunlock_slow:
    clrex
    mov     x0, x9
    svc     #0x1b                       // ArbitrateUnlock
    ret

// cv_wait(x0 = cv key, x1 = mutex, w2 = own handle), no timeout
cv_wait:
    mov     x9, x0
    mov     x0, x1
    mov     x1, x9
    mov     x3, #-1
    svc     #0x1c                       // WaitProcessWideKeyAtomic
    ret

// cv_signal(x0 = cv key, w1 = count)
cv_signal:
    svc     #0x1d                       // SignalProcessWideKey
    ret

// log_event(x0 = D, w1 = value): appends to the log (atomic index).
log_event:
    add     x11, x0, #0x1c
.Llog_retry:
    ldaxr   w9, [x11]
    add     w12, w9, #1
    stlxr   w13, w12, [x11]
    cbnz    w13, .Llog_retry
    add     x10, x0, #0x1000
    str     w1, [x10, w9, uxtw #2]
    ret

// --- producer/consumer: bounded queue of 4, one producer, one consumer ------------

// producer(x0 = struct: handle, item count): puts 1..count.
producer:
    mov     x19, x0
    ldr     w20, [x19]
    ldr     w23, [x19, #4]
    mov     x21, #0x08010000
    mov     w22, #1
.Lp_loop:
    mov     x0, x21
    mov     w1, w20
    bl      mutex_lock
.Lp_full:
    ldr     w8, [x21, #8]
    cmp     w8, #4
    b.lo    .Lp_put
    add     x0, x21, #0xc
    mov     x1, x21
    mov     w2, w20
    bl      cv_wait
    b       .Lp_full
.Lp_put:
    ldr     w9, [x21, #0x10]
    add     x10, x21, #0x40
    str     w22, [x10, w9, uxtw #2]
    add     w9, w9, #1
    and     w9, w9, #3
    str     w9, [x21, #0x10]
    ldr     w8, [x21, #8]
    add     w8, w8, #1
    str     w8, [x21, #8]
    mov     x0, x21
    mov     w1, w22
    bl      log_event
    add     x0, x21, #4
    mov     w1, #1
    bl      cv_signal
    mov     x0, x21
    mov     w1, w20
    bl      mutex_unlock
    cmp     w22, w23
    add     w22, w22, #1
    b.ne    .Lp_loop
    svc     #0x0a                       // ExitThread

// consumer(x0 = struct: handle, item count): sums the items into D+0x18.
consumer:
    mov     x19, x0
    ldr     w20, [x19]
    ldr     w23, [x19, #4]
    mov     x21, #0x08010000
    mov     w22, #0
.Lc_loop:
    mov     x0, x21
    mov     w1, w20
    bl      mutex_lock
.Lc_empty:
    ldr     w8, [x21, #8]
    cbnz    w8, .Lc_take
    add     x0, x21, #4
    mov     x1, x21
    mov     w2, w20
    bl      cv_wait
    b       .Lc_empty
.Lc_take:
    ldr     w9, [x21, #0x14]
    add     x10, x21, #0x40
    ldr     w24, [x10, w9, uxtw #2]
    add     w9, w9, #1
    and     w9, w9, #3
    str     w9, [x21, #0x14]
    ldr     w8, [x21, #8]
    sub     w8, w8, #1
    str     w8, [x21, #8]
    ldr     w11, [x21, #0x18]
    add     w11, w11, w24
    str     w11, [x21, #0x18]
    mov     x0, x21
    orr     w1, w24, #0x80000000
    bl      log_event
    add     x0, x21, #0xc
    mov     w1, #1
    bl      cv_signal
    mov     x0, x21
    mov     w1, w20
    bl      mutex_unlock
    add     w22, w22, #1
    cmp     w22, w23
    b.ne    .Lc_loop
    svc     #0x0a                       // ExitThread

// Main: consumer and producer at priority 44 on the default core, 200 items.
pc_main:
    mov     x21, #0x08010000
    mov     w26, #200
    adr     x1, consumer
    add     x2, x21, #0x100
    add     x3, x21, #0xa000
    mov     w4, #44
    mov     w5, #-2
    svc     #0x08                       // CreateThread
    cbnz    w0, .Lpc_fail
    str     w1, [x21, #0x100]
    str     w26, [x21, #0x104]
    adr     x1, producer
    add     x2, x21, #0x110
    add     x3, x21, #0xc000
    mov     w4, #44
    mov     w5, #-2
    svc     #0x08
    cbnz    w0, .Lpc_fail
    str     w1, [x21, #0x110]
    str     w26, [x21, #0x114]
    ldr     w0, [x21, #0x100]
    svc     #0x09                       // StartThread
    cbnz    w0, .Lpc_fail
    ldr     w0, [x21, #0x110]
    svc     #0x09
    cbnz    w0, .Lpc_fail
    add     x1, x21, #0x100
    mov     w2, #1
    mov     x3, #-1
    svc     #0x18                       // WaitSynchronization(consumer)
    cbnz    w0, .Lpc_fail
    add     x1, x21, #0x110
    mov     w2, #1
    mov     x3, #-1
    svc     #0x18                       // WaitSynchronization(producer)
    cbnz    w0, .Lpc_fail
    mov     w9, #1
    str     w9, [x21, #0x20]
    svc     #0x07                       // ExitProcess
.Lpc_fail:
    str     w0, [x21, #0x24]
    svc     #0x07

// --- priorities: three workers, logged in the order they run ----------------------

// prio_worker(x0 = struct: handle, value to log)
prio_worker:
    ldr     w1, [x0, #4]
    mov     x0, #0x08010000
    bl      log_event
    svc     #0x0a

// Main at priority 44 starts workers at 50, 45, 40 (all on the default core),
// logs 1, then waits for them. Expected with one slot per core: 40 preempts
// main at its StartThread, then main logs 1, then 45 and 50 run while main
// waits: 40, 1, 45, 50.
prio_main:
    mov     x21, #0x08010000
    mov     w26, #50
    add     x22, x21, #0x100
    mov     x23, #0xa000
.Lprio_create:
    str     w26, [x22, #4]
    adr     x1, prio_worker
    mov     x2, x22
    add     x3, x21, x23
    mov     w4, w26
    mov     w5, #-2
    svc     #0x08
    cbnz    w0, .Lprio_fail
    str     w1, [x22]
    add     x22, x22, #0x10
    add     x23, x23, #0x2000
    sub     w26, w26, #5
    cmp     w26, #35
    b.ne    .Lprio_create
    ldr     w0, [x21, #0x100]
    svc     #0x09
    cbnz    w0, .Lprio_fail
    ldr     w0, [x21, #0x110]
    svc     #0x09
    cbnz    w0, .Lprio_fail
    ldr     w0, [x21, #0x120]
    svc     #0x09
    cbnz    w0, .Lprio_fail
    mov     x0, x21
    mov     w1, #1
    bl      log_event
    add     x1, x21, #0x100
    mov     w2, #1
    mov     x3, #-1
    svc     #0x18
    cbnz    w0, .Lprio_fail
    add     x1, x21, #0x110
    mov     w2, #1
    mov     x3, #-1
    svc     #0x18
    cbnz    w0, .Lprio_fail
    add     x1, x21, #0x120
    mov     w2, #1
    mov     x3, #-1
    svc     #0x18
    cbnz    w0, .Lprio_fail
    mov     w9, #1
    str     w9, [x21, #0x20]
    svc     #0x07
.Lprio_fail:
    str     w0, [x21, #0x24]
    svc     #0x07

// --- spin-waits without svc --------------------------------------------------------

// spin_worker(x0 = struct): sets the spin flag and exits.
spin_worker:
    mov     x21, #0x08010000
    mov     w9, #1
    add     x10, x21, #0x28
    stlr    w9, [x10]
    svc     #0x0a

// Main at priority 44 starts a worker at priority 50 on core 1 and spins until
// it has set the flag: first with YIELD hints, then (second worker) with a bare
// loop that only the back-edge poll can interrupt in Baton mode.
spin_main:
    mov     x21, #0x08010000
    add     x22, x21, #0x28
    mov     x24, #0                     // round
.Lspin_round:
    str     wzr, [x22]
    adr     x1, spin_worker
    add     x2, x21, #0x100
    add     x3, x21, #0xa000
    mov     w4, #50
    mov     w5, #1
    svc     #0x08
    cbnz    w0, .Lspin_fail
    mov     w25, w1
    mov     w0, w1
    svc     #0x09
    cbnz    w0, .Lspin_fail
    cbnz    x24, .Lspin_bare
.Lspin_yield:
    ldar    w9, [x22]
    cbnz    w9, .Lspin_done
    yield
    b       .Lspin_yield
.Lspin_bare:
    ldar    w9, [x22]
    cbz     w9, .Lspin_bare
.Lspin_done:
    str     w25, [x21, #0x100]
    add     x1, x21, #0x100
    mov     w2, #1
    mov     x3, #-1
    svc     #0x18                       // wait for the worker to exit
    cbnz    w0, .Lspin_fail
    mov     w0, w25
    svc     #0x16                       // CloseHandle
    add     x24, x24, #1
    cmp     x24, #2
    b.ne    .Lspin_round
    mov     w9, #1
    str     w9, [x21, #0x20]
    svc     #0x07
.Lspin_fail:
    str     w0, [x21, #0x24]
    svc     #0x07

// --- IPC (P3.3) --------------------------------------------------------------------

// ipc_main: connects to "sm:", RegisterClient, GetServiceHandle("lgp:test"),
// calls command 0 (add) of that service with 2 and 3, closes both sessions.
// Messages are built in the TLS like libnx's cmifMakeRequest (HIPC header,
// raw data 16-byte aligned from the message start, CMIF "SFCI" header).
// Results: +0x030 sum from the service, +0x02c the failing step.
ipc_main:
    mov     x21, #0x08010000
    mrs     x20, tpidrro_el0
    movz    w27, #0x4653                // "SFCI"
    movk    w27, #0x4943, lsl #16
    movz    w28, #0x4653                // "SFCO"
    movk    w28, #0x4f43, lsl #16
    mov     w26, #1                     // step 1: svcConnectToNamedPort("sm:")
    movz    w9, #0x6d73
    movk    w9, #0x3a, lsl #16
    str     x9, [x21, #0x60]
    add     x1, x21, #0x60
    svc     #0x1f
    cbnz    w0, .Lipc_fail
    mov     w23, w1
    mov     w26, #2                     // step 2: sm RegisterClient(pid, 0)
    mov     w9, #4                      // type Request, no buffers
    str     w9, [x20]
    movz    w9, #10                     // 10 data words, special header
    movk    w9, #0x8000, lsl #16
    str     w9, [x20, #4]
    mov     w9, #1                      // send pid
    str     w9, [x20, #8]
    str     xzr, [x20, #0xc]            // pid placeholder (the kernel fills it in)
    str     w27, [x20, #0x20]
    str     wzr, [x20, #0x24]
    str     wzr, [x20, #0x28]           // command 0
    str     wzr, [x20, #0x2c]
    str     xzr, [x20, #0x30]
    mov     w0, w23
    svc     #0x21
    cbnz    w0, .Lipc_fail
    ldr     w9, [x20, #0x10]            // reply: no handles, raw data at 0x10
    cmp     w9, w28
    b.ne    .Lipc_bad
    ldr     w0, [x20, #0x18]
    cbnz    w0, .Lipc_fail
    mov     w26, #3                     // step 3: sm GetServiceHandle("lgp:test")
    mov     w9, #4
    str     w9, [x20]
    mov     w9, #10
    str     w9, [x20, #4]
    str     w27, [x20, #0x10]
    str     wzr, [x20, #0x14]
    mov     w9, #1                      // command 1
    str     w9, [x20, #0x18]
    str     wzr, [x20, #0x1c]
    movz    x9, #0x676c                 // "lgp:test"
    movk    x9, #0x3a70, lsl #16
    movk    x9, #0x6574, lsl #32
    movk    x9, #0x7473, lsl #48
    str     x9, [x20, #0x20]
    mov     w0, w23
    svc     #0x21
    cbnz    w0, .Lipc_fail
    ldr     w9, [x20, #0x10]            // raw data at 0x10 with or without the handle
    cmp     w9, w28
    b.ne    .Lipc_bad
    ldr     w0, [x20, #0x18]
    cbnz    w0, .Lipc_fail
    ldr     w9, [x20, #8]               // special header: one move handle
    cmp     w9, #0x20
    b.ne    .Lipc_bad
    ldr     w24, [x20, #0xc]
    mov     w26, #4                     // step 4: lgp:test command 0, add(2, 3)
    mov     w9, #4
    str     w9, [x20]
    mov     w9, #10
    str     w9, [x20, #4]
    str     w27, [x20, #0x10]
    str     wzr, [x20, #0x14]
    str     wzr, [x20, #0x18]
    str     wzr, [x20, #0x1c]
    mov     w9, #2
    str     w9, [x20, #0x20]
    mov     w9, #3
    str     w9, [x20, #0x24]
    mov     w0, w24
    svc     #0x21
    cbnz    w0, .Lipc_fail
    ldr     w9, [x20, #0x10]
    cmp     w9, w28
    b.ne    .Lipc_bad
    ldr     w0, [x20, #0x18]
    cbnz    w0, .Lipc_fail
    ldr     w9, [x20, #0x20]
    str     w9, [x21, #0x30]
    mov     w26, #5                     // step 5: close (HIPC type 2), then the handles
    mov     w9, #2
    str     w9, [x20]
    str     wzr, [x20, #4]
    mov     w0, w24
    svc     #0x21
    cbnz    w0, .Lipc_fail
    mov     w0, w24
    svc     #0x16
    cbnz    w0, .Lipc_fail
    mov     w0, w23
    svc     #0x16
    cbnz    w0, .Lipc_fail
    mov     w9, #1
    str     w9, [x21, #0x20]
    svc     #0x07
.Lipc_bad:
    mov     w0, #0xbad
.Lipc_fail:
    str     w0, [x21, #0x24]
    str     w26, [x21, #0x2c]
    svc     #0x07
