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

#include "util/fonts.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "config.hpp"
#include "fmt/format.h"
#include "i18n.hpp"
#include "imgui/imgui.h"
#include "platform.hpp"
#include "util/paths.hpp"
#include "util/strings.hpp"

#if OSHOT_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif OSHOT_FREEDESKTOP
#  include <fontconfig/fontconfig.h>
#endif

#if OSHOT_WINDOWS
static bool is_hidden_directory(const fs::directory_entry& e)
{
    const std::wstring& wpath = e.path().wstring();
    DWORD               attrs = GetFileAttributesW(wpath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN);
}
#else
static bool is_hidden_directory(const fs::directory_entry& e)
{
    const std::string& name = e.path().filename().string();
    return !name.empty() && name.front() == '.';
}
#endif

// The system and user font folders, searched for a font given by file name
static std::vector<fs::path> font_search_dirs()
{
#if OSHOT_WINDOWS
    static constexpr auto dirs = std::to_array<std::string_view>({
        "C:\\Windows\\Fonts\\",
        "C:\\Windows\\Resources\\Themes\\Fonts\\",
        "$LOCALAPPDATA\\Microsoft\\Windows\\Fonts\\",
    });
#elif OSHOT_MACOS
    static constexpr auto dirs = std::to_array<std::string_view>({
        "/System/Library/Fonts/",
        "/Library/Fonts/",
        "~/Library/Fonts/",
    });
#else
    static constexpr auto dirs = std::to_array<std::string_view>({
        "/usr/share/fonts/",
        "/usr/local/share/fonts/",
        "~/.fonts/",
        "~/.local/share/fonts/",
    });
#endif

    std::vector<fs::path> ret;
    ret.reserve(dirs.size());
    for (const std::string_view dir : dirs)
        ret.emplace_back(expand_var(std::string(dir)));
    return ret;
}

// Calls `fn` with every regular file under `root`, skipping hidden folders
static void for_each_file_under(const fs::path& root, const std::function<bool(const fs::path&)>& fn)
{
    std::error_code ec;
    if (!fs::exists(root, ec) || ec)
        return;

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }

        const fs::directory_entry& e = *it;
        if (e.is_directory(ec) && is_hidden_directory(e))
        {
            it.disable_recursion_pending();
            continue;
        }
        if (e.is_regular_file(ec) && !fn(e.path()))
            return;
    }
}

fs::path get_font_path(const std::string& font)
{
    if (font.empty())
        return {};

    fs::path font_path(font);
    if (fs::exists(font_path))
        return font_path;

    const std::vector<fs::path>& dirs = font_search_dirs();

    // A file directly in a font directory
    for (const fs::path& root : dirs)
    {
        const fs::path& candidate = root / font_path;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec)
            return candidate;
    }

    // Anywhere under one, by file name
    fs::path found;
    for (const fs::path& root : dirs)
    {
        for_each_file_under(root, [&](const fs::path& p) {
            if (p.filename() == font_path)
                found = p;
            return found.empty();
        });
        if (!found.empty())
            return found;
    }

    return {};
}

std::vector<fs::path> list_font_files()
{
    std::vector<fs::path>           ret;
    std::unordered_set<std::string> names;
    for (const fs::path& root : font_search_dirs())
    {
        for_each_file_under(root, [&](const fs::path& p) {
            const std::string ext = str_tolower(p.extension().string());
            // The first of the same name, as get_font_path() would find by name
            if ((ext == ".ttf" || ext == ".otf" || ext == ".ttc") && names.insert(p.filename().string()).second)
                ret.push_back(p);
            return true;
        });
    }
    return ret;
}

ImFont* add_font_file(ImFontAtlas* atlas, const fs::path& path, const float size, const ImFontConfig* cfg)
{
    // A file stb_truetype can't parse (WOFF, a bitmap font) is an error ImGui asserts on, unless told not to
    ImGuiIO&     io       = ImGui::GetIO();
    const bool   assert   = std::exchange(io.ConfigErrorRecoveryEnableAssert, false);
    const bool   tooltip  = std::exchange(io.ConfigErrorRecoveryEnableTooltip, false);
    ImFontConfig font_cfg = cfg ? *cfg : ImFontConfig();
    font_cfg.Flags |= ImFontFlags_NoLoadError;
    ImFont* font                        = atlas->AddFontFromFileTTF(path.string().c_str(), size, &font_cfg);
    io.ConfigErrorRecoveryEnableAssert  = assert;
    io.ConfigErrorRecoveryEnableTooltip = tooltip;
    return font;
}

// A system font file, and the font's index in it for a collection (.ttc)
struct font_file_t
{
    std::string path;
    int         index = 0;
};

/**
 * Find the system UI font, plus the fonts covering a language's script when it lacks them.
 * Uses fontconfig on Linux/BSD, a list of the fonts shipped with the OS on Windows/macOS
 * @param lang A language code like "ru" or "zh_TW"
 * @return The fonts, the first one for Latin text too, the others to fill in its missing glyphs
 **/
