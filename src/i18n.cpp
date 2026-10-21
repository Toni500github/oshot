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

#include "i18n.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

#include "platform.hpp"
#include "spdlog/spdlog.h"
#include "tinygettext/dictionary.hpp"
#include "tinygettext/language.hpp"
#include "tinygettext/log.hpp"
#include "tinygettext/po_parser.hpp"

#if OSHOT_WINDOWS
#  include <windows.h>
#elif OSHOT_MACOS
#  include <CoreFoundation/CoreFoundation.h>
#endif

namespace fs = std::filesystem;
namespace tg = tinygettext;

// po/<code>.po, embedded into the generated translations.cpp by scripts/embed_translations.cmake
struct embedded_po_t
{
    const char* code;
    const char* data;
    std::size_t size;
};
extern const embedded_po_t g_embedded_po[];
extern const std::size_t   g_embedded_po_count;

namespace
{
// Lets the maps below be searched with a string_view, without building a std::string
struct sv_hash
{
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
template <typename T>
using string_map = std::unordered_map<std::string, T, sv_hash, std::equal_to<>>;

// A language's translations, copied out of tinygettext's Dictionary: tr() needs strings
// that stay put, and lookups that don't allocate or log on every frame
struct catalog_t
{
    // msgid -> msgstr, or the msgstr[N] plural forms
    string_map<std::vector<std::string>> strings;

    // Translations of msgids with an ImGui ID suffix, filled by tr() on first use
    string_map<std::string> suffixed;
    std::mutex              suffixed_mtx;

    tg::PluralForms plural_forms;
    std::string     code;
    bool            extended = false;
};

// Catalogs are never freed: tr() hands out pointers into them, which have to
// outlive a language change and any thread still drawing at exit
std::mutex                    g_load_mtx;
auto*                         g_loaded  = new std::map<std::string, catalog_t*>;
std::atomic<catalog_t*>       g_current = nullptr;
std::vector<i18n::language_t> g_available;
fs::path                      g_locale_dir;

// ------------------------------
// Validation
// ------------------------------

/**
 * Collect the placeholders of a format string: fmt's {} with auto indexes numbered,
 * and printf's % (for ImGui::Text() and friends), so a translation can be checked against its msgid
 * @return The {} ones sorted (a translation may reorder them) followed by the % ones in order,
 *         or nullopt if a brace is unbalanced or auto and manual indexes are mixed
 **/
std::optional<std::vector<std::string>> placeholders(std::string_view s)
{
    std::vector<std::string> braces, percents;
    bool                     auto_index = false, manual_index = false;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '{' || s[i] == '}')
        {
            if (i + 1 < s.size() && s[i + 1] == s[i])
            {
                ++i;
                continue;
            }
            if (s[i] == '}')
                return std::nullopt;

            const std::size_t end = s.find_first_of("{}", i + 1);
            if (end == s.npos || s[end] != '}')
                return std::nullopt;

            std::string_view token = s.substr(i + 1, end - i - 1);
            if (token.empty() || token.front() == ':')
            {
                auto_index = true;
                braces.push_back(fmt::format("{{{}{}}}", braces.size(), token));
            }
            else
            {
                manual_index = true;
                braces.push_back(fmt::format("{{{}}}", token));
            }
            i = end;
        }
        else if (s[i] == '%')
        {
            if (i + 1 < s.size() && s[i + 1] == '%')
            {
                ++i;
                continue;
            }

            // No ' ' flag: it would make a literal "100% done" look like "% d"
            std::size_t j = i + 1;
            while (j < s.size() && std::string_view("-+#0123456789.*hlLqjzt").find(s[j]) != std::string_view::npos)
                ++j;
            if (j < s.size() && std::string_view("diouxXeEfFgGaAcsp").find(s[j]) != std::string_view::npos)
            {
                percents.emplace_back(s.substr(i, j - i + 1));
                i = j;
            }
        }
    }

    if (auto_index && manual_index)
        return std::nullopt;

    std::ranges::sort(braces);
    braces.insert(braces.end(), std::make_move_iterator(percents.begin()), std::make_move_iterator(percents.end()));
    return braces;
}

// A character past Latin-1 needs a 2-byte UTF-8 sequence led by 0xC4 or more, or a longer one
bool has_extended_chars(std::string_view s)
{
    return std::ranges::any_of(s, [](const char c) { return static_cast<unsigned char>(c) >= 0xC4; });
}

// ------------------------------
// Catalog sources
// ------------------------------

// A .po file to parse: its name for tinygettext's messages, and its content
struct po_source_t
{
    std::string name;
    std::string data;
};

// <config dir>/locale/<code>.po, else the embedded po/<code>.po
std::optional<po_source_t> read_catalog(const std::string& code)
{
    std::error_code ec;
    const fs::path& path = g_locale_dir / (code + ".po");
    if (!g_locale_dir.empty() && fs::is_regular_file(path, ec))
    {
        std::ifstream f(path, std::ios::binary);
        if (f)
            return po_source_t{ .name = path.string(), .data = std::string(std::istreambuf_iterator<char>(f), {}) };
        spdlog::warn("Failed to read {}", path.string());
    }

    for (std::size_t i = 0; i < g_embedded_po_count; ++i)
        if (code == g_embedded_po[i].code)
            return po_source_t{ .name = fmt::format("po/{}.po", code),
                                .data = std::string(g_embedded_po[i].data, g_embedded_po[i].size) };

    return std::nullopt;
}

