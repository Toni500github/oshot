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

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "config.hpp"
#include "fmt/format.h"
#include "hotkey.hpp"
#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#include "platform.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/spdlog.h"
#include "util.hpp"
#include "util/color.hpp"
#include "util/fonts.hpp"
#include "util/image.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
#include "util/strings.hpp"
#ifndef DISABLE_PLUGINS
#  include "plugin.hpp"
#  include "state_manager.hh"
#endif
#include "helpers.hpp"

using namespace std::chrono_literals;

// ------------------------------
// Helpers
// ------------------------------

static std::unordered_map<std::string, int>& color_name_map()
{
    static std::unordered_map<std::string, int> map;
    if (map.empty())
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
            map[ImGui::GetStyleColorName(i)] = i;

    return map;
}

// The interface font size: font-size, else `own_size`, the main font's own one
static float interface_font_size(const float own_size)
{
    const int size = g_config->File.font_size;
    return size > 0 ? float(std::clamp(size, 10, 32)) : own_size;
}

void apply_imgui_theme()
{
    ScreenshotTool::StyleDefaultColor();

    ImGuiStyle& style = ImGui::GetStyle();
    const auto& ov    = g_config->theme_overrides;
    const auto& cmap  = color_name_map();

    for (const auto& [name, hex] : ov.colors)
    {
        auto it = cmap.find(name);
        if (it == cmap.end())
            continue;
        ImVec4 col;
        if (hexstr_to_imvec4(hex.data(), col))
            style.Colors[it->second] = col;
    }

    // -1 means "not set"
    if (ov.window_rounding >= 0.f)
        style.WindowRounding = ov.window_rounding;
    if (ov.frame_rounding >= 0.f)
        style.FrameRounding = ov.frame_rounding;
    if (ov.grab_rounding >= 0.f)
        style.GrabRounding = ov.grab_rounding;
    if (ov.tab_rounding >= 0.f)
        style.TabRounding = ov.tab_rounding;
    if (ov.window_border >= 0.f)
        style.WindowBorderSize = ov.window_border;
    if (ov.frame_border >= 0.f)
        style.FrameBorderSize = ov.frame_border;

    // For the next frame, since a frame puts back the size it started with (ImGui's own style editor does the same)
    style._NextFrameFontSizeBase = interface_font_size(ImGui::GetDefaultFont()->LegacySize);
}

// ------------------------------
// Windows
// ------------------------------

#if OSHOT_HAS_GLOBAL_HOTKEY
// Button that records the next key combination into `hotkey_str`
static void draw_hotkey_recorder(std::string& hotkey_str, bool window_just_opened)
{
    static bool recording = false;
    if (window_just_opened)
        recording = false;

    const ImGuiIO& io = ImGui::GetIO();

    ImGui::TextUnformatted(_("Global capture hotkey"));
    ImGui::SameLine();
    HelpMarker(
        _("System-wide shortcut that starts a capture while oshot is running in the tray.\n"
          "Click the button, then press the key combination. Click it again to cancel."));

    const std::string& label = recording ? _("Press keys...") : (hotkey_str.empty() ? _("Not set") : hotkey_str);
    if (ImGui::Button(fmt::format("{}##config_hotkey", label).c_str(), ImVec2(ImGui::GetFontSize() * 12, 0)))
        recording = !recording;

    ImGui::SameLine();
    ImGui::BeginDisabled(hotkey_str.empty());
    if (ImGui::Button(_("Clear##config_hotkey_clear")))
    {
        hotkey_str.clear();
        recording = false;
    }
    ImGui::EndDisabled();

    bool missing_mod = false;
    if (recording)
    {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        {
            const auto key = toe<ImGuiKey>(k);
            if (!hotkey::is_supported_key(key) || !ImGui::IsKeyDown(key))
                continue;

            // ImGui swaps Cmd and Ctrl on macOS, undo it to get the physical keys
            const bool       swapped = io.ConfigMacOSXBehaviors;
            hotkey::hotkey_t hk{ .mods = hotkey::Mod::kNone, .key = key };
            if (swapped ? io.KeySuper : io.KeyCtrl)
                hk.mods |= hotkey::Mod::Ctrl;
            if (swapped ? io.KeyCtrl : io.KeySuper)
                hk.mods |= hotkey::Mod::Super;
            if (io.KeyShift)
                hk.mods |= hotkey::Mod::Shift;
            if (io.KeyAlt)
                hk.mods |= hotkey::Mod::Alt;

            // A bare letter/digit/etc. would swallow normal typing system-wide
            const bool standalone_ok = (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) || key == ImGuiKey_PrintScreen ||
                                       key == ImGuiKey_Pause || key == ImGuiKey_Insert;
            if (hk.mods == hotkey::Mod::kNone && !standalone_ok)
            {
                missing_mod = true;
                continue;
            }

            hotkey_str = hotkey::to_string(hk);
            recording  = false;
            break;
        }
    }

    if (missing_mod)
        ImGui::TextDisabled(_("Hold Ctrl, Shift, Alt or %s too"), OSHOT_MACOS ? "Cmd" : "Win");
    else if (!hotkey::status().empty())
        ImGui::TextDisabled("%s", hotkey::status().c_str());
    else
        ImGui::TextDisabled("%s", _("Applied by the tray instance (oshot --tray, or started at sign-in)"));
}
#endif

// A font file loaded on its own for the preview in Preferences. Only the last one asked for
// stays in the atlas, since a CJK collection takes about 20 MB
struct font_preview_t
{
    fs::path path;            // the file last asked for, loaded or not
    ImFont*  font = nullptr;  // nullptr if ImGui couldn't load it
};
static font_preview_t font_preview;

static void release_preview_font()
{
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (font_preview.font && atlas->Fonts.contains(font_preview.font))
        atlas->RemoveFont(font_preview.font);
    font_preview = {};
}

static ImFont* load_preview_font(const fs::path& path)
{
    // The ImGui context of an earlier capture took the font with its atlas
    if (font_preview.font && !ImGui::GetIO().Fonts->Fonts.contains(font_preview.font))
        font_preview = {};
    if (path == font_preview.path)
        return font_preview.font;

    release_preview_font();
    font_preview.path = path;
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec))
        return nullptr;

    font_preview.font = add_font_file(ImGui::GetIO().Fonts, path, 16.0f);
    if (font_preview.font)
        ImFontAtlasBuildMain(ImGui::GetIO().Fonts);
    return font_preview.font;
}

// A writing system, a character only it has, and a line to preview it. No line for the scripts
// ImGui can't lay out (right to left, or letters joined or combined)
struct font_script_t
{
    const char* name;
    ImWchar     probe;
    const char* sample;
};

