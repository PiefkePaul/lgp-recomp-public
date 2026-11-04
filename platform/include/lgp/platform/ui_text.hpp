// The app's own interface text (menus, folder dialogs, notes on the start
// screen, the Android import) in the device's language: the code holds the
// English text, German comes from a table (platform/src/ui_text.cpp) while the
// device asks for German. The game's own language is set apart from this
// (hle::language_for_locale, settings.ini language =).
//
// A new text: write it in English inside tr()/trf() and add its German line
// to the table; the test platform.ui_text_table finds texts without one.
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lgp::platform {

enum class UiLanguage { English, German };

// The interface language for a host locale ("de", "de-AT", "de_CH.UTF-8",
// "en-US"): German for any German one, else English.
UiLanguage ui_language_for_locale(std::string_view locale);
// "en" or "de" (also "english", "german", "deutsch"), else the locale rule above.
UiLanguage ui_language_from_name(std::string_view name);
const char* ui_language_name(UiLanguage language);  // "en", "de"

// Set once at start (before the first window or dialog); English until then.
void set_ui_language(UiLanguage language);
UiLanguage ui_language();

// `english` in the interface language. Texts the table lacks stay English.
// The pointer lives as long as the program (the argument or a table entry).
const char* tr(const char* english);
std::string tr(const std::string& english);
// tr() of `english`, then each "{}" replaced by the next of `args`.
std::string trf(const char* english, std::initializer_list<std::string_view> args);

// The German table (English, German) in its order, for tests.
std::vector<std::pair<const char*, const char*>> ui_text_table();

}  // namespace lgp::platform
