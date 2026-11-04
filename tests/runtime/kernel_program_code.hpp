// Own A64 test programs for the kernel (P3.1/P2.6), shared by the Unicorn
// executor, the lifter-based generator and the lifted executor. Assembled with
// llvm-mc (-triple=aarch64); the source is next to each word. No dependencies,
// so the host-side generator can include it without the runtime.
#pragma once

#include <cstdint>
#include <vector>

namespace lgp_test {

inline constexpr uint64_t kProgramBase = 0x0800'0000;  // start of the code region

struct ProgramCode {
    const char* name;
    std::vector<uint32_t> words;  // instructions, lifted as one function from kProgramBase
    std::vector<uint8_t> data;    // bytes placed right after the instructions
};

inline const std::vector<ProgramCode>& program_codes() {
    static const std::vector<ProgramCode> programs = {
        {"memory_svcs",
         {
             0xd2800081,  // mov x1, #4                      GetInfo(HeapRegionAddress)
             0xd2900022,  // mov x2, #0x8001
             0xf2bfffe2,  // movk x2, #0xffff, lsl #16       x2 = current process
             0xd2800003,  // mov x3, #0
             0xd4000521,  // svc #0x29
             0xaa0003f3,  // mov x19, x0
             0xaa0103f4,  // mov x20, x1
             0xd2a00401,  // mov x1, #0x200000               SetHeapSize(2 MiB)
             0xd4000021,  // svc #0x01
             0xaa000273,  // orr x19, x19, x0
             0xaa0103f5,  // mov x21, x1
             0xd2824689,  // movz x9, #0x1234
             0xf2aacf09,  // movk x9, #0x5678, lsl #16
             0xf90002a9,  // str x9, [x21]
             0xf90006a9,  // str x9, [x21, #8]
             0x910402a0,  // add x0, x21, #0x100             QueryMemory(info = heap+0x100, addr = heap)
             0xaa1503e2,  // mov x2, x21
             0xd40000c1,  // svc #0x06
             0xaa000273,  // orr x19, x19, x0
             0xb94112b6,  // ldr w22, [x21, #0x110]          info.state
             0xb9411ab7,  // ldr w23, [x21, #0x118]          info.perm
             0xaa1803e0,  // mov x0, x24                     MapMemory(dst = x24, src = heap, 0x1000)
             0xaa1503e1,  // mov x1, x21
             0xd2820002,  // mov x2, #0x1000
             0xd4000081,  // svc #0x04
             0xaa000273,  // orr x19, x19, x0
             0xf9400319,  // ldr x25, [x24]                  contents moved with the mapping
             0xd29579a9,  // mov x9, #0xabcd
             0xf9000709,  // str x9, [x24, #8]               write through the stack mapping
             0xaa1803e0,  // mov x0, x24                     UnmapMemory
             0xaa1503e1,  // mov x1, x21
             0xd2820002,  // mov x2, #0x1000
             0xd40000a1,  // svc #0x05
             0xaa000273,  // orr x19, x19, x0
             0xf94006ba,  // ldr x26, [x21, #8]              the write is back in the heap
             0xaa1503e0,  // mov x0, x21                     SetMemoryAttribute(heap, 0x1000, Uncached, Uncached)
             0xd2820001,  // mov x1, #0x1000
             0xd2800102,  // mov x2, #8
             0xd2800103,  // mov x3, #8
             0xd4000061,  // svc #0x03
             0xaa000273,  // orr x19, x19, x0
             0x100000a0,  // adr x0, msg                     OutputDebugString("ok", 2)
             0xd2800041,  // mov x1, #2
             0xd40004e1,  // svc #0x27
             0xaa000273,  // orr x19, x19, x0
             0xd40000e1,  // svc #0x07                       ExitProcess
         },
         {'o', 'k', 0, 0}},
        {"transfer_memory",
         {
             0xd2a00401,  // mov x1, #0x200000               SetHeapSize(2 MiB)
             0xd4000021,  // svc #0x01
             0xaa0103f5,  // mov x21, x1
             0xaa1503e1,  // mov x1, x21                     CreateTransferMemory(heap, 0x1000, perm None)
             0xd2820002,  // mov x2, #0x1000
             0xd2800003,  // mov x3, #0
             0xd40002a1,  // svc #0x15
             0xaa0003f3,  // mov x19, x0
             0xaa0103f6,  // mov x22, x1                     handle
             0xaa1603e0,  // mov x0, x22                     CloseHandle(handle)
             0xd40002c1,  // svc #0x16
             0xaa000273,  // orr x19, x19, x0
             0x914042a0,  // add x0, x21, #0x10, lsl #12     MapMemory(dst in the heap) -> InvalidMemoryRegion
             0xaa1503e1,  // mov x1, x21
             0xd2820002,  // mov x2, #0x1000
             0xd4000081,  // svc #0x04
             0xaa0003f4,  // mov x20, x0
             0xaa1503e1,  // mov x1, x21                     CreateTransferMemory again, keep the handle
             0xd2820002,  // mov x2, #0x1000
             0xd2800003,  // mov x3, #0
             0xd40002a1,  // svc #0x15
             0xaa000273,  // orr x19, x19, x0
             0xf94002a9,  // ldr x9, [x21]                   lent with perm None: must fault
             0xd40000e1,  // svc #0x07
         },
         {}},
        {"unimplemented_svc",
         {
             0xd4000fe1,  // svc #0x7f (no such svc for applications)
             0xd40000e1,  // svc #0x07
         },
         {}},
    };
    return programs;
}

}  // namespace lgp_test
