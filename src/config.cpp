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

#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "fmt/base.h"
#include "fmt/os.h"
#include "texts.hpp"
#include "util.hpp"

Config::Config(const fs::path& configFile, const fs::path& configDir)
    : m_config_path(configFile.string()), m_config_dir_path(configDir.string())
{
    if (!fs::exists(configDir))
    {
        spdlog::warn("Oshot config folder was not found, creating folders at {}!", configDir.string());
        fs::create_directories(configDir);
        fs::create_directories(configDir / "models");
        fs::create_directories(configDir / "plugins");
    }

    if (!fs::exists(configFile))
    {
        spdlog::warn("Config file {} not found, generating new one", configFile.string());
        GenerateConfig(configFile.string());
    }
}

void Config::LoadConfigFile(const std::string& filename)
{
    MUST_OK(LoadFile(filename), die("{}", _r.error_v()));

    File.ocr_path         = GetValue<std::string>("default.ocr-path", File.ocr_path);
    File.ocr_get_repo     = GetValue<std::string>("default.ocr-repo-downlaod", "tesseract-ocr/tessdata");
    File.ocr_model        = GetValue<std::string>("default.ocr-model", "eng");
    File.theme_file_path  = GetValue<std::string>("default.theme-file", "theme.toml");
    File.image_out_fmt    = GetValue<std::string>("default.image-out-fmt", "oshot_{:%F_%H-%M}");
    File.delay            = GetValue<int>("default.delay", -1);
    File.show_text_tools  = GetValue<bool>("default.show-text-tools", true);
    File.enable_vsync     = GetValue<bool>("default.vsync", true);
    File.real_full_screen = GetValue<bool>("default.real-full-screen", false);
    File.pref_conf_to_env = GetValue<bool>("default.config-over-env", false);
    File.render_anns      = GetValue<bool>("default.annotations-in-text-tools", true);
    File.ctrl_c_copy_img  = GetValue<bool>("default.ctrl-c-copy-img", false);

    File.fonts = GetValueArrayStr("default.fonts", { GetValue<std::string>("default.font", "") });

    File.allow_out_edit = GetValue<bool>("default.allow-edit-ocr", false);  // deprecated
    File.allow_out_edit = GetValue<bool>("default.allow-text-edit", File.allow_out_edit);

    const char* t;
    if (!File.pref_conf_to_env && (t = getenv("TESSDATA_PREFIX")))
        File.ocr_path = t;

    File.image_out_type.first = str_toupper(GetValue<std::string>("default.image-out-ext", "PNG"));
    if (IMAGE_EXTS_ENUM.find(File.image_out_type.first) != IMAGE_EXTS_ENUM.end())
    {
        File.image_out_type.second = IMAGE_EXTS_ENUM.at(File.image_out_type.first);
    }
    else
    {
        File.image_out_type.first  = "PNG";
        File.image_out_type.second = ImageExt::PNG;
    }

    static constexpr std::array<std::string_view, 19> prefixes = { "off", "auto", "B",   "KiB", "MiB", "GiB", "TiB",
                                                                   "PiB", "EiB",  "ZiB", "YiB", "KB",  "MB",  "GB",
                                                                   "TB",  "PB",   "EB",  "ZB",  "YB" };
    File.image_out_size_fmt = GetValue<std::string>("default.image-out-size-ind", "auto");
    if (std::find(prefixes.begin(), prefixes.end(), File.image_out_size_fmt) == prefixes.end())
        File.image_out_size_fmt = "auto";

    {
        std::string& s = g_config->File.theme_file_path;
        fs::path     p(s);
        if (p.has_filename() && p.is_relative())
            s.insert(0, m_config_dir_path + DIR_SEP_STR);
    }
}

