// "main" of the SDK-shaped boot test pair: the game side. Imports from sdk
// (functions through the PLT, data through the GOT), its own initializer and
// nnMain, which sets up graphics like a game: nv:: driver setup, then
// nvnBootstrapLoader. With the NVN-HLE it renders the frames of the P4.1
// guest program (tests/nvn/guest/nvn_clear.cpp, linked into this module).
// Own code, freestanding.
#include "guest.hpp"

static uint32_t g_main_inits;

// tests/nvn/guest/nvn_clear.cpp: 3 frames, 0 on success.
using PfnBootstrap = void (*(*)(const char*))();
extern "C" int guest_main(PfnBootstrap bootstrap);

extern "C" __attribute__((used)) void lgp_main_init() {
    lgp_record_init('M');
    ++g_main_inits;
}

void nnMain() {
    lgp_print("main: nnMain");
    lgp_check("main: own initializer ran once", g_main_inits == 1);
    lgp_check("main: sdk data import", lgp_sdk_counter == 41);
    auto lookup = nn::ro::detail::g_pLookupGlobalAutoFunctionPointer;
    lgp_check("main: nn::ro symbol lookup",
              lookup != nullptr && lookup("nnMain") == reinterpret_cast<uintptr_t>(&nnMain) &&
                  lookup("lgp_no_such_symbol") == 0);
    // A name check like LGP's after the intro (docs/analysis/17).
    nn::ngc::ProfanityFilter filter;
    const uint32_t init = filter.Initialize(nullptr, 0);
    const char* words[2] = {"Red", "Blue"};
    uint32_t masks[2] = {7, 7};
    filter.CheckProfanityWords(masks, 0xffff, words, 2);
    char text[] = "Red";
    int masked = 7;
    filter.MaskProfanityWordsInText(&masked, text, 0xffff);
    const uint32_t version = filter.GetContentVersion();
    filter.Finalize();
    if (init == 0 && masks[0] == 0 && masks[1] == 0 && masked == 0 && text[0] == 'R' && version == 0)
        lgp_print("main: profanity filter answered on the host");
    else if (init != 0 && masks[0] == 0xffffffffu && masked == 1 && text[0] == '*')
        lgp_print("main: profanity filter of the sdk");
    else
        lgp_print("main: profanity filter mixed");
    lgp_print("main: first graphics call");
    nv::SetGraphicsAllocator(nullptr, nullptr, nullptr, nullptr);
    alignas(16) static uint8_t graphics_memory[0x1000];
    nv::InitializeGraphics(graphics_memory, sizeof(graphics_memory));
    void* proc = nvnBootstrapLoader("nvnDeviceGetProcAddress");
    if (proc == nullptr) {
        lgp_print("main: no NVN driver answer");
        return;
    }
    // &nvnBootstrapLoader is the sdk export (GOT), as a game passes it on.
    const int r = guest_main(reinterpret_cast<PfnBootstrap>(&nvnBootstrapLoader));
    lgp_check("main: frames rendered through NVN", r == 0);
    nv::FinalizeGraphics();
}
