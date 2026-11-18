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

#include "theme.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <span>
#include <string>
#include <utility>

#include "config.hpp"
#include "imgui/imgui.h"
#include "util.hpp"

#include "helpers.hpp"

namespace
{

struct app_color_t
{
    AppCol      col;
    const char* name;
    rgba_t      dark;
    rgba_t      light;
};

// The overlay draws on the screenshot, so its colors are the same in both themes
// clang-format off
// NOLINTBEGIN(modernize-use-designated-initializers)
constexpr auto APP_COLORS = std::to_array<app_color_t>({
    { AppCol::SelectionBorder,        "selection-border",         0x0096FFFF_rgba, 0x0096FFFF_rgba },
    { AppCol::SelectionHandle,        "selection-handle",         0xFFFFFFFF_rgba, 0xFFFFFFFF_rgba },
    { AppCol::SelectionHandleHovered, "selection-handle-hovered", 0xFFFF00FF_rgba, 0xFFFF00FF_rgba },
    { AppCol::SelectionDim,           "selection-dim",            0x00000080_rgba, 0x00000080_rgba },
    { AppCol::SizeLabelBg,            "size-label-bg",            0x0F0F0FD9_rgba, 0x0F0F0FD9_rgba },
    { AppCol::SizeLabelBorder,        "size-label-border",        0xFFFFFF40_rgba, 0xFFFFFF40_rgba },
    { AppCol::SizeLabelText,          "size-label-text",          0xFFFFFFFF_rgba, 0xFFFFFFFF_rgba },
    { AppCol::HintSelect,             "hint-select",              0x00FF00FF_rgba, 0x00FF00FF_rgba },
    { AppCol::HintFull,               "hint-full",                0x00FFFFFF_rgba, 0x00FFFFFF_rgba },
    { AppCol::HintSelectAll,          "hint-select-all",          0x0077FFFF_rgba, 0x0077FFFF_rgba },
    { AppCol::ToolSelected,           "tool-selected",            0x6699FFFF_rgba, 0x3D5FD9FF_rgba },
    { AppCol::TextMuted,              "text-muted",               0x999999FF_rgba, 0x666666FF_rgba },
    { AppCol::Info,                   "info",                     0x00AEFFFF_rgba, 0x0070B0FF_rgba },
    { AppCol::Success,                "success",                  0x2ECC71FF_rgba, 0x1E7B34FF_rgba },
    { AppCol::Warning,                "warning",                  0xFFCC33FF_rgba, 0x9A6700FF_rgba },
    { AppCol::Error,                  "error",                    0xFF6666FF_rgba, 0xC62828FF_rgba },
    { AppCol::Notice,                 "notice",                   0xFF7444FF_rgba, 0xB3471DFF_rgba },
    { AppCol::LogDebug,               "log-debug",                0x9999FFFF_rgba, 0x5555CCFF_rgba },
    { AppCol::ConfidenceLow,          "confidence-low",           0xA33131FF_rgba, 0xA33131FF_rgba },
    { AppCol::ConfidenceMid,          "confidence-mid",           0x9C7A1EFF_rgba, 0x9C7A1EFF_rgba },
    { AppCol::ConfidenceHigh,         "confidence-high",          0x1F8A47FF_rgba, 0x1F8A47FF_rgba },
    { AppCol::Tag,                    "tag",                      0x334C7FFF_rgba, 0x334C7FFF_rgba },
    { AppCol::TagNeutral,             "tag-neutral",              0x3F3F3FFF_rgba, 0x5F5F5FFF_rgba },
    { AppCol::TagSuccess,             "tag-success",              0x197233FF_rgba, 0x197233FF_rgba },
    { AppCol::TagWarning,             "tag-warning",              0x725919FF_rgba, 0x8A6A12FF_rgba },
    { AppCol::TagError,               "tag-error",                0x8C2626FF_rgba, 0x8C2626FF_rgba },
    { AppCol::Danger,                 "danger",                   0x8C1F1FFF_rgba, 0xC0392BFF_rgba },
    { AppCol::DangerHovered,          "danger-hovered",           0xB23333FF_rgba, 0xD9534FFF_rgba },
    { AppCol::DangerActive,           "danger-active",            0x730F0FFF_rgba, 0xA93226FF_rgba },
});
// NOLINTEND(modernize-use-designated-initializers)
// clang-format on
static_assert(APP_COLORS.size() == idx(AppCol::COUNT));

constexpr bool app_colors_in_order()
{
    for (size_t i = 0; i < APP_COLORS.size(); ++i)
        if (idx(APP_COLORS[i].col) != i)
            return false;
    return true;
}
static_assert(app_colors_in_order());

constexpr std::array<rgba_t, idx(AppCol::COUNT)> dark_app_colors() noexcept
{
    std::array<rgba_t, idx(AppCol::COUNT)> out{};
    for (const app_color_t& c : APP_COLORS)
        out[idx(c.col)] = c.dark;
    return out;
}

constinit std::array<rgba_t, idx(AppCol::COUNT)> g_app_colors = dark_app_colors();

using imgui_col_t = std::pair<ImGuiCol, rgba_t>;

// clang-format off
constexpr auto DARK = std::to_array<imgui_col_t>({
    { ImGuiCol_Text,             0xDCDDE1FF_rgba },
    { ImGuiCol_TextDisabled,     0x666870FF_rgba },
    { ImGuiCol_WindowBg,         0x0D1015FF_rgba },
    { ImGuiCol_ChildBg,          0x0D1015FF_rgba },
    { ImGuiCol_PopupBg,          0x10131AFF_rgba },
    { ImGuiCol_Border,           0x242933FF_rgba },
    { ImGuiCol_TitleBg,          0x090B0FFF_rgba },
    { ImGuiCol_TitleBgActive,    0x5274F0FF_rgba },
    { ImGuiCol_FrameBg,          0x151920FF_rgba },
    { ImGuiCol_FrameBgHovered,   0x202630FF_rgba },
    { ImGuiCol_FrameBgActive,    0x29313EFF_rgba },
    { ImGuiCol_Button,           0x294CC7FF_rgba },
    { ImGuiCol_ButtonHovered,    0x385DE0FF_rgba },
    { ImGuiCol_ButtonActive,     0x203CA6FF_rgba },
    { ImGuiCol_Header,           0x1A1F28FF_rgba },
    { ImGuiCol_HeaderHovered,    0x252C38FF_rgba },
    { ImGuiCol_HeaderActive,     0x303946FF_rgba },
    { ImGuiCol_Tab,              0x10141BFF_rgba },
    { ImGuiCol_TabHovered,       0x252E42FF_rgba },
    { ImGuiCol_TabSelected,      0x294CC7FF_rgba },
    { ImGuiCol_SliderGrab,       0x294CC7FF_rgba },
    { ImGuiCol_SliderGrabActive, 0x5274F0FF_rgba },
    { ImGuiCol_ScrollbarBg,      0x07090CFF_rgba },
    { ImGuiCol_ScrollbarGrab,    0x2A303AFF_rgba },
    { ImGuiCol_CheckMark,        0x5274F0FF_rgba },
    { ImGuiCol_MenuBarBg,        0x090B0FFF_rgba },
    { ImGuiCol_PlotHistogram,    0x5274F0FF_rgba },
});

constexpr auto LIGHT = std::to_array<imgui_col_t>({
    { ImGuiCol_Text,             0x1C1C1FFF_rgba },
    { ImGuiCol_TextDisabled,     0x424546FF_rgba },
    { ImGuiCol_WindowBg,         0xE9E9E9FF_rgba },
    { ImGuiCol_ChildBg,          0xE9E9E9FF_rgba },
    { ImGuiCol_PopupBg,          0xDDDDDDFF_rgba },
    { ImGuiCol_Border,           0xCFCFCFFF_rgba },
    { ImGuiCol_TitleBg,          0xE6E6E6FF_rgba },
    { ImGuiCol_TitleBgActive,    0x7694F5FF_rgba },
    { ImGuiCol_FrameBg,          0xD2D2D2FF_rgba },
    { ImGuiCol_FrameBgHovered,   0xC7C7C7FF_rgba },
    { ImGuiCol_FrameBgActive,    0xCFCFD8FF_rgba },
    { ImGuiCol_Button,           0x6F8EF0FF_rgba },
    { ImGuiCol_ButtonHovered,    0x85A0F5FF_rgba },
    { ImGuiCol_ButtonActive,     0x5C7DE8FF_rgba },
    { ImGuiCol_Header,           0xCCCCCCFF_rgba },
    { ImGuiCol_HeaderHovered,    0xCAD1E0FF_rgba },
    { ImGuiCol_HeaderActive,     0xD2D9E3FF_rgba },
    { ImGuiCol_Tab,              0xE9E9E9FF_rgba },
    { ImGuiCol_TabHovered,       0xE2E7EFFF_rgba },
    { ImGuiCol_TabSelected,      0x6F8EF0FF_rgba },
    { ImGuiCol_SliderGrab,       0x6F8EF0FF_rgba },
    { ImGuiCol_SliderGrabActive, 0x8AA4FFFF_rgba },
    { ImGuiCol_ScrollbarBg,      0xE9E9E9FF_rgba },
    { ImGuiCol_ScrollbarGrab,    0xC5CAD3FF_rgba },
    { ImGuiCol_CheckMark,        0x7694F5FF_rgba },
    { ImGuiCol_MenuBarBg,        0xE6E6E6FF_rgba },
    { ImGuiCol_PlotHistogram,    0x7694F5FF_rgba },
});
// clang-format on

// The first layer of `name` whose value parses
theme::resolved_t resolve(const theme::table_t&  own,
                          const theme::table_t&  included,
                          const std::string_view name,
                          const Config&          cfg)
{
    for (const auto& [table, source] :
         { std::pair{ &own, theme::Source::File }, std::pair{ &included, theme::Source::Include } })
        if (const auto it = table->find(name); it != table->end())
            if (const std::optional<rgba_t> col = theme::parse_color(it->second, cfg))
                return { .value = col, .source = source, .raw = it->second };
    return {};
}

}  // namespace

