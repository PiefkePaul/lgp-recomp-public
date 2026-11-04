// The app's interface text in the device's language (lgp/platform/ui_text.hpp):
// English in the code, German from the table for a German device.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "../runtime/check.hpp"
#include "lgp/platform/ui_text.hpp"

using namespace lgp::platform;

namespace {

void test_locale() {
    for (const char* german : {"de", "de-DE", "de-AT", "de_CH", "de_DE.UTF-8", "DE-de", "German", "deutsch"})
        CHECK(ui_language_from_name(german) == UiLanguage::German);
    for (const char* english : {"", "en", "en-US", "en_GB.UTF-8", "fr-FR", "da", "dev", "C", "POSIX", "english"})
        CHECK(ui_language_from_name(english) == UiLanguage::English);
    CHECK(ui_language_for_locale("de-DE") == UiLanguage::German);
    CHECK(ui_language_for_locale("ja") == UiLanguage::English);
    CHECK(std::strcmp(ui_language_name(UiLanguage::German), "de") == 0);
    CHECK(std::strcmp(ui_language_name(UiLanguage::English), "en") == 0);
}

void test_english() {
    set_ui_language(UiLanguage::English);
    const char* settings = "Settings";
    CHECK(tr(settings) == settings);  // the same text, no copy
    CHECK(tr(std::string("Quit")) == "Quit");
    CHECK(trf("Save {}", {"3"}) == "Save 3");
    CHECK(trf("Backed up to {}", {"saves-2"}) == "Backed up to saves-2");
}

void test_german() {
    set_ui_language(UiLanguage::German);
    CHECK(std::string(tr("Settings")) == "Einstellungen");
    CHECK(tr(std::string("Quit")) == "Beenden");
    CHECK(std::string(tr("Size")) == "Gr\xC3\xB6\xC3\x9F" "e");
    CHECK(trf("Save {}", {"3"}) == "Spielstand 3");
    CHECK(trf("The game folder does not fit:\n{}\n{}", {"D:/x", "Kaputt."}) ==
          "Der Spielordner passt nicht:\nD:/x\nKaputt.");
    // Unknown texts stay English; extra or missing arguments do no harm.
    CHECK(std::string(tr("No such text")) == "No such text");
    CHECK(trf("Now: {}.", {}) == "Jetzt: {}.");
    CHECK(trf("Now: {}.", {"Pro Controller", "extra"}) == "Jetzt: Pro Controller.");
    set_ui_language(UiLanguage::English);
}

void test_table() {
    std::set<std::string> english;
    for (const auto& [en, de] : ui_text_table()) {
        CHECK(en != nullptr && de != nullptr && *en != '\0' && *de != '\0');
        CHECK(english.insert(en).second);  // each text once
    }
    CHECK(english.size() > 100);
}

}  // namespace

int main() {
    RUN(test_locale);
    RUN(test_english);
    RUN(test_german);
    RUN(test_table);
    return finish();
}