// clang-format off
static constexpr auto font_scripts = std::to_array<font_script_t>({
    { .name=N_("Latin"),      .probe=0x0041, .sample="The quick brown fox jumps over the lazy dog" },
    { .name=N_("Greek"),      .probe=0x03A9, .sample="ΞΕΣΚΕΠΑΖΩ ΤΗΝ ΨΥΧΟΦΘΟΡΑ ΒΔΕΛΥΓΜΙΑ" },  // capitals have no accents a font may lack
    { .name=N_("Cyrillic"),   .probe=0x0416, .sample="Съешь же ещё этих мягких французских булок" },
    { .name=N_("Arabic"),     .probe=0x0628, .sample="نص حكيم له سر قاطع وذو شأن عظيم مكتوب على ثوب أخضر ومغلف بجلد أزرق" },
    { .name=N_("Hebrew"),     .probe=0x05D0, .sample="עטלף אבק נס דרך מזגן שהתפוצץ כי חם" },
    { .name=N_("Devanagari"), .probe=0x0915, .sample="ऋषियों को सताने वाले दुष्ट राक्षसों के राजा रावण का सर्वनाश करने वाले विष्णुवतार भगवान श्रीराम, अयोध्या के महाराज दशरथ के बड़े सपुत्र थे।" },
    { .name=N_("Thai"),       .probe=0x0E01, .sample="ะ า ิ ี ึ ื ุ ู เ แ โ ใ ไ ำ ๅ ฤ ฤๅ ฦ ฦๅ" },
    { .name=N_("Chinese"),    .probe=0x4E2D, .sample="我能吞下玻璃而不伤身体" },
    { .name=N_("Japanese"),   .probe=0x3042, .sample="いろはにほへと ちりぬるを" },
    { .name=N_("Korean"),     .probe=0xAC00, .sample="다람쥐 헌 쳇바퀴에 타고파" },
});
// clang-format on

// A small label, filled when `on`, only outlined when not
static void draw_chip(const char* label, const bool on)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2      text  = ImGui::CalcTextSize(label);
    const ImVec2      pad(style.FramePadding.x * 0.75f, style.FramePadding.y * 0.5f);
    const ImVec2      min = ImGui::GetCursorScreenPos();
    const ImVec2      max(min.x + text.x + pad.x * 2, min.y + text.y + pad.y * 2);
    ImGui::Dummy(ImVec2(max.x - min.x, max.y - min.y));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (on)
        draw_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_Button), style.FrameRounding);
    else
        draw_list->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
    draw_list->AddText(
        ImVec2(min.x + pad.x, min.y + pad.y), ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
}

// The card that shows a font: its path, the scripts it has and sample lines in it at `size`
static void draw_font_preview(const std::string& title,
                              const char*        role,
                              const fs::path&    path,
                              ImFont*            font,
                              const float        size)
{
    static std::string preview_text;

    ImGui::BeginChild("##font_preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextUnformatted(title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", role);

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (!path.empty())
        ImGui::TextWrapped("%s", path.string().c_str());
    ImGui::PopStyleColor();

    if (!font)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, (0xFF6666FF_rgba).to_imvec4());
        ImGui::TextWrapped("%s",
                           path.empty()
                               ? _("Not found: not an existing file, nor a file in the system font folders")
                               : _("Can't be loaded: only TrueType and OpenType fonts work (.ttf, .otf, .ttc)"));
        ImGui::PopStyleColor();
        ImGui::EndChild();
        return;
    }

    // The scripts it has, as chips wrapped to the card's width
    ImGui::Spacing();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i < font_scripts.size(); ++i)
    {
        const font_script_t& script     = font_scripts[i];
        const char*          name       = i18n::tr(script.name);
        const bool           has        = font->IsGlyphInFont(script.probe);
        const float          chip_width = ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 1.5f;
        if (i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + chip_width <= right)
            ImGui::SameLine();
        draw_chip(name, has);
        ImGui::SetItemTooltip("%s",
                              has ? _("Has this script") : _("Lacks this script, a font after it can fill it in"));
    }
    ImGui::Spacing();

    ImGui::BeginChild("##font_samples",
                      ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_FrameStyle);
    ImGui::PushFont(font, size);
    if (!preview_text.empty())
        ImGui::TextWrapped("%s", preview_text.c_str());
    for (const font_script_t& script : font_scripts)
    {
        if (!script.sample || !font->IsGlyphInFont(script.probe))
            continue;
        ImGui::TextWrapped("%s", script.sample);
        if (&script == &font_scripts[0])  // Latin
            ImGui::TextWrapped("%s", "0123456789 àéîõü ÆØß ?!&@#");
    }
    ImGui::PopFont();
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##preview_text", _("Type to preview your own text"), &preview_text);
    ImGui::EndChild();
}

// Up and Down in the font input move the highlighted suggestion, -1 for none
static int font_suggestion_keys(ImGuiInputTextCallbackData* data)
{
    int& highlighted = *static_cast<int*>(data->UserData);
    highlighted += data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
    return 0;
}