// Every language with a catalog, embedded or in the locale folder, sorted
std::vector<std::string> catalog_codes()
{
    std::vector<std::string> codes;
    for (std::size_t i = 0; i < g_embedded_po_count; ++i)
        codes.emplace_back(g_embedded_po[i].code);

    std::error_code ec;
    if (!g_locale_dir.empty() && fs::is_directory(g_locale_dir, ec))
    {
        for (const fs::directory_entry& entry : fs::directory_iterator(g_locale_dir, ec))
            if (entry.path().extension() == ".po")
                codes.push_back(entry.path().stem().string());
    }

    std::ranges::sort(codes);
    codes.erase(std::ranges::unique(codes).begin(), codes.end());
    return codes;
}

/**
 * Parse a .po file with tinygettext and add its valid translations to a catalog,
 * replacing the ones it already has
 * @param code The catalog's language code
 * @param cat The catalog to fill
 **/
void load_catalog(const std::string& code, catalog_t& cat)
{
    const std::optional<po_source_t>& src = read_catalog(code);
    if (!src)
        return;

    tg::Dictionary dict;
    try
    {
        std::istringstream in(src->data);
        // Fuzzy entries are guesses waiting for review: never show them, like gettext
        tg::POParser::parse(src->name, in, dict, /*use_fuzzy=*/false);
    }
    catch (const std::exception& e)
    {
        spdlog::warn("Failed to parse {}: {}", src->name, e.what());
        return;
    }

    if (dict.get_plural_forms())
        cat.plural_forms = dict.get_plural_forms();

    dict.foreach ([&](const std::string& msgid, const std::vector<std::string>& msgstrs) {
        // A plural's msgid_plural isn't kept by tinygettext: every form is checked against
        // the singular, so both English forms have to use the same placeholders
        const auto& expected = placeholders(msgid);
        for (const std::string& msgstr : msgstrs)
        {
            if (!msgstr.empty() && placeholders(msgstr) != expected)
            {
                spdlog::warn(
                    "{}: placeholders of the translation of \"{}\" don't match, ignoring it", src->name, msgid);
                return;
            }
        }

        cat.extended = cat.extended || std::ranges::any_of(msgstrs, has_extended_chars);
        cat.strings.insert_or_assign(msgid, msgstrs);
    });
}

/**
 * Pick the catalogs for a locale, using tinygettext's language matching
 * @param locale A POSIX, Windows or macOS locale name, e.g. "pt_BR.UTF-8", "pt-BR" or "zh-Hans-CN"
 * @param codes The languages with a catalog
 * @return The codes to load in order, the best match last so it wins: {"pt", "pt_BR"}.
 *         Empty for English or a language without a catalog
 **/
std::vector<std::string> pick_catalogs(std::string locale, const std::vector<std::string>& codes)
{
    std::ranges::replace(locale, '-', '_');
    // macOS names Chinese by script, the catalogs by region
    if (locale.starts_with("zh_Hans"))
        locale = "zh_CN";
    else if (locale.starts_with("zh_Hant"))
        locale = "zh_TW";

    const tg::Language wanted = tg::Language::from_env(locale);
    if (!wanted || wanted.get_language() == "en")
        return {};

    std::string best;
    int         best_score = 0;
    for (const std::string& code : codes)
    {
        const int score = tg::Language::match(wanted, tg::Language::from_env(code));
        if (score > best_score)
        {
            best       = code;
            best_score = score;
        }
    }
    if (best.empty())
        return {};

    // The plain language's catalog fills in what a regional one doesn't translate
    std::vector<std::string> out;
    const tg::Language       best_lang = tg::Language::from_env(best);
    const tg::Language       base      = tg::Language::from_spec(best_lang.get_language());
    if (base != best_lang)
    {
        for (const std::string& code : codes)
            if (tg::Language::from_env(code) == base)
                out.push_back(code);
    }
    out.push_back(best);
    return out;
}

// Route tinygettext's messages to spdlog. Its lines end with a newline
void set_tinygettext_logging()
{
    // Info is a line per string it can't translate: we don't use its lookups anyway
    tg::Log::set_log_info_callback(nullptr);
    tg::Log::set_log_warning_callback([](const std::string& s) {
        spdlog::warn("tinygettext: {}", std::string_view(s).substr(0, s.find_last_not_of('\n') + 1));
    });
    tg::Log::set_log_error_callback([](const std::string& s) {
        spdlog::error("tinygettext: {}", std::string_view(s).substr(0, s.find_last_not_of('\n') + 1));
    });
}

}  // namespace

// ------------------------------
// Public API
// ------------------------------

