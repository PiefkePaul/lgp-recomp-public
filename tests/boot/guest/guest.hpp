// Shared helpers of the SDK-shaped boot test modules (freestanding, no libc).
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LGP_EXPORT __attribute__((visibility("default")))

extern "C" void lgp_svc_output_debug_string(const char* s, size_t n);
extern "C" [[noreturn]] void lgp_svc_exit_process();
// svcQueryMemory(MemoryInfo* out, PageInfo (x1, out), address); returns the result.
extern "C" uint32_t lgp_svc_query_memory(void* info, uint64_t unused, uintptr_t addr);

inline size_t lgp_strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

inline void lgp_print(const char* s) { lgp_svc_output_debug_string(s, lgp_strlen(s)); }

// "<prefix><0/1>" style check line.
inline void lgp_check(const char* what, bool ok) {
    char buf[96];
    size_t n = 0;
    for (const char* p = what; *p != '\0' && n < 80; ++p)
        buf[n++] = *p;
    const char* verdict = ok ? ": ok" : ": FAILED";
    for (const char* p = verdict; *p != '\0'; ++p)
        buf[n++] = *p;
    lgp_svc_output_debug_string(buf, n);
}

// What rtld hands the SDK; declared like the SDK does (names only matter).
namespace nn::init {
LGP_EXPORT void Start(uint32_t thread_handle, uintptr_t argument_address, void (*notify_exception_handler_ready)(),
                      void (*call_initializator)());
}
namespace nn::ro::detail {
struct ModuleObjectList {
    void* front;
    void* back;
};
extern LGP_EXPORT ModuleObjectList* g_pAutoLoadList;
extern LGP_EXPORT ModuleObjectList* g_pManualLoadList;
extern LGP_EXPORT bool* g_pRoDebugFlag;
extern LGP_EXPORT uintptr_t (*g_pLookupGlobalAutoFunctionPointer)(const char* name);
}  // namespace nn::ro::detail

// The driver setup main imports from sdk besides nvnBootstrapLoader (what
// games do, docs/knowledge/nvn-bootstrap-und-treiber.md).
namespace nv {
using AllocateFunction = void* (*)(size_t size, size_t alignment, void* user);
using FreeFunction = void (*)(void* p, void* user);
using ReallocateFunction = void* (*)(void* p, size_t size, void* user);
LGP_EXPORT void SetGraphicsAllocator(AllocateFunction allocate, FreeFunction free, ReallocateFunction reallocate,
                                     void* user);
LGP_EXPORT void InitializeGraphics(void* memory, size_t size);
LGP_EXPORT void FinalizeGraphics();
}  // namespace nv

// The SDK's profanity filter (nn::ngc), with the mangled names of the real
// one: the lift gives these a hook door (lgp/recomp/sdk_hooks.hpp), and
// without systemdata/0100000000000823 the boot program answers them on the
// host. This one plays an SDK without the NgWord2 archive: Initialize fails
// (as the mount would), every word is a profanity, every text is masked.
namespace nn::ngc {
class ProfanityFilter {
public:
    LGP_EXPORT uint32_t Initialize(void* work, size_t size);
    LGP_EXPORT void Finalize();
    LGP_EXPORT uint32_t GetContentVersion();
    LGP_EXPORT uint32_t CheckProfanityWords(uint32_t* masks, uint32_t patterns, const char** words, size_t n);
    LGP_EXPORT uint32_t MaskProfanityWordsInText(int* masked, char* text, uint32_t patterns);

private:
    uint32_t state_ = 0;
};
}  // namespace nn::ngc

extern "C" {
LGP_EXPORT void nnMain();
LGP_EXPORT void* nvnBootstrapLoader(const char* name);
// Order in which the module initializers ran ("S" sdk, "M" main).
LGP_EXPORT void lgp_record_init(char who);
LGP_EXPORT extern uint32_t lgp_sdk_counter;
}