namespace theme
{

bool valid_palette_name(const std::string_view name)
{
    return !name.empty() && std::ranges::all_of(name, [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

const char* app_color_name(const AppCol col)
{
    return APP_COLORS[idx(col)].name;
}

std::optional<AppCol> app_color_from_name(const std::string_view name)
{
    for (const app_color_t& c : APP_COLORS)
        if (name == c.name)
            return c.col;
    return std::nullopt;
}

rgba_t base_app_color(const ThemeBase base, const AppCol col)
{
    const app_color_t& c = APP_COLORS[idx(col)];
    return base == ThemeBase::Light ? c.light : c.dark;
}

size_t drop_base_copies(table_t& colors, const ThemeBase base)
{
    const std::span<const imgui_col_t> table = base == ThemeBase::Light ? std::span(LIGHT) : std::span(DARK);
    return std::erase_if(colors, [&](const auto& entry) {
        rgba_t     col;
        const int  i  = imgui_color_from_name(entry.first);
        const auto it = std::ranges::find(table, i, &imgui_col_t::first);
        return it != table.end() && parse_hex_rgba(entry.second, col) && col.to_rgba() == it->second.to_rgba();
    });
}

int imgui_color_from_name(const std::string_view name)
{
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        if (name == ImGui::GetStyleColorName(i))
            return i;
    return -1;
}

void style_base(const ThemeBase base, ImGuiStyle& style)
{
    std::span<const imgui_col_t> colors;
    if (base == ThemeBase::Light)
    {
        ImGui::StyleColorsLight(&style);
        colors = LIGHT;
    }
    else
    {
        ImGui::StyleColorsDark(&style);
        colors = DARK;
    }

    for (const auto& [col, rgba] : colors)
        style.Colors[col] = rgba.to_imvec4();

    style.WindowRounding = 4.0f;
    style.FrameRounding  = 4.0f;
    style.GrabRounding   = 1.5f;
    style.TabRounding    = 2.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize  = 0.0f;
}

std::optional<rgba_t> palette_color(const std::string_view name, const Config& cfg)
{
    rgba_t col;
    for (const table_t* palette : { &cfg.theme_overrides.palette, &cfg.theme_included.palette })
        if (const auto it = palette->find(name); it != palette->end() && parse_hex_rgba(it->second, col))
            return col;
    return std::nullopt;
}

std::optional<rgba_t> parse_color(const std::string_view value, const Config& cfg)
{
    if (!value.starts_with('$'))
    {
        rgba_t col;
        if (parse_hex_rgba(value, col))
            return col;
        return std::nullopt;
    }

    std::string_view   name = value.substr(1);
    std::optional<int> opacity;
    if (const size_t slash = name.find('/'); slash != std::string_view::npos)
    {
        const std::string_view pct = name.substr(slash + 1);
        int                    n   = 0;
        const auto [end, ec]       = std::from_chars(pct.data(), pct.data() + pct.size(), n);
        if (ec != std::errc() || end != pct.data() + pct.size() || n < 0 || n > 100)
            return std::nullopt;
        opacity = n;
        name    = name.substr(0, slash);
    }

    std::optional<rgba_t> col = palette_color(name, cfg);
    if (col && opacity)
        col->a = uint8_t(std::lround(*opacity * 255.0 / 100.0));
    return col;
}

resolved_t resolve_imgui_color(const std::string_view name, const Config& cfg)
{
    return resolve(cfg.theme_overrides.colors, cfg.theme_included.colors, name, cfg);
}

resolved_t resolve_app_color(const AppCol col, const Config& cfg)
{
    return resolve(cfg.theme_overrides.oshot_colors, cfg.theme_included.oshot_colors, app_color_name(col), cfg);
}

void apply(const Config& cfg, ImGuiStyle& style)
{
    const Config::theme_overrides_t& ov = cfg.theme_overrides;
    style_base(ov.base, style);

    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        if (const auto col = resolve_imgui_color(ImGui::GetStyleColorName(i), cfg).value)
            style.Colors[i] = col->to_imvec4();

    for (const app_color_t& c : APP_COLORS)
        g_app_colors[idx(c.col)] = resolve_app_color(c.col, cfg).value.value_or(base_app_color(ov.base, c.col));

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
}

rgba_t color(const AppCol col)
{
    return g_app_colors[idx(col)];
}

}  // namespace theme