static std::vector<font_file_t> find_language_fonts(const std::string& lang)
{
    std::vector<font_file_t> fonts;

#if OSHOT_FREEDESKTOP
    // fontconfig wants RFC 3066 names: "zh-tw"
    std::string fc_lang = str_tolower(lang);
    std::ranges::replace(fc_lang, '_', '-');

    if (!FcInit())
        return fonts;

    FcPattern* pat =
        FcNameParse(reinterpret_cast<const FcChar8*>(fmt::format("sans-serif:lang={}:scalable=true", fc_lang).c_str()));
    if (!pat)
        return fonts;
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult   result;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);
    if (!match)
        return fonts;

    // The best match isn't guaranteed to support the language, e.g. with no CJK font installed,
    // nor to be an outline font stb_truetype can load (a bitmap or Type 1 font makes ImGui assert)
    FcChar8*   file     = nullptr;
    FcChar8*   format   = nullptr;
    FcBool     scalable = FcFalse;
    FcLangSet* langs    = nullptr;
    int        index    = 0;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
        FcPatternGetBool(match, FC_SCALABLE, 0, &scalable) == FcResultMatch && scalable &&
        FcPatternGetString(match, FC_FONTFORMAT, 0, &format) == FcResultMatch &&
        (std::string_view(reinterpret_cast<const char*>(format)) == "TrueType" ||
         std::string_view(reinterpret_cast<const char*>(format)) == "CFF") &&
        FcPatternGetLangSet(match, FC_LANG, 0, &langs) == FcResultMatch &&
        FcLangSetHasLang(langs, reinterpret_cast<const FcChar8*>(fc_lang.c_str())) != FcLangDifferentLang)
    {
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        fonts.push_back({ .path = reinterpret_cast<const char*>(file), .index = index });
    }
    FcPatternDestroy(match);
#else
    const std::string base = lang.substr(0, lang.find('_'));
#  if OSHOT_WINDOWS
    const char*    windir = std::getenv("WINDIR");
    const fs::path dir    = fs::path(windir ? windir : "C:\\Windows") / "Fonts";

    // Segoe UI covers Latin, Greek, Cyrillic, Arabic and Hebrew
    std::vector<std::string_view> names = { "segoeui.ttf" };
    if (lang == "zh_TW" || lang == "zh_HK")
        names.emplace_back("msjh.ttc");
    else if (base == "zh")
        names.emplace_back("msyh.ttc");
    else if (base == "ja")
        names.insert(names.end(), { "YuGothM.ttc", "meiryo.ttc" });
    else if (base == "ko")
        names.emplace_back("malgun.ttf");
    else if (base == "th")
        names.emplace_back("leelawui.ttf");
    else if (base == "hi" || base == "mr" || base == "ne" || base == "bn")
        names.emplace_back("Nirmala.ttf");
#  elif OSHOT_MACOS
    const fs::path dir = "/System/Library/Fonts";

    // Helvetica covers Latin, Greek and Cyrillic, and Arial Unicode most other scripts
    std::vector<std::string_view> names = { "Helvetica.ttc" };
    if (base == "ja")
        names.emplace_back("ヒラギノ角ゴシック W3.ttc");
    else if (base == "ko")
        names.emplace_back("AppleSDGothicNeo.ttc");
    else if (base == "zh")
        names.emplace_back("Hiragino Sans GB.ttc");
    if (i18n::charset() == i18n::Charset::Other)
        names.emplace_back("Supplemental/Arial Unicode.ttf");
#  else
    const fs::path                dir;
    std::vector<std::string_view> names;
#  endif

    for (const std::string_view name : names)
    {
        std::error_code ec;
        const fs::path& path = dir / fs::path(std::u8string(name.begin(), name.end()));
        if (fs::exists(path, ec))
            fonts.push_back({ .path = path.string() });
    }
#endif

    return fonts;
}

void build_font_atlas(ImGuiIO& io)
{
    io.Fonts->Clear();

    ImFontConfig font_cfg;
    ImFont*      first = nullptr;

    for (const std::string& font : g_config->File.fonts)
    {
        const fs::path& path = get_font_path(font);
        if (path.empty())
        {
            spdlog::warn("Font '{}' not found, skipping", font);
            continue;
        }

        ImFont* f = add_font_file(io.Fonts, path, 16.0f, &font_cfg);
        if (!f)
        {
            spdlog::warn("Font '{}' failed to load", font);
            continue;
        }

        if (!first)
        {
            first = f;
            // MergeMode needs a font added first. The next fonts then fill in the glyphs
            // the first one lacks, e.g. Arabic glyphs for a font without them
            font_cfg.MergeMode = true;
        }
    }

    // builtin-font: ProggyClean instead of the system UI font, unless a configured font comes first.
    // It has every Latin-1 character, so the system fonts are only needed for other scripts
    bool builtin_first = false;
    if (!first && g_config->File.builtin_font)
    {
        first              = io.Fonts->AddFontDefaultBitmap();
        font_cfg.MergeMode = true;
        builtin_first      = first != nullptr;
    }

    // The system UI font is the default one, or fills in the language's script after the fonts before it
    std::vector<font_file_t> system_fonts;
    if (!builtin_first || i18n::charset() != i18n::Charset::Latin1)
    {
        system_fonts = find_language_fonts(i18n::current());
        if (system_fonts.empty() && i18n::charset() != i18n::Charset::Latin1)
            spdlog::warn("No font found for language {}: set one in Preferences > Fonts", i18n::current());
    }

    // A font merged into ProggyClean has to take its implicit size, so ImGui scales it the same way
    const float system_size = builtin_first ? 0.0f : 16.0f;
    for (const font_file_t& font : system_fonts)
    {
        font_cfg.FontNo = font.index;
        ImFont* f       = add_font_file(io.Fonts, font.path, system_size, &font_cfg);
        if (!f)
            continue;

        spdlog::debug("Loaded font {} for language {}", font.path, i18n::current());
        if (!first)
        {
            first              = f;
            font_cfg.MergeMode = true;
        }
    }

    if (!first)
        first = io.Fonts->AddFontDefault();  // no system font found, use ImGui's built-in one

    io.FontDefault = first;
}
