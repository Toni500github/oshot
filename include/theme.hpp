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

#ifndef _THEME_HPP_
#define _THEME_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "util/color.hpp"

class Config;
struct ImGuiStyle;

// The built-in themes the theme file applies on top of
enum class ThemeBase : std::uint8_t
{
    Dark,
    Light
};

// oshot's own colors, the [theme.oshot-colors] table
enum class AppCol : std::uint8_t
{
    SelectionBorder,
    SelectionHandle,
    SelectionHandleHovered,
    SelectionDim,
    SizeLabelBg,
    SizeLabelBorder,
    SizeLabelText,
    HintSelect,
    HintFull,
    HintSelectAll,
    ToolSelected,
    TextMuted,
    Info,
    Success,
    Warning,
    Error,
    Notice,
    LogDebug,
    ConfidenceLow,
    ConfidenceMid,
    ConfidenceHigh,
    Tag,
    TagNeutral,
    TagSuccess,
    TagWarning,
    TagError,
    Danger,
    DangerHovered,
    DangerActive,
    COUNT
};

namespace theme
{

// A theme color table: name -> value as written. Transparent, so lookups by string_view don't allocate
using table_t = std::map<std::string, std::string, std::less<>>;

// Where a color's value comes from
enum class Source : std::uint8_t
{
    Base,
    Include,
    File
};

struct resolved_t
{
    std::optional<rgba_t> value;  // nullopt for the base's
    Source                source = Source::Base;
    std::string_view      raw;  // as written, empty for the base
};

/**
 * Whether `name` can name a palette color: letters, digits, '-' and '_', so it's a bare TOML key
 * @param name The name, without '$'
 **/
bool valid_palette_name(std::string_view name);

/**
 * The key of an oshot color in [theme.oshot-colors]
 * @param col The color
 **/
const char* app_color_name(AppCol col);

/**
 * An oshot color by its [theme.oshot-colors] key
 * @param name The key
 * @return nullopt if no color has that key
 **/
std::optional<AppCol> app_color_from_name(std::string_view name);

/**
 * The base theme's value of an oshot color
 * @param base The base theme
 * @param col The color
 **/
rgba_t base_app_color(ThemeBase base, AppCol col);

/**
 * An ImGuiCol by its GetStyleColorName() name
 * @param name The name, e.g. "WindowBg"
 * @return -1 if no color has that name
 **/
int imgui_color_from_name(std::string_view name);

/**
 * Remove the colors that repeat what a base theme sets, which older theme files listed in full
 * @param colors ImGuiCol name -> value
 * @param base The base theme
 * @return How many were removed
 **/
size_t drop_base_copies(table_t& colors, ThemeBase base);

/**
 * Set the base theme's colors and sizes. Leaves the rest of the style alone
 * @param base The base theme
 * @param style The style to change
 **/
void style_base(ThemeBase base, ImGuiStyle& style);

/**
 * Parse a theme color value: "#RRGGBB[AA]", or "$name" from the palette with an optional
 * opacity in percent, "$name/50"
 * @param value The value as written
 * @param cfg The config with the theme's palette
 * @return nullopt if it isn't a color, or names a missing palette entry
 **/
std::optional<rgba_t> parse_color(std::string_view value, const Config& cfg);

/**
 * A palette entry, the theme file's own over the includes'
 * @param name The entry's name, without '$'
 * @param cfg The config with the theme's palette
 **/
std::optional<rgba_t> palette_color(std::string_view name, const Config& cfg);

/**
 * What an ImGui color ends up as: the theme file's value, else the includes', else the base's.
 * A value that doesn't parse falls through to the next one. The base's value is left to the caller,
 * since ImGui's own colors are floats that a round trip through rgba_t would change
 * @param name The ImGui color's name
 * @param cfg The config with the theme
 **/
resolved_t resolve_imgui_color(std::string_view name, const Config& cfg);

/**
 * resolve_imgui_color() for an oshot color
 * @param col The color
 * @param cfg The config with the theme
 **/
resolved_t resolve_app_color(AppCol col, const Config& cfg);

/**
 * Apply the config's theme: the base, the includes' colors, the theme file's colors and sizes,
 * and oshot's own colors, which color() returns from then on
 * @param cfg The config with the theme
 * @param style The style to set
 **/
void apply(const Config& cfg, ImGuiStyle& style);

/**
 * An oshot color of the theme apply() set last, the dark base's before that
 * @param col The color
 **/
rgba_t color(AppCol col);

}  // namespace theme

#endif  // !_THEME_HPP_
