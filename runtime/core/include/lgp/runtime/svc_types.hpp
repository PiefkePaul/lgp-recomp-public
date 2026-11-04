// Guest-visible kernel types: memory states, permissions, attributes, the
// MemoryInfo layout written by svcQueryMemory, svc numbers and GetInfo ids.
//
// Values are the Horizon ABI as documented by switchbrew and used by libnx;
// they are interface facts, no code was taken from another project.
#pragma once

#include <cstdint>

namespace lgp::rt {

// Low byte of the kernel memory state, i.e. what svcQueryMemory reports.
enum class MemState : uint8_t {
    Free = 0x00,
    Io = 0x01,
    Static = 0x02,
    Code = 0x03,              // .text/.rodata of a loaded module
    CodeData = 0x04,          // .data/.bss of a loaded module
    Normal = 0x05,            // heap (svcSetHeapSize)
    Shared = 0x06,            // shared memory (svcMapSharedMemory)
    AliasCode = 0x08,
    AliasCodeData = 0x09,
    Ipc = 0x0A,
    Stack = 0x0B,             // svcMapMemory destination (thread stacks)
    ThreadLocal = 0x0C,       // TLS pages
    Transfered = 0x0D,
    SharedTransfered = 0x0E,
    SharedCode = 0x0F,
    Inaccessible = 0x10,      // outside the address space
    NonSecureIpc = 0x11,
    NonDeviceIpc = 0x12,
    Kernel = 0x13,
    GeneratedCode = 0x14,
    CodeOut = 0x15,
};

enum MemPerm : uint8_t {
    kPermNone = 0,
    kPermR = 1,
    kPermW = 2,
    kPermX = 4,
    kPermRW = kPermR | kPermW,
    kPermRX = kPermR | kPermX,
};

enum MemAttr : uint8_t {
    kAttrNone = 0,
    kAttrLocked = 1,
    kAttrIpcLocked = 2,
    kAttrDeviceShared = 4,
    kAttrUncached = 8,
};

// svcQueryMemory output (0x28 bytes in guest memory).
struct MemoryInfo {
    uint64_t addr;
    uint64_t size;
    uint32_t state;  // MemState
    uint32_t attr;   // MemAttr bits
    uint32_t perm;   // MemPerm bits
    uint32_t ipc_refcount;
    uint32_t device_refcount;
    uint32_t padding;
};
static_assert(sizeof(MemoryInfo) == 0x28);

// Supervisor call numbers (immediate of the SVC instruction).
enum Svc : uint32_t {
    kSvcSetHeapSize = 0x01,
    kSvcSetMemoryPermission = 0x02,
    kSvcSetMemoryAttribute = 0x03,
    kSvcMapMemory = 0x04,
    kSvcUnmapMemory = 0x05,
    kSvcQueryMemory = 0x06,
    kSvcExitProcess = 0x07,
    kSvcCreateThread = 0x08,
    kSvcStartThread = 0x09,
    kSvcExitThread = 0x0A,
    kSvcSleepThread = 0x0B,
    kSvcGetThreadPriority = 0x0C,
    kSvcSetThreadPriority = 0x0D,
    kSvcGetThreadCoreMask = 0x0E,
    kSvcSetThreadCoreMask = 0x0F,
    kSvcGetCurrentProcessorNumber = 0x10,
    kSvcSignalEvent = 0x11,
    kSvcClearEvent = 0x12,
    kSvcMapSharedMemory = 0x13,
    kSvcUnmapSharedMemory = 0x14,
    kSvcCreateTransferMemory = 0x15,
    kSvcCloseHandle = 0x16,
    kSvcResetSignal = 0x17,
    kSvcWaitSynchronization = 0x18,
    kSvcCancelSynchronization = 0x19,
    kSvcArbitrateLock = 0x1A,
    kSvcArbitrateUnlock = 0x1B,
    kSvcWaitProcessWideKeyAtomic = 0x1C,
    kSvcSignalProcessWideKey = 0x1D,
    kSvcGetSystemTick = 0x1E,
    kSvcConnectToNamedPort = 0x1F,
    kSvcSendSyncRequest = 0x21,
    kSvcSendSyncRequestWithUserBuffer = 0x22,
    kSvcGetProcessId = 0x24,
    kSvcGetThreadId = 0x25,
    kSvcBreak = 0x26,
    kSvcOutputDebugString = 0x27,
    kSvcGetInfo = 0x29,
    kSvcMapPhysicalMemory = 0x2C,
    kSvcUnmapPhysicalMemory = 0x2D,
    kSvcWaitForAddress = 0x34,
    kSvcSignalToAddress = 0x35,
    kSvcCreateEvent = 0x45,
};

// svcGetInfo first id.
enum InfoType : uint32_t {
    kInfoCoreMask = 0,
    kInfoPriorityMask = 1,
    kInfoAliasRegionAddress = 2,
    kInfoAliasRegionSize = 3,
    kInfoHeapRegionAddress = 4,
    kInfoHeapRegionSize = 5,
    kInfoTotalMemorySize = 6,
    kInfoUsedMemorySize = 7,
    kInfoDebuggerAttached = 8,
    kInfoResourceLimit = 9,
    kInfoIdleTickCount = 10,
    kInfoRandomEntropy = 11,
    kInfoAslrRegionAddress = 12,
    kInfoAslrRegionSize = 13,
    kInfoStackRegionAddress = 14,
    kInfoStackRegionSize = 15,
    kInfoSystemResourceSizeTotal = 16,
    kInfoSystemResourceSizeUsed = 17,
    kInfoProgramId = 18,
    kInfoInitialProcessIdRange = 19,
    kInfoUserExceptionContextAddress = 20,
    kInfoTotalNonSystemMemorySize = 21,
    kInfoUsedNonSystemMemorySize = 22,
    kInfoIsApplication = 23,
};

// Handle values.
using Handle = uint32_t;
inline constexpr Handle kInvalidHandle = 0;
inline constexpr Handle kPseudoHandleCurrentThread = 0xFFFF8000;
inline constexpr Handle kPseudoHandleCurrentProcess = 0xFFFF8001;

}  // namespace lgp::rt
