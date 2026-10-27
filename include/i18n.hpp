/*
 * Copyright 2026 Toni500
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _I18N_HPP_
#define _I18N_HPP_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fmt/format.h"

/**
 * Translate a string literal to the current language (see i18n::tr()).
 * scripts/update_translations.sh extracts every _("...") into po/oshot.pot
 **/
#define _(str) i18n::tr(str)

/**
 * Mark a string literal for extraction without translating it here,
 * e.g. in a static array that is passed through _() or i18n::tr() later
 **/
#define N_(str) str

namespace i18n
{
// A language the user can pick in Preferences
struct language_t
{
    // e.g. "it" or "pt_BR", the .po file's name
    std::string code;

    // In the language itself when tinygettext knows it, e.g. "Italiano", else in English
    std::string native_name;

    // In English, e.g. "Italian", for when the UI font can't draw native_name
    std::string english_name;
};

// The characters a catalog uses, from the narrowest to the widest set of fonts that can draw them
enum class Charset : uint8_t
{
    Latin1,    // ImGui's built-in font draws them
    European,  // Latin, Greek and Cyrillic, which Helvetica covers
    Other      // e.g. CJK, Arabic or Thai
};

/**
 * Translate a format string, then format it with fmt.
 * fmt checks the English string against the arguments at compile time, like fmt::format(),
 * and a translation whose placeholders don't match it is never loaded, so this can't throw
 * @param msgid The English format string, a string literal
 * @param args Arguments in the format
 **/
template <typename... Args>
std::string tr_fmt(fmt::format_string<Args...> msgid, Args&&... args);

/**
 * Pick the plural form of a format string for a count, translate it, then format it with fmt.
 * Both English forms are checked at compile time, like tr_fmt()
 * @param msgid The English singular, e.g. "{} plugin", a string literal
 * @param msgid_plural The English plural, e.g. "{} plugins", a string literal
 * @param n The count that picks the form
 * @param args Arguments in the format, usually n among them
 **/
template <typename... Args>
std::string tr_n_fmt(fmt::format_string<Args...> msgid,
                     fmt::format_string<Args...> msgid_plural,
                     long                        n,
                     Args&&... args);

/**
 * Load the catalog of a language and make it the current one.
 * Embedded catalogs come from po/<code>.po; a <config dir>/locale/<code>.po file overrides
 * or adds one, so translators can test their work without rebuilding oshot.
 * The catalog closest to the language is picked, e.g. "it" for "it_CH", and a
 * regional one like "pt_BR" is completed by "pt" when both exist
 * @param lang A language code like "it" or "pt_BR", or "auto"/"" for the system language.
 *             Unknown languages fall back to English
 * @param config_dir The config directory, for its locale/ folder
 **/
void init(std::string_view lang, const std::string& config_dir);

/**
 * Translate a string to the current language. Thread-safe.
 * An ImGui ID suffix ("##id" or "###id") is kept as it is and only the label is translated,
 * so _("Save##save_btn") looks up "Save"
 * @param msgid The English string
 * @return The translation, or msgid itself when there is none. Valid until the program exits
 **/
const char* tr(const char* msgid);

/**
 * Translate a string that depends on a count, using the language's Plural-Forms. Thread-safe
 * @param msgid The English singular
 * @param msgid_plural The English plural
 * @param n The count that picks the form
 * @return The translation, or msgid/msgid_plural when there is none. Valid until the program exits
 **/
const char* tr_n(const char* msgid, const char* msgid_plural, long n);

/* @return The code of the loaded language, "en" when no catalog is loaded */
const std::string& current();

/* @return The system's UI language code, e.g. "it_IT" or "en" */
std::string system_language();

/* @return English plus every language with a catalog, sorted by code */
std::vector<language_t> available();

/* @return The characters the current language uses, Latin1 when no catalog is loaded */
Charset charset();
}  // namespace i18n

// A format_string made from a literal views the whole literal, so data() is null-terminated
template <typename... Args>
std::string i18n::tr_fmt(fmt::format_string<Args...> msgid, Args&&... args)
{
    return fmt::format(fmt::runtime(tr(msgid.get().data())), std::forward<Args>(args)...);
}

template <typename... Args>
std::string i18n::tr_n_fmt(fmt::format_string<Args...> msgid,
                           fmt::format_string<Args...> msgid_plural,
                           long                        n,
                           Args&&... args)
{
    return fmt::format(fmt::runtime(tr_n(msgid.get().data(), msgid_plural.get().data(), n)),
                       std::forward<Args>(args)...);
}

#endif  // !_I18N_HPP_