void Config::LoadThemeFile(const std::string& filename)
{
    m_theme_path    = filename;
    theme_overrides = {};
    theme_overrides.colors.clear();

    // Since the filename (default.theme-file) will be likely
    // related to relative path of the config directory, let's
    // snapshot and switch to that directory.
    CdGuard guard(m_config_dir_path);

    if (fs::exists(filename))
    {
        MUST_OK(m_theme.LoadFile(filename), die("{}", _r.error_v()));
        m_theme_path = filename;
    }

    theme_overrides_t& ov = theme_overrides;
    if (const toml::table* colors = m_theme.GetTbl().at_path("theme.colors").as_table())
    {
        colors->for_each(
            [&](const toml::key& k, const toml::value<std::string>& v) { ov.colors[k.str().data()] = v.get(); });
    }

    ov.smooth_animations = m_theme.GetValue<bool>("smooth-animations", false);
    ov.window_rounding   = m_theme.GetValue<float>("window-rounding", -1.f);
    ov.frame_rounding    = m_theme.GetValue<float>("frame-rounding", -1.f);
    ov.grab_rounding     = m_theme.GetValue<float>("grab-rounding", -1.f);
    ov.tab_rounding      = m_theme.GetValue<float>("tab-rounding", -1.f);
    ov.window_border     = m_theme.GetValue<float>("window-border", -1.f);
    ov.frame_border      = m_theme.GetValue<float>("frame-border", -1.f);
}

void Config::GenerateConfig(const std::string& filename, const bool force)
{
    if (!force && fs::exists(filename) &&
        !ask_user_yn(false, "WARNING: config file '{}' already exists. Do you want to overwrite it?", filename))
        std::exit(1);

    auto f = fmt::output_file(filename.data());

    std::string fonts_str;
    if (!File.fonts.empty())
    {
        for (const std::string& font : File.fonts)
            fonts_str += '\'' + font + "', ";
        fonts_str.pop_back();  // ' '
        fonts_str.pop_back();  // ','
    }

    f.print(AUTOCONFIG,
            EscapeString(File.ocr_path),
            EscapeString(File.ocr_model),
            EscapeString(File.ocr_get_repo),
            File.delay,
            File.color_picker,
            File.cpa_mode,
            File.real_full_screen,
            File.enable_vsync,
            File.allow_out_edit,
            File.show_text_tools,
            File.pref_conf_to_env,
            File.render_anns,
            File.ctrl_c_copy_img,
            EscapeString(fonts_str),
            EscapeString(File.image_out_type.first),
            EscapeString(File.image_out_fmt),
            EscapeString(File.image_out_size_fmt),
            EscapeString(File.theme_file_path));
}

void Config::GenerateTheme(const std::string& filename, const bool force)
{
    if (!force && fs::exists(filename) &&
        !ask_user_yn(false, "WARNING: theme file '{}' already exists. Do you want to overwrite it?", filename))
        std::exit(1);

    CdGuard guard(m_config_dir_path);

    auto f = fmt::output_file(filename.data());
    if (!force)
    {
        f.print("{}", AUTOTHEME);
        return;
    }

    theme_overrides_t& ov = theme_overrides;
    f.print(R"([theme]
smooth-animations = true

# All sections and keys are optional. Omit anything you don't want to override.

# ---------------------------------------------------------------
# Rounding (pixels, 0 = sharp corners, max ~12)
# ---------------------------------------------------------------
[theme.style]
window-rounding = {}
frame-rounding  = {}
grab-rounding   = {}
tab-rounding    = {}

# Border width in pixels. 0 = none, 1 = thin line.
window-border = {}
frame-border  = {}
)",
            ov.smooth_animations,
            ov.window_rounding,
            ov.frame_rounding,
            ov.grab_rounding,
            ov.tab_rounding,
            ov.window_border,
            ov.frame_border);

    f.print(R"(
# ---------------------------------------------------------------
# Color overrides
# Format: "#RRGGBBAA"
# Only the entries you list here are overridden;
# everything else falls back to the base theme.
#
# Full list of valid names:
#   https://github.com/ocornut/imgui/blob/master/imgui.cpp
#   (search for "GetStyleColorName")
# ---------------------------------------------------------------
[theme.colors]
)");

    for (const auto& [name, hex] : ov.colors)
        f.print("{} = \"{}\"\n", name, hex);
}
