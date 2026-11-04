// SDK functions that always get a hook door (lgp/runtime/hooks.hpp) when a
// module exports them, so the runtime can stand in for them where the game
// needs data the user's dump does not have.
//
// nn::ngc::ProfanityFilter reads its word lists from the system data archive
// NgWord2 (0100000000000823) of the console's firmware. Without the archive
// fsp-srv answers TargetNotFound and LGP stops after the intro. With these
// doors the boot program replaces the filter when the game folder has no
// systemdata/0100000000000823 (runtime/boot/src/ngc_hle.cpp): nothing is a
// profanity, nothing is masked. The names are the SDK's public API
// (mangled), not game data; which ones a game exports varies, missing ones
// are skipped.
#pragma once

#include <array>
#include <string_view>

namespace lgp::recomp {

enum class SdkHookRole {
    NgcInitialize,         // Result Initialize(this, void* work, size_t size): mounts NgWord2
    NgcFinalize,           // void Finalize(this): unmounts it
    NgcContentVersion,     // u32 GetContentVersion(this): version.dat of the archive
    NgcCheckWords,         // Result CheckProfanityWords(this, u32* masks, u32 patterns, const T** words, size_t n)
    NgcMaskText,           // Result MaskProfanityWordsInText(this, int* masked, T* text, u32 patterns)
};

struct SdkHookSymbol {
    std::string_view name;
    SdkHookRole role;
};

inline constexpr std::array<SdkHookSymbol, 10> kSdkHookSymbols = {{
    {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", SdkHookRole::NgcInitialize},
    {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", SdkHookRole::NgcInitialize},
    {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", SdkHookRole::NgcFinalize},
    {"_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", SdkHookRole::NgcContentVersion},
    {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", SdkHookRole::NgcCheckWords},
    {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", SdkHookRole::NgcCheckWords},
    {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", SdkHookRole::NgcCheckWords},
    {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", SdkHookRole::NgcMaskText},
    {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", SdkHookRole::NgcMaskText},
    {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", SdkHookRole::NgcMaskText},
}};

}  // namespace lgp::recomp
