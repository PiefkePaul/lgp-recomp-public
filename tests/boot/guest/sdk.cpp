// "sdk" of the SDK-shaped boot test pair: exports nn::init::Start like the
// Nintendo SDK does and plays its part of the process start (oss-rtld's
// __rtld_start_app calls it with two callbacks). Own code, freestanding.
#include "guest.hpp"

namespace nn::ro::detail {
ModuleObjectList* g_pAutoLoadList;
ModuleObjectList* g_pManualLoadList;
bool* g_pRoDebugFlag;
uintptr_t (*g_pLookupGlobalAutoFunctionPointer)(const char* name);
}  // namespace nn::ro::detail

uint32_t lgp_sdk_counter;
static char g_order[8];
static uint32_t g_order_len;
static bool g_notified;

void lgp_record_init(char who) {
    if (g_order_len + 1 < sizeof(g_order))
        g_order[g_order_len++] = who;
}

// DT_INIT of this module (linked with -init).
extern "C" __attribute__((used)) void lgp_sdk_init() {
    lgp_record_init('S');
    lgp_sdk_counter += 41;
}

namespace nn::init {
void Start(uint32_t thread_handle, uintptr_t argument_address, void (*notify_exception_handler_ready)(),
           void (*call_initializator)()) {
    lgp_print("sdk: nn::init::Start");
    lgp_check("sdk: main thread handle", thread_handle != 0);
    // Like the SDK's nnosInitialize: arguments are parsed only if the argument
    // address is mapped RW; otherwise argc/argv keep their built-in defaults
    // (argc 1). Without arguments the area must not look like an empty one.
    uint64_t info[5] = {};  // MemoryInfo: addr, size, u32 state, attr, perm, ...
    const uint32_t rc = lgp_svc_query_memory(info, 0, argument_address);
    const uint32_t perm = static_cast<uint32_t>(info[3]);
    lgp_check("sdk: no argument area", argument_address != 0 && rc == 0 && perm != 3);
    notify_exception_handler_ready();
    g_notified = true;
    call_initializator();
    lgp_check("sdk: initializers sdk before main", g_order_len == 2 && g_order[0] == 'S' && g_order[1] == 'M');
    lgp_check("sdk: nn::ro lists bound", ro::detail::g_pAutoLoadList != nullptr &&
                                             ro::detail::g_pAutoLoadList->front == ro::detail::g_pAutoLoadList &&
                                             ro::detail::g_pManualLoadList != nullptr && ro::detail::g_pRoDebugFlag &&
                                             !*ro::detail::g_pRoDebugFlag);
    nnMain();
    lgp_print("sdk: nnMain returned");
    lgp_svc_exit_process();
}
}  // namespace nn::init

// The real driver sets itself up here; these only report that they ran
// (GraphicsMode::Continue). With the NVN-HLE the boot program skips them.
namespace nv {
void SetGraphicsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void*) {
    lgp_print("sdk: driver nv::SetGraphicsAllocator ran");
}
void InitializeGraphics(void*, size_t) { lgp_print("sdk: driver nv::InitializeGraphics ran"); }
void FinalizeGraphics() { lgp_print("sdk: driver nv::FinalizeGraphics ran"); }
}  // namespace nv

// The real one hands out NVN function pointers; this one only reports that
// the driver ran (GraphicsMode::Continue).
void* nvnBootstrapLoader(const char* name) {
    lgp_check("sdk: driver nvnBootstrapLoader called after start", g_notified && name != nullptr);
    return nullptr;
}

namespace nn::ngc {
uint32_t ProfanityFilter::Initialize(void*, size_t) {
    state_ = 1;
    return 0x7d402;  // TargetNotFound: no NgWord2 to mount
}
void ProfanityFilter::Finalize() { state_ = 0; }
uint32_t ProfanityFilter::GetContentVersion() { return 0x1a + state_; }
uint32_t ProfanityFilter::CheckProfanityWords(uint32_t* masks, uint32_t, const char**, size_t n) {
    for (size_t i = 0; i < n; ++i)
        masks[i] = 0xffffffffu;
    return 0;
}
uint32_t ProfanityFilter::MaskProfanityWordsInText(int* masked, char* text, uint32_t) {
    *masked = 1;
    text[0] = '*';
    return 0;
}
}  // namespace nn::ngc