static void draw_preference_fonts(const bool window_just_opened)
{
    // What stb_truetype loads
    static constexpr SDL_DialogFileFilter font_filter{ .name = N_("Fonts"), .pattern = "ttf;otf;ttc" };
    static constexpr int                  max_suggestions = 8;

    static std::string           new_font;
    static std::vector<fs::path> resolved_font_paths;  // get_font_path() of each font, in the list's order
    static size_t                selected       = 0;
    static bool                  should_refocus = false;

    // The installed fonts, listed in the background each time the window opens
    static std::future<std::vector<fs::path>> listing;
    static std::vector<fs::path>              installed;

    // The installed fonts whose name has the typed text, for suggestion_query
    static std::vector<fs::path> suggestions;
    static std::string           suggestion_query;
    static int                   highlighted = 0;

    std::vector<std::string>& fonts = g_config->File.fonts;
    if (window_just_opened)
    {
        new_font.clear();
        if (!listing.valid())
            listing = std::async(std::launch::async, list_font_files);
    }
    if (window_just_opened || resolved_font_paths.size() != fonts.size())
    {
        resolved_font_paths.clear();
        for (const std::string& f : fonts)
            resolved_font_paths.push_back(get_font_path(f));
    }
    selected = std::min(selected, fonts.empty() ? 0 : fonts.size() - 1);

    bool listed_now = false;
    if (listing.valid() && listing.wait_for(0s) == std::future_status::ready)
    {
        installed  = listing.get();
        listed_now = true;
    }

    // A path isn't searched, only a name. Matches that start with the text come first
    const bool is_path = new_font.find_first_of("/\\") != std::string::npos;
    if (new_font != suggestion_query || listed_now)
    {
        suggestion_query = new_font;
        highlighted      = 0;
        suggestions.clear();
        if (!new_font.empty() && !is_path)
        {
            const std::string                                 query = str_tolower(new_font);
            std::vector<std::tuple<int, std::string, size_t>> matches;  // rank, lowercase name, index in installed
            for (size_t i = 0; i < installed.size(); ++i)
            {
                const std::string name  = installed[i].filename().string();
                const std::string lower = str_tolower(name);
                const size_t      pos   = lower.find(query);
                if (pos == std::string::npos || std::ranges::find(fonts, name) != fonts.end())
                    continue;
                matches.emplace_back(lower == query ? 0 : pos == 0 ? 1 : 2, lower, i);
            }
            std::ranges::sort(matches);
            for (size_t i = 0; i < matches.size() && i < max_suggestions; ++i)
                suggestions.push_back(installed[std::get<2>(matches[i])]);
        }
    }
    highlighted = std::clamp(highlighted, -1, int(suggestions.size()) - 1);

    // The highlighted suggestion, or the text as typed
    const auto pending_font = [&]() -> std::string {
        if (suggestion_query == new_font && highlighted >= 0 && size_t(highlighted) < suggestions.size())
            return suggestions[size_t(highlighted)].filename().string();
        return new_font;
    };

    const auto add_font = [&](const std::string& font) {
        if (font.empty() || std::ranges::find(fonts, font) != fonts.end())
            return;
        fonts.push_back(font);
        resolved_font_paths.push_back(get_font_path(font));
        selected = fonts.size() - 1;
        new_font.clear();
        should_refocus = true;
    };

    // Moves a font to another place, shifting the ones in between
    const auto move_font = [&](const size_t from, const size_t to) {
        const auto rotate = [&](auto& v) {
            const auto first = v.begin() + static_cast<ptrdiff_t>(std::min(from, to));
            const auto last  = v.begin() + static_cast<ptrdiff_t>(std::max(from, to)) + 1;
            if (from < to)
                std::rotate(first, first + 1, last);
            else
                std::rotate(first, last - 1, last);
        };
        rotate(fonts);
        rotate(resolved_font_paths);
        selected = to;
    };

    // A path shows only its file name, the preview has the rest
    const auto display_name = [](const std::string& font) { return fs::path(font).filename().string(); };

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText(_("Fonts"));
    ImGui::TextWrapped("%s",
                       _("The first font draws the interface, the next ones fill in the characters it lacks, "
                         "e.g. NotoSansCJK-Regular.ttc after Roboto-Regular.ttf for Chinese or Japanese. "
                         "The system UI font fills in what is still missing."));
    ImGui::Spacing();

    ImGui::Checkbox(_("Use the built-in font##config_builtin_font"), &g_config->File.builtin_font);
    ImGui::SameLine();
    HelpMarker(
        _("ImGui's ProggyClean, a small pixel font, instead of the system UI font.\n"
          "The fonts below still come first, and system fonts still fill in the characters it lacks."));

    // The size build_font_atlas() will load the main font at: 16 px, or 13 for ProggyClean
    const float own_size   = g_config->File.builtin_font && fonts.empty() ? 13.0f : 16.0f;
    const float font_size  = interface_font_size(own_size);
    int         shown_size = int(font_size);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(_("Font size"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::SliderInt("##config_font_size", &shown_size, 10, 32, "%d px", ImGuiSliderFlags_AlwaysClamp))
        g_config->File.font_size = shown_size;
    ImGui::SameLine();
    if (g_config->File.font_size > 0)
    {
        if (ImGui::SmallButton(_("Default")))
            g_config->File.font_size = 0;
    }
    else
    {
        ImGui::TextDisabled("%s", _("default"));
    }
    ImGui::SameLine();
    HelpMarker(
        _("The size of all the text in oshot's windows, applied when saved. The preview below uses it.\n"
          "The default is the main font's own size, 16 px, or 13 px with the built-in font, "
          "which is blurry at any other size."));
    ImGui::Spacing();

    // The list and the preview side by side when there's room, else the preview goes below
    const ImGuiStyle& style        = ImGui::GetStyle();
    const float       avail        = ImGui::GetContentRegionAvail().x;
    const bool        side_by_side = avail >= ImGui::GetFontSize() * 34.0f;
    const float       list_width   = side_by_side ? std::floor(avail * 0.45f) : avail;
    // The theme's Header color is close to the background, too faint to mark a selection
    const ImU32 selection_color = ImGui::GetColorU32(ImGuiCol_Button, 0.45f);

    ImGui::BeginGroup();
    const float row_height = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
    const float rows       = float(std::clamp(fonts.size(), size_t(3), size_t(6)));
    if (ImGui::BeginTable("##font_list",
                          3,
                          ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY,
                          ImVec2(list_width, row_height * rows + 2.0f)))  // + the borders
    {
        ImGui::TableSetupColumn("##number", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##role", ImGuiTableColumnFlags_WidthFixed);

        std::optional<std::pair<size_t, size_t>> dragged;  // moved after the loop, which still reads the list
        for (size_t i = 0; i < fonts.size(); ++i)
        {
            ImGui::PushID(int(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, selection_color);
            if (ImGui::Selectable(fmt::format("{}", i + 1).c_str(), selected == i, ImGuiSelectableFlags_SpanAllColumns))
                selected = i;
            ImGui::PopStyleColor();
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("OSHOT_FONT_ROW", &i, sizeof(i));
                ImGui::TextUnformatted(display_name(fonts[i]).c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("OSHOT_FONT_ROW"))
                    dragged = { *static_cast<const size_t*>(payload->Data), i };
                ImGui::EndDragDropTarget();
            }

            ImGui::TableNextColumn();
            if (resolved_font_paths[i].empty())
                ImGui::TextColored((0xFF6666FF_rgba), "%s", display_name(fonts[i]).c_str());
            else
                ImGui::TextUnformatted(display_name(fonts[i]).c_str());

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", i == 0 ? _("Main") : _("Fallback"));
            ImGui::PopID();
        }
        if (fonts.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", _("No fonts added, the system UI font is used."));
        }
        ImGui::EndTable();

        if (dragged)
            move_font(dragged->first, dragged->second);
    }

    ImGui::BeginDisabled(fonts.empty() || selected == 0);
    if (ImGui::ArrowButton("##font_up", ImGuiDir_Up))
        move_font(selected, selected - 1);
    ImGui::SetItemTooltip("%s", _("Move up, or drag the row"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(selected + 1 >= fonts.size());
    if (ImGui::ArrowButton("##font_down", ImGuiDir_Down))
        move_font(selected, selected + 1);
    ImGui::SetItemTooltip("%s", _("Move down, or drag the row"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(fonts.empty());
    if (ImGui::Button(_("Remove")))
    {
        fonts.erase(fonts.begin() + static_cast<ptrdiff_t>(selected));
        resolved_font_paths.erase(resolved_font_paths.begin() + static_cast<ptrdiff_t>(selected));
        selected = std::min(selected, fonts.empty() ? 0 : fonts.size() - 1);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (should_refocus)
    {
        ImGui::SetKeyboardFocusHere(0);
        should_refocus = false;
    }
    // Enter adds the highlighted suggestion, else the text. A file from "..." or a dropped one is added right away.
    // The input and its "..." button fill the line up to the Add font button (3 = the gap before the empty label)
    ImGui::PushItemWidth(list_width - button_width({ _("Add font") }) - style.ItemSpacing.x - 3.0f);
    draw_input_text_file(
        "",
        "##font_path",
        font_filter,
        [&] { add_font(pending_font()); },
        new_font,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
        font_suggestion_keys,
        &highlighted);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    const std::string pending   = pending_font();
    const bool        duplicate = std::ranges::find(fonts, pending) != fonts.end();
    ImGui::BeginDisabled(pending.empty() || duplicate);
    if (ImGui::Button(_("Add font")))
        add_font(pending);
    if (duplicate)
        ImGui::SetItemTooltip("%s", _("Already in the list"));
    ImGui::EndDisabled();

    // Clicking a suggestion adds it
    if (!suggestions.empty())
    {
        ImGui::BeginChild(
            "##font_suggestions", ImVec2(list_width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        for (size_t i = 0; i < suggestions.size(); ++i)
        {
            const std::string name = suggestions[i].filename().string();
            ImGui::PushID(int(i));
            ImGui::PushStyleColor(ImGuiCol_Header, selection_color);
            if (ImGui::Selectable(name.c_str(), highlighted == int(i)))
                add_font(name);
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", suggestions[i].string().c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", suggestions[i].parent_path().string().c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    const char* hint = nullptr;
    if (new_font.empty() || is_path)
        hint = _("Type to search the installed fonts, or enter a font file's path");
    else if (listing.valid())
        hint = _("Searching the font folders...");
    else if (suggestions.empty())
        hint = _("No installed font matches, Enter adds the name as typed");
    else
        hint = _("Up and Down choose a font, Enter adds it");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + list_width);
    ImGui::TextDisabled("%s", hint);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    if (side_by_side)
        ImGui::SameLine();
    else
        ImGui::Spacing();

    // The selected font, else the font in use
    ImGui::BeginGroup();
    if (!fonts.empty())
        draw_font_preview(display_name(fonts[selected]),
                          selected == 0 ? _("main font") : _("fallback font"),
                          resolved_font_paths[selected],
                          load_preview_font(resolved_font_paths[selected]),
                          font_size);
    else
    {
        release_preview_font();
        draw_font_preview(_("Interface font"), _("in use now"), {}, ImGui::GetIO().FontDefault, font_size);
    }
    ImGui::EndGroup();
}

static void draw_preference_edit_config(const std::function<void()>& refresh_models_func, bool window_just_opened)
{
    static constexpr auto image_prev_units = std::to_array({ "off", "auto", "B", "KiB", "MiB", "KB", "MB" });
    static constexpr SDL_DialogFileFilter toml_filter{ .name = N_("TOML files"), .pattern = "toml" };

    static int                 image_ext_sel    = 0;
    static int                 image_preuni_sel = 0;
    static Result<std::string> r                = get_config_image_out_fmt();

    ImGui::SeparatorText(_("Edit default config"));
    ImGui::Spacing();

    // --- Language ---
    // ImGui's own font is Latin-1 only, so "Русский" would be "???????" in an English UI.
    // Fonts for every script would cost megabytes, so fall back to the English name
    auto language_name = [](const i18n::language_t& l) -> const std::string& {
        ImFont*     font = ImGui::GetFont();
        const char* s    = l.native_name.c_str();
        const char* end  = s + l.native_name.size();
        while (s < end)
        {
            unsigned int c = 0;
            s += ImTextCharFromUtf8(&c, s, end);
            if (c > IM_UNICODE_CODEPOINT_MAX || !font->IsGlyphInFont(static_cast<ImWchar>(c)))
                return l.english_name;
        }
        return l.native_name;
    };

    ImGui::TextUnformatted(_("Language"));
    ImGui::SameLine();
    HelpMarker(
        _("Applies the next time oshot starts, after saving.\n"
          "To add or test a translation, put its .po file in the locale folder of the config directory."));
    {
        std::string&                        lang    = g_config->File.language;
        const std::vector<i18n::language_t> langs   = i18n::available();
        const bool                          is_auto = lang.empty() || lang == "auto";
        const auto                          it      = std::ranges::find(langs, lang, &i18n::language_t::code);
        const std::string preview = is_auto ? _("System language") : (it != langs.end() ? language_name(*it) : lang);

        if (ImGui::BeginCombo("##config_language", preview.c_str()))
        {
            if (ImGui::Selectable(_("System language"), is_auto))
                lang = "auto";
            ImGui::Separator();
            for (const i18n::language_t& l : langs)
            {
                // The code as the ID, since names can repeat
                const bool selected = !is_auto && l.code == lang;
                if (ImGui::Selectable(fmt::format("{}##{}", language_name(l), l.code).c_str(), selected))
                    lang = l.code;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Spacing();

    // --- OCR path ---
    ImGui::TextUnformatted(_("Default OCR path"));
    ImGui::SameLine();
    HelpMarker(_("Full path to the OCR models (.traineddata). Supports drag-and-drop."));
    draw_input_text_folder("", "##config_ocr_path", refresh_models_func, g_config->File.ocr_path);
    ImGui::Spacing();

    // --- OCR model ---
    ImGui::TextUnformatted(_("Default OCR model"));
    ImGui::InputText("##config_ocr_model", &g_config->File.ocr_model);
    ImGui::Spacing();

    // --- Capture delay ---
    ImGui::TextUnformatted(_("Capture delay"));
    ImGui::SameLine();
    HelpMarker(
        _("Delay before acquiring a screenshot (milliseconds).\n"
          "Has no effect when opening an external image (e.g. -f flag)."));
    ImGui::InputInt("##config_delay", &g_config->File.delay, 5, 10);
    ImGui::Spacing();

    // --- Theme settings ---
    ImGui::TextUnformatted(_("Default Theme file path"));
    ImGui::SameLine();
    HelpMarker(
        _("Full path to the theme file (theme.toml by default), or relative to the config directory. Supports "
          "drag-and-drop."));
    draw_input_text_file("", "##config_theme_file_path", toml_filter, nullptr, g_config->File.theme_file_path);
    if (!fs::exists(g_config->GetThemeFilePath()))
        ImGui::TextColored((0xff7444FF_rgba), "%s", _("File doesn't exist, fallback to default hardcoded theme"));
    ImGui::Spacing();

    // A list instead of ImGui's "a\0b\0" string, whose items can't be translated one by one
    ImGui::TextUnformatted(_("Color picker style"));
    const auto picker_styles = std::to_array({ _("Bar - Square"), _("Wheel - Triangle") });
    ImGui::Combo(
        "##config_color_picker", &g_config->File.color_picker, picker_styles.data(), int(picker_styles.size()));
    ImGui::Spacing();

    ImGui::TextUnformatted(_("Annotation alpha editing"));
    const auto alpha_modes = std::to_array({ _("Disabled"), _("Inline slider"), _("Dedicated bar") });
    ImGui::Combo(
        "##config_color_picker_alpha_mode", &g_config->File.cpa_mode, alpha_modes.data(), int(alpha_modes.size()));
    ImGui::Spacing();

    // --- Checkboxes ---
#if !OSHOT_MACOS  // always fullscreen there
    ImGui::Checkbox(_("Fullscreen window##config_fullscreen"), &g_config->File.fullscreen);
    ImGui::SameLine();
    HelpMarker(
        _("Makes the overlay a fullscreen window, which covers panels and taskbars and which "
          "the compositor leaves alone.\n"
          "Turn it off only if your window manager misbehaves with fullscreen windows. "
          "Wayland always uses a fullscreen window. Takes effect on the next capture."));
#endif

    ImGui::Checkbox(_("Vertical sync (VSync)##config_vsync"), &g_config->File.enable_vsync);
    ImGui::SameLine();
    HelpMarker(
        _("Draws in step with your monitor's refresh rate, which looks smoother and draws no more "
          "frames than the monitor shows.\n"
          "Disable it if the overlay feels sluggish."));

    ImGui::Checkbox(_("Allow output edits##config_allow_out_edit"), &g_config->File.allow_out_edit);

    ImGui::Checkbox(_("Allow more than one tray##config_allow_multi_tray"), &g_config->File.allow_multi_tray);
    ImGui::SameLine();
    HelpMarker(
        _("Lets 'oshot --tray' start another tray icon when one is already running, instead of exiting. "
          "Only the first tray registers the global hotkey.\n"
          "Takes effect on the next launch."));

    ImGui::Checkbox(_("Allow more than one overlay##config_allow_multi_overlay"), &g_config->File.allow_multi_overlay);
    ImGui::SameLine();
    HelpMarker(
        _("Lets a capture open while another overlay is already open, e.g. to take a screenshot of oshot itself. "
          "When disabled, the new capture is ignored, so holding or mashing the capture key opens just one overlay."));

    ImGui::Checkbox(_("Show text tools at startup##config_show_text_tools"), &g_config->File.show_text_tools);

    ImGui::Checkbox(_("Prefer config variables over environment variables##config_pref_conf_to_env"),
                    &g_config->File.pref_conf_to_env);
    ImGui::SameLine();
    HelpMarker(
        _("Some systems, such as NixOS, point oshot to the OCR models from the package manager with environment "
          "variables like $TESSDATA_PREFIX. "
          "Enabling this makes the paths in the config file win over those variables."));

    ImGui::Checkbox(_("Consider annotations when scanning##config_render_anns"), &g_config->File.render_anns);
    ImGui::SameLine();
    HelpMarker(_("When enabled, annotations are included in the region passed to the text extractor."));

    ImGui::Checkbox(_("Legacy toolbar placement##config_old_toolbar_pos"), &g_config->File.old_toolbar_pos);
    ImGui::SameLine();
    HelpMarker(_("Put all the toolbar buttons on one line under the selection"));

    ImGui::Checkbox(_("Anti-aliased toolbar icons##config_aa_icons"), &g_config->File.aa_icons);
    ImGui::SameLine();
    HelpMarker(
        _("Draw the toolbar icons with smooth edges. Disable it for pixel-perfect icons.\n"
          "Applies from the next capture."));

    ImGui::Checkbox(_("Use CTRL+C to copy image##config_ctrl_c_copy_img"), &g_config->File.ctrl_c_copy_img);
    ImGui::SameLine();
    HelpMarker(
        _("Shortcut to use when copying the image selection.\n"
          "If disabled, the shortcut will be CTRL+SHIFT+C."));

#if OSHOT_HAS_GLOBAL_HOTKEY
    ImGui::Spacing();
    draw_hotkey_recorder(g_config->File.hotkey, window_just_opened);
#endif

    // --- Image filename output format section ---
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted(_("Output filename extension"));
    if (ImGui::BeginCombo("##config_image_out_ext", g_config->File.image_out_type.first.c_str()))
    {
        for (size_t i = 0; i < idx(ImageExt::COUNT); ++i)
        {
            const bool selected = std::cmp_equal(image_ext_sel, i);

            if (ImGui::Selectable(IMAGE_EXTS_STR[i].second, selected))
            {
                image_ext_sel                        = int(i);
                g_config->File.image_out_type.first  = IMAGE_EXTS_STR[i].second;
                g_config->File.image_out_type.second = toe<ImageExt>(i);
            }

            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    ImGui::TextUnformatted(_("Size preview unit"));
    ImGui::SameLine();
    HelpMarker(_("Unit used to show the selection's estimated file size next to the selection border"));
    if (ImGui::BeginCombo("##config_image_out_size_fmt", g_config->File.image_out_size_fmt.c_str()))
    {
        for (size_t i = 0; i < image_prev_units.size(); ++i)
        {
            const bool selected = std::cmp_equal(image_preuni_sel, i);

            if (ImGui::Selectable(image_prev_units[i], selected))
            {
                image_preuni_sel                  = int(i);
                g_config->File.image_out_size_fmt = image_prev_units[i];
            }

            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    ImGui::TextUnformatted(_("Output filename format"));
    ImGui::SameLine();
    HelpMarker(_("The image extension is appended automatically. Uses {fmt} chrono specifiers."));
    ImGui::Spacing();

    if (ImGui::InputText("##config_image_out_fmt", &g_config->File.image_out_fmt))
        r = get_config_image_out_fmt();

    if (!r.ok())
        ImGui::TextColored(Colors::RED, "%s", r.error_v().c_str());
    else
        ImGui::TextDisabled("%s", r.get().c_str());

    ImGui::Spacing();

    if (ImGui::TreeNode(_("Format help")))
    {
        auto row = [](const char* spec, const char* desc) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", spec);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(desc);
        };

        ImGui::Spacing();
        ImGui::TextDisabled("%s", _("Specifiers:"));
        ImGui::Indent();
        ImGui::TextDisabled("%s", _("Date:"));
        ImGui::TableSetupColumn("##spec", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupColumn("##desc", ImGuiTableColumnFlags_WidthStretch);
        if (ImGui::BeginTable("##fmt_date", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%Y", _("full year                (e.g. 2025)"));
            row("%y", _("2-digit year             (e.g. 25)"));
            row("%C", _("century                  (e.g. 20)"));
            row("%m", _("month (01-12)"));
            row("%d", _("day of month (01-31)"));
            row("%j", _("day of year (001-366)"));
            row("%F", _("short for %Y-%m-%d       (e.g. 2025-04-19)"));
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextDisabled("%s", _("Time:"));
        if (ImGui::BeginTable("##fmt_time", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%H", _("hour 24h      (00-23)"));
            row("%I", _("hour 12h      (01-12)"));
            row("%M", _("minute        (00-59)"));
            row("%S", _("second        (00-60)"));
            row("%p", _("AM / PM"));
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextDisabled("%s", _("Week / Weekday:"));
        if (ImGui::BeginTable("##fmt_week", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%u", _("weekday ISO                    (1=Mon ... 7=Sun)"));
            row("%w", _("weekday                        (0=Sun ... 6=Sat)"));
            row("%a", _("abbreviated weekday            (e.g. Sat)"));
            row("%A", _("full weekday name              (e.g. Saturday)"));
            row("%U", _("week of year, Sun-start        (00-53)"));
            row("%W", _("week of year, Mon-start        (00-53)"));
            row("%V", _("ISO week number                (01-53)"));
            row("%G", _("ISO week-based year            (e.g. 2025)"));
            row("%g", _("ISO week-based year, 2-digit   (e.g. 25)"));
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextDisabled("%s", _("Month name:"));
        if (ImGui::BeginTable("##fmt_month", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%b", _("abbreviated month           (e.g. Apr)"));
            row("%B", _("full month name             (e.g. April)"));
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextDisabled("%s", _("Other:"));
        if (ImGui::BeginTable("##fmt_other", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%%", _("literal %"));
            ImGui::EndTable();
        }
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::TextDisabled("%s", _("Examples:"));
        ImGui::Indent();
        ImGui::BulletText("oshot_{:%%F_%%H-%%M}-test ->  oshot_2025-04-19_14-30-test.%s",
                          g_config->File.image_out_type.first.c_str());
        ImGui::BulletText("oshot_{:%%F_%%H-%%M-%%S}   ->  oshot_2025-04-19_14-30-05.%s",
                          g_config->File.image_out_type.first.c_str());
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::TextDisabled("%s", _("Warnings:"));
        ImGui::Indent();
        // Through "%s": a translation can't then turn a "%" of these into a printf placeholder
        // xgettext:no-c-format
        ImGui::BulletText("%s", _("The colon inside {} is required: {:%F} correct, {%F} will error."));
        ImGui::BulletText("%s", _("Avoid %T -- it expands to HH:MM:SS (colons break paths on Windows)."));
        ImGui::BulletText("%s", _("%H-%M is 1-minute precise; add %S if you take rapid shots."));
        ImGui::Unindent();
        ImGui::Spacing();
        ImGui::TreePop();
    }

    draw_preference_fonts(window_just_opened);
}

#ifndef DISABLE_PLUGINS
// Draws a plugin's declarative settings, editing rt.settings_editing
static void draw_plugin_settings(plugin_runtime_t& rt)
{
    const std::span<const oshot_setting_t> settings = rt.Settings();
    for (size_t i = 0; i < settings.size() && i < rt.settings_editing.size(); ++i)
    {
        const oshot_setting_t& s   = settings[i];
        setting_value_t&       val = rt.settings_editing[i];

        ImGui::PushID(s.key);
        switch (s.kind)
        {
            case OSHOT_SETTING_BOOL: ImGui::Checkbox(s.label, &std::get<bool>(val)); break;
            case OSHOT_SETTING_STRING:
                if (s.flags & OSHOT_UI_MULTILINE)
                    ImGui::InputTextMultiline(s.label, &std::get<std::string>(val));
                else
                    ImGui::InputText(s.label,
                                     &std::get<std::string>(val),
                                     (s.flags & OSHOT_UI_PASSWORD) ? ImGuiInputTextFlags_Password : 0);
                break;
            case OSHOT_SETTING_INT:
            {
                const int64_t min = static_cast<int64_t>(s.min), max = static_cast<int64_t>(s.max), step = 1;
                if (min < max)
                    ImGui::SliderScalar(s.label,
                                        ImGuiDataType_S64,
                                        &std::get<int64_t>(val),
                                        &min,
                                        &max,
                                        nullptr,
                                        ImGuiSliderFlags_AlwaysClamp);
                else
                    ImGui::InputScalar(s.label, ImGuiDataType_S64, &std::get<int64_t>(val), &step);
                break;
            }
            case OSHOT_SETTING_FLOAT:
                if (s.min < s.max)
                    ImGui::SliderScalar(s.label,
                                        ImGuiDataType_Double,
                                        &std::get<double>(val),
                                        &s.min,
                                        &s.max,
                                        "%.3f",
                                        ImGuiSliderFlags_AlwaysClamp);
                else
                    ImGui::InputScalar(s.label, ImGuiDataType_Double, &std::get<double>(val), nullptr, nullptr, "%g");
                break;
            case OSHOT_SETTING_CHOICE:
            {
                auto& chosen = std::get<std::string>(val);
                if (ImGui::BeginCombo(s.label, chosen.c_str()))
                {
                    for (size_t j = 0; j < s.n_choices; ++j)
                    {
                        const bool selected = chosen == s.choices[j];
                        ImGui::PushID(static_cast<int>(j));
                        if (ImGui::Selectable(s.choices[j], selected))
                            chosen = s.choices[j];
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                break;
            }
        }
        if (s.help)
        {
            ImGui::SameLine();
            HelpMarker(s.help);
        }
        ImGui::PopID();
    }
}

// The text transformer chain as g_text_transformers has it: each plugin ID and whether its transformer is on
static std::vector<std::pair<std::string, bool>> current_text_transformers()
{
    std::vector<std::pair<std::string, bool>> ret;
    ret.reserve(g_text_transformers.size());
    for (const text_transformer_t& t : g_text_transformers)
        ret.emplace_back(t.rt->id, t.enabled);
    return ret;
}

// Draws the text transformers in the order being edited, with buttons to move them and switch them off
static void draw_text_transformers(std::vector<std::pair<std::string, bool>>& chain, bool busy)
{
    if (chain.empty())
        return;

    ImGui::SeparatorText(_("Text transformers"));
    ImGui::TextWrapped("%s",
                       _("They rewrite the text OCR and the QR/barcode scan read before it's shown, from top to "
                         "bottom. Each one gets the text the one above it returned."));
    if (busy)
        ImGui::TextDisabled("%s", _("Wait for the plugin operation to finish to change them."));

    ImGui::BeginDisabled(busy);
    std::optional<std::pair<size_t, size_t>> swap;
    for (size_t i = 0; i < chain.size(); ++i)
    {
        auto& [id, enabled] = chain[i];
        const auto  it      = g_plugins.find(id);
        const char* name    = it != g_plugins.end() ? it->second.plugin->name : id.c_str();

        ImGui::PushID(id.c_str());
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up))
            swap.emplace(i - 1, i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == chain.size());
        if (ImGui::ArrowButton("##down", ImGuiDir_Down))
            swap.emplace(i, i + 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox(fmt::format("{}. {}", i + 1, name).c_str(), &enabled);
        ImGui::SetItemTooltip("%s", id.c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    // After the loop, which holds references into the vector
    if (swap)
        std::swap(chain[swap->first], chain[swap->second]);
    ImGui::Spacing();
}

static void draw_preference_plugin(auto&                                      plugin_dirty,
                                   bool&                                      prefs_modified,
                                   std::vector<std::pair<std::string, bool>>& transformers,
                                   bool                                       busy)
{
    draw_text_transformers(transformers, busy);
    if (transformers != current_text_transformers())
        prefs_modified = true;

    bool any = !transformers.empty();
    for (auto& [id, rt] : g_plugins)
    {
        const oshot_plugin_t* pl           = rt.plugin;
        const bool            has_settings = !rt.Settings().empty();
        if (!has_settings && !pl->render_preferences)
            continue;
        any = true;

        ImGui::PushID(id.c_str());
        ImGui::SeparatorText(fmt::format("{} ({})", pl->name, pl->id).c_str());
        ImGui::Spacing();

        if (has_settings)
        {
            draw_plugin_settings(rt);
            if (rt.settings_editing != rt.settings_saved)
            {
                plugin_dirty[id] = true;
                prefs_modified   = true;
            }
        }

        // A custom page goes under the generated settings
        if (pl->render_preferences)
        {
            ImGui::BeginChild("##plugin_prefs", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
            if (rt.RenderPreferences())
            {
                plugin_dirty[id] = true;
                prefs_modified   = true;
            }
            ImGui::EndChild();
        }

        ImGui::PopID();
        ImGui::Spacing();
    }

    if (!any)
        ImGui::TextDisabled(
            "%s", g_plugins.empty() ? _("No plugins are loaded.") : _("None of the loaded plugins have settings."));
}

// Writes the edited chain to state.toml and applies it. Saved IDs of plugins that
// aren't loaded (disabled ones) keep their place after the loaded ones
static void save_text_transformers(StateManager& state, const std::vector<std::pair<std::string, bool>>& chain)
{
    state.Reload();
    const text_transformers_t saved = state.GetTextTransformers();

    text_transformers_t t;
    auto                in_chain = [&](const std::string& id) {
        return std::ranges::find(chain, id, &std::pair<std::string, bool>::first) != chain.end();
    };
    for (const auto& [id, enabled] : chain)
    {
        t.order.push_back(id);
        if (!enabled)
            t.disabled.push_back(id);
    }
    for (const std::string& id : saved.order)
        if (!in_chain(id))
            t.order.push_back(id);
    for (const std::string& id : saved.disabled)
        if (!in_chain(id))
            t.disabled.push_back(id);

    state.SetTextTransformers(t);
    MUST_OK(state.SaveState(), spdlog::error("Failed to save the text transformers' order: {}", _r.error_v()));
    sort_text_transformers(state);
}
#endif

static void draw_theme_editor()
{
    Config::theme_overrides_t& ov = g_config->theme_overrides;

    ImGui::SeparatorText(_("Custom theme options"));

    ImGui::Checkbox(_("Enable smooth animations"), &ov.smooth_animations);

    ImGui::SeparatorText(_("Style overrides"));

    // Rounding / border sliders
    auto style_row = [&](const char* label, float& val, float lo, float hi) {
        float v = (val < 0.f) ? 0.f : val;
        if (ImGui::SliderFloat(label, &v, lo, hi, "%.1f"))
            val = v;
        ImGui::SameLine();
        if (ImGui::SmallButton(fmt::format("{}##{}", _("reset"), label).c_str()))
            val = -1.f;
    };

    style_row(_("Window rounding"), ov.window_rounding, 0, 12);
    style_row(_("Frame rounding"), ov.frame_rounding, 0, 12);
    style_row(_("Grab rounding"), ov.grab_rounding, 0, 12);
    style_row(_("Tab rounding"), ov.tab_rounding, 0, 12);
    style_row(_("Window border"), ov.window_border, 0, 2);
    style_row(_("Frame border"), ov.frame_border, 0, 2);

    ImGui::Spacing();
    ImGui::SeparatorText(_("Color overrides"));
    ImGui::TextDisabled("%s", _("Click a swatch to edit. Changes apply on Save."));
    ImGui::Spacing();

    // Only the most used colors, so the list stays short. The theme file can set the others
    static constexpr auto important = std::to_array({
        "Text",          "TextDisabled",  "WindowBg",       "ChildBg",       "PopupBg",       "Border",
        "PlotHistogram", "FrameBg",       "FrameBgHovered", "FrameBgActive", "TitleBg",       "TitleBgActive",
        "MenuBarBg",     "ScrollbarBg",   "ScrollbarGrab",  "CheckMark",     "SliderGrab",    "SliderGrabActive",
        "Button",        "ButtonHovered", "ButtonActive",   "Header",        "HeaderHovered", "HeaderActive",
        "Tab",           "TabHovered",    "TabSelected",
    });

    const auto& cmap = color_name_map();

    ImGui::BeginChild("##color_list", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 13.0f), true);
    for (const char* name : important)
    {
        auto it = cmap.find(name);
        if (it == cmap.end())
            continue;

        // The override, else the live style
        ImVec4 col = ImGui::GetStyle().Colors[it->second];

        auto oit = ov.colors.find(name);
        if (oit != ov.colors.end())
        {
            const std::string& h = oit->second;
            if (!h.empty() && h[0] == '#')
                hexstr_to_imvec4(h, col);
        }
        ImGui::PushID(name);
        if (ImGui::ColorEdit4(
                "##c", reinterpret_cast<float*>(&col), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            ov.colors[name] = col_to_hexstr(col);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(name);

        // Allow clearing individual override
        if (oit != ov.colors.end())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                ov.colors.erase(name);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::Button(_("Clear all color overrides")))
        ov.colors.clear();
    ImGui::SameLine();
    HelpMarker(_("Reverts to the base theme colors on next Save."));
}

void ScreenshotTool::DrawPreferencesWindow()
{
    // Translated when drawn
#ifndef DISABLE_PLUGINS
    static constexpr auto items = std::to_array({ N_("Defaults"), N_("Plugins"), N_("Theme") });
#else
    static constexpr auto items = std::to_array({ N_("Defaults"), N_("Theme") });
#endif

    static bool    prefs_modified     = false;
    static bool    prev_window_open   = false;
    static bool    armed              = false;
    const bool     window_just_opened = !prev_window_open;
    static PrefTab selected_tab       = PrefTab::Defaults;

    static Config::config_file_t                 config_snapshot;  // config state at the moment the window opened
    static Config::theme_overrides_t             theme_snapshot;   // theme state at the moment the window opened
    static std::unordered_map<std::string, bool> plugin_dirty;
#ifndef DISABLE_PLUGINS
    static std::vector<std::pair<std::string, bool>> transformers;  // the chain as Preferences shows it until saved
    // state.toml can't be written while a plugin task uses it on its worker thread
    const bool plugin_busy = m_install_state && m_install_state->running;
#endif

    if (!m_show_window.Has(SubWindow::Preferences))
    {
        prev_window_open = false;
        release_preview_font();
        return;
    }

    prev_window_open = true;

    auto save_current_tab = [&]() {
        if (config_snapshot != g_config->File)
        {
            g_config->GenerateConfig(g_config->GetConfigPath(), true);
            g_config->LoadConfigFile(g_config->GetConfigPath());
            SyncRuntimeFromConfig();
            if (!g_config->File.theme_file_path.empty())
                g_config->LoadThemeFile(g_config->GetThemeFilePath());
            apply_imgui_theme();
            config_snapshot = g_config->File;
            theme_snapshot  = g_config->theme_overrides;
        }

        if (theme_snapshot != g_config->theme_overrides)
        {
            if (!g_config->File.theme_file_path.empty())
            {
                g_config->GenerateTheme(g_config->GetThemeFilePath(), true);
                g_config->LoadThemeFile(g_config->GetThemeFilePath());
                color_name_map().clear();
            }
            else
            {
                ImGui::OpenPopup(_("Theme file path is empty###theme_filepath_empty"));
            }

            apply_imgui_theme();
            theme_snapshot = g_config->theme_overrides;
        }

#ifndef DISABLE_PLUGINS
        if (!plugin_busy && transformers != current_text_transformers())
            save_text_transformers(m_plugin_manager.GetStateManager(), transformers);

        for (auto& [id, rt] : g_plugins)
        {
            if (!plugin_dirty[id])
                continue;

            if (rt.settings_editing != rt.settings_saved)
                rt.StoreSettings();
            if (rt.plugin->on_save_preferences)
                rt.plugin->on_save_preferences(rt.state);  // the plugin calls host->set_* here, filling rt.config
            rt.SaveConfig();
            if (OSHOT_PLUGIN_HAS(rt.plugin, on_settings_saved) && rt.plugin->on_settings_saved)
                rt.plugin->on_settings_saved(rt.state);
            plugin_dirty[id] = false;
        }
#endif
    };

    auto discard_current_tab = [&]() {
        g_config->File            = config_snapshot;
        g_config->theme_overrides = theme_snapshot;
#ifndef DISABLE_PLUGINS
        transformers = current_text_transformers();
        for (auto& [id, rt] : g_plugins)
        {
            if (!plugin_dirty[id])
                continue;
            rt.settings_editing = rt.settings_saved;
            if (rt.plugin->on_discard_preferences)
                rt.plugin->on_discard_preferences(rt.state);
            plugin_dirty[id] = false;
        }
#endif
    };

    // Snapshot the config when the window first appears so Discard can restore it.
    if (window_just_opened)
    {
        config_snapshot = g_config->File;
        theme_snapshot  = g_config->theme_overrides;
#ifndef DISABLE_PLUGINS
        // Edits left from a window that closed without Save or Discard are dropped
        plugin_dirty.clear();
        transformers = current_text_transformers();
        for (auto& [id, rt] : g_plugins)
            rt.LoadSettings();
#endif
    }

    if (config_snapshot != g_config->File || theme_snapshot != g_config->theme_overrides)
        prefs_modified = true;

    bool window_open = true;
    ImGui::SetNextWindowSize(font_relative(40, 34), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("Preferences###preferences_window"), &window_open, ImGuiWindowFlags_NoSavedSettings))
    {
        // [x] or Esc closes it, or asks first if there are unsaved changes.
        // OpenPopup() must be called between Begin() and End()
        if (!window_open || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            if (prefs_modified)
                ImGui::OpenPopup(_("Unsaved changes###pref_unsaved"));
            else
                m_show_window.Clear(SubWindow::Preferences);
        }

        float tabs_width = ImGui::GetFontSize() * 8.0f;
        for (const char* item : items)
            tabs_width = std::max(tabs_width, ImGui::CalcTextSize(i18n::tr(item)).x);
        tabs_width += ImGui::GetStyle().WindowPadding.x * 2.0f;

        // ResizeX saves the width in the ini and only uses ours the first time, so a minimum keeps the tabs
        // readable after a font or language change
        ImGui::SetNextWindowSizeConstraints(ImVec2(tabs_width, 0), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::BeginChild("##left_panel", ImVec2(tabs_width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        for (size_t i = 0; i < items.size(); i++)
            if (ImGui::Selectable(i18n::tr(items[i]), selected_tab == toe<PrefTab>(i)))
                selected_tab = toe<PrefTab>(i);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::BeginChild("##right_panel", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
        switch (selected_tab)
        {
            case PrefTab::kNone:    break;
            case PrefTab::Defaults: draw_preference_edit_config([&] { RefreshOcrModels(); }, window_just_opened); break;
#ifndef DISABLE_PLUGINS
            case PrefTab::Plugins:
                draw_preference_plugin(plugin_dirty, prefs_modified, transformers, plugin_busy);
                break;
#endif
            case PrefTab::Theme: draw_theme_editor(); break;
        }
        ImGui::EndChild();

        if (prefs_modified && create_timed_button(_("Save##save_press"), _("Saved!"), armed))
        {
            save_current_tab();
            prefs_modified = false;
            armed          = true;
        }
        else if (prefs_modified && selected_tab == PrefTab::Defaults)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", _("Restart oshot for the new settings to take effect"));
        }

        ImGui::EndGroup();

        // Confirmation modal
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(_("Unsaved changes###pref_unsaved"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(_("You have unsaved changes. What would you like to do?"));
            ImGui::Spacing();

            const ImVec2 size(button_width({ _("Save & Close"), _("Discard"), _("Cancel") }), 0);
            if (ImGui::Button(_("Save & Close"), size))
            {
                save_current_tab();
                m_show_window.Clear(SubWindow::Preferences);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(_("Discard"), size))
            {
                discard_current_tab();
                m_show_window.Clear(SubWindow::Preferences);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(_("Cancel"), size))
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
                _("Theme file path is empty###theme_filepath_empty"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(
                _("Please set a theme file path like 'theme.toml' before saving theme changes.\n"
                  "Changes will be set temporarily for the current session."));
            ImGui::Spacing();
            if (ImGui::Button(_("OK"), ImVec2(std::max(ImGui::GetFontSize() * 6.0f, button_width({ _("OK") })), 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::End();

    // Once closed, the next open starts fresh
    if (!m_show_window.Has(SubWindow::Preferences))
    {
        prev_window_open = false;
        prefs_modified   = false;
    }
}