std::string i18n::system_language()
{
    // LANGUAGE is GNU gettext's priority list, e.g. "it:en"
    for (const char* var : { "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG" })
    {
        const char* v = std::getenv(var);
        if (!v || !*v)
            continue;

        std::string s(v);
        s = s.substr(0, s.find(':'));
        if (s == "C" || s.starts_with("C.") || s == "POSIX")
            return "en";
        if (!s.empty())
            return s;
    }

#if OSHOT_WINDOWS
    wchar_t buf[LOCALE_NAME_MAX_LENGTH]{};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), buf, LOCALE_NAME_MAX_LENGTH, 0) > 0)
    {
        // Locale names are ASCII
        std::string s;
        for (const wchar_t* p = buf; *p; ++p)
            s += static_cast<char>(*p);
        return s;
    }
#elif OSHOT_MACOS
    std::string s;
    CFArrayRef  langs = CFLocaleCopyPreferredLanguages();
    if (langs && CFArrayGetCount(langs) > 0)
    {
        const auto lang = static_cast<CFStringRef>(CFArrayGetValueAtIndex(langs, 0));
        char       buf[64]{};
        if (CFStringGetCString(lang, buf, sizeof(buf), kCFStringEncodingUTF8))
            s = buf;
    }
    if (langs)
        CFRelease(langs);
    if (!s.empty())
        return s;
#endif

    return "en";
}

void i18n::init(std::string_view lang, const std::string& config_dir)
{
    std::scoped_lock lock(g_load_mtx);

    set_tinygettext_logging();
    g_locale_dir = config_dir.empty() ? fs::path{} : fs::path(config_dir) / "locale";

    const std::vector<std::string>& codes = catalog_codes();
    g_available                           = { { .code = "en", .native_name = "English", .english_name = "English" } };
    for (const std::string& code : codes)
    {
        const tg::Language language = tg::Language::from_env(code);
        if (!language)
            spdlog::warn("Translation '{}' isn't a language code tinygettext knows, it can't be picked", code);
        else
            g_available.push_back(
                { .code = code, .native_name = language.get_localized_name(), .english_name = language.get_name() });
    }

    const bool                      is_auto = lang.empty() || lang == "auto";
    const std::string               locale  = is_auto ? system_language() : std::string(lang);
    const std::vector<std::string>& picked  = pick_catalogs(locale, codes);
    if (picked.empty())
    {
        g_current.store(nullptr, std::memory_order_release);
        if (!is_auto && !locale.starts_with("en"))
            spdlog::warn("No translation for language '{}', using English", locale);
        return;
    }

    catalog_t*& cat = (*g_loaded)[picked.back()];
    if (!cat)
    {
        cat       = new catalog_t;
        cat->code = picked.back();
        for (const std::string& code : picked)
            load_catalog(code, *cat);
        if (!cat->plural_forms)
            spdlog::warn("{}.po has no Plural-Forms tinygettext knows, plural strings stay in English", cat->code);
    }

    g_current.store(cat, std::memory_order_release);
    spdlog::info("Language: {} ({} strings{})", cat->code, cat->strings.size(), is_auto ? ", from the system" : "");
}

const char* i18n::tr(const char* msgid)
{
    catalog_t* cat = g_current.load(std::memory_order_acquire);
    if (!cat || !msgid)
        return msgid;

    const std::string_view id(msgid);
    const std::size_t      suffix = id.find("##");
    const auto             it     = cat->strings.find(id.substr(0, suffix));
    if (it == cat->strings.end() || it->second.empty() || it->second[0].empty())
        return msgid;

    const std::string& msgstr = it->second[0];
    if (suffix == id.npos)
        return msgstr.c_str();

    std::scoped_lock lock(cat->suffixed_mtx);
    auto [sit, inserted] = cat->suffixed.try_emplace(std::string(id));
    if (inserted)
        sit->second = msgstr + std::string(id.substr(suffix));
    return sit->second.c_str();
}

const char* i18n::tr_n(const char* msgid, const char* msgid_plural, const long n)
{
    const char*      english = (n == 1) ? msgid : msgid_plural;
    const catalog_t* cat     = g_current.load(std::memory_order_acquire);
    if (!cat)
        return english;

    const auto it = cat->strings.find(std::string_view(msgid));
    if (it == cat->strings.end())
        return english;

    // Plural rules only look at the last digits, but take an int
    const int          count = static_cast<int>(n > INT_MAX ? n % 1000000 + 1000000 : std::max<long>(n, 0));
    const unsigned int index = cat->plural_forms ? cat->plural_forms.get_plural(count) : (n == 1 ? 0 : 1);
    if (index >= it->second.size() || it->second[index].empty())
        return english;
    return it->second[index].c_str();
}

const std::string& i18n::current()
{
    static const std::string english = "en";
    const catalog_t*         cat     = g_current.load(std::memory_order_acquire);
    return cat ? cat->code : english;
}

std::vector<i18n::language_t> i18n::available()
{
    std::scoped_lock lock(g_load_mtx);
    return g_available;
}

bool i18n::needs_extended_font()
{
    const catalog_t* cat = g_current.load(std::memory_order_acquire);
    return cat && cat->extended;
}
