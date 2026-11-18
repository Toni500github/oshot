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
#include <array>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "fmt/os.h"
#include "spdlog/spdlog.h"
#include "texts.hpp"
#include "theme.hpp"
#include "util.hpp"
#include "util/color.hpp"
#include "util/dialogs.hpp"
#include "util/image.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
#include "util/strings.hpp"

using std::find;

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

#if !OSHOT_FREEDESKTOP
    // The install location isn't writable and the working directory is arbitrary when launched from
    // Explorer/Finder, so keep every OCR model in the config dir, seeded with the bundled ones
    File.ocr_path = (configDir / "models").string();
    SeedBundledModels(configDir / "models");
#endif

    if (!fs::exists(configFile))
    {
        spdlog::warn("Config file {} not found, generating new one", configFile.string());
        GenerateConfig(configFile.string());
    }
}

void Config::SeedBundledModels(const fs::path& models_dir)
{
    const fs::path& bundled = get_bundled_models_dir();
    if (bundled.empty())
        return;

    std::error_code ec;
    fs::create_directories(models_dir, ec);
    for (const fs::directory_entry& entry : fs::directory_iterator(bundled, ec))
    {
        if (entry.path().extension() != ".traineddata")
            continue;

        const fs::path& dst = models_dir / entry.path().filename();
        if (fs::exists(dst, ec))
            continue;

        if (fs::copy_file(entry.path(), dst, ec))
            spdlog::info("Copied bundled OCR model {} to {}", entry.path().filename().string(), models_dir.string());
        else
            spdlog::warn("Failed to copy bundled OCR model {}: {}", entry.path().string(), ec.message());
    }
}

void Config::LoadConfigFile(const std::string& filename)
{
    MUST_OK(LoadFile(filename), dialog::die("{}", _r.error_v()));

    File.ocr_path            = GetValue<std::string>("default.ocr-path", File.ocr_path);
    File.ocr_get_repo        = GetValue<std::string>("default.ocr-repo-download", "tesseract-ocr/tessdata");
    File.ocr_model           = GetValue<std::string>("default.ocr-model", "eng");
    File.theme_file_path     = GetValue<std::string>("default.theme-file", "theme.toml");
    File.image_out_fmt       = GetValue<std::string>("default.image-out-fmt", "oshot_{:%F_%H-%M}");
    File.delay               = GetValue<int>("default.delay", -1);
    File.color_picker        = GetValue<int>("default.color-picker", 0);
    File.cpa_mode            = GetValue<int>("default.color-picker-alpha-mode", 2);
    File.show_text_tools     = GetValue<bool>("default.show-text-tools", true);
    File.aa_icons            = GetValue<bool>("default.antialiased-icons", true);
    File.enable_vsync        = GetValue<bool>("default.vsync", true);
    File.fullscreen          = GetValue<bool>("default.fullscreen", true);
    File.allow_multi_tray    = GetValue<bool>("default.allow-multiple-trays", false);
    File.allow_multi_overlay = GetValue<bool>("default.allow-multiple-overlays", false);
    File.glx_vendor          = GetValue<std::string>("default.glx-vendor", "");
    File.old_toolbar_pos     = GetValue<bool>("default.legacy-toolbar-pos", false);
    File.pref_conf_to_env    = GetValue<bool>("default.config-over-env", false);
    File.render_anns         = GetValue<bool>("default.annotations-in-text-tools", true);
    File.ctrl_c_copy_img     = GetValue<bool>("default.ctrl-c-copy-img", true);
    File.allow_out_edit      = GetValue<bool>("default.allow-text-edit", false);
    File.hotkey              = GetValue<std::string>("default.hotkey", File.hotkey);
    File.language            = GetValue<std::string>("default.language", "auto");
    File.fonts               = GetValueArrayStr("default.fonts", {});
    File.builtin_font        = GetValue<bool>("default.builtin-font", false);
    File.font_size           = GetValue<int>("default.font-size", 0);

    const char* t = getenv("TESSDATA_PREFIX");
    if (!File.pref_conf_to_env && t)
        File.ocr_path = t;

    File.image_out_type.first = str_toupper(GetValue<std::string>("default.image-out-ext", "PNG"));
    const auto ext_it =
        std::ranges::find_if(IMAGE_EXTS_STR, [&](const auto& ext) { return File.image_out_type.first == ext.second; });
    if (ext_it != IMAGE_EXTS_STR.end())
    {
        File.image_out_type.second = ext_it->first;
    }
    else
    {
        spdlog::warn("Unknown image-out-ext '{}', using PNG (accepted: png, jpeg, bmp, tga)",
                     File.image_out_type.first);
        File.image_out_type.first  = "PNG";
        File.image_out_type.second = ImageExt::PNG;
    }

    static constexpr std::array<std::string_view, 19> prefixes = { "off", "auto", "B",   "KiB", "MiB", "GiB", "TiB",
                                                                   "PiB", "EiB",  "ZiB", "YiB", "KB",  "MB",  "GB",
                                                                   "TB",  "PB",   "EB",  "ZB",  "YB" };
    File.image_out_size_fmt = GetValue<std::string>("default.image-out-size-ind", "auto");
    if (std::ranges::find(prefixes, File.image_out_size_fmt) == prefixes.end())
        File.image_out_size_fmt = "auto";
}

std::string Config::GetThemeFilePath() const
{
    const fs::path p(File.theme_file_path);
    if (p.has_filename() && p.is_relative())
        return (fs::path(m_config_dir_path) / p).string();
    return File.theme_file_path;
}

// The modification time of `path`, min() when it's missing
static fs::file_time_type file_stamp(const std::string& path)
{
    std::error_code          ec;
    const fs::file_time_type t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : t;
}

// Read a theme file's color tables into `out`, over what it already holds
static void read_color_tables(const toml::table& tbl, Config::theme_colors_t& out, const std::string& filename)
{
    const auto read = [&](const char* table, theme::table_t& map, auto&& known) {
        const toml::table* t = tbl.at_path(table).as_table();
        if (!t)
            return;

        for (const auto& [k, v] : *t)
        {
            const std::string key(k.str());
            if (!known(key))
                spdlog::warn("Theme {}: [{}] has no color named '{}'", filename, table, key);
            else if (const toml::value<std::string>* str = v.as_string())
                map[key] = str->get();
            else
                spdlog::warn("Theme {}: [{}] {} isn't a string", filename, table, key);
        }
    };
    read("theme.palette", out.palette, theme::valid_palette_name);
    read("theme.colors", out.colors, [](const std::string& k) { return theme::imgui_color_from_name(k) >= 0; });
    read("theme.oshot-colors", out.oshot_colors, [](const std::string& k) {
        return theme::app_color_from_name(k).has_value();
    });
}

void Config::LoadThemeFile(const std::string& filename, const bool reload)
{
    // default.theme-file is usually relative to the config directory
    CdGuard guard(m_config_dir_path);

    const std::string path = filename.empty() ? filename : fs::absolute(filename).string();

    std::vector<std::pair<std::string, fs::file_time_type>> files;
    if (!path.empty())
        files.emplace_back(path, file_stamp(path));

    TomlAPI tbl;
    if (!path.empty() && fs::exists(path))
    {
        const Result<>& loaded = tbl.LoadFile(path);
        if (!loaded.ok())
        {
            if (reload)
            {
                // Remembered as read, so it's parsed again only once it changes
                spdlog::error("{}\nKeeping the current theme.", loaded.error_v());
                std::erase_if(m_theme_files, [&](const auto& f) { return f.first == path; });
                m_theme_files.push_back(files.front());
                return;
            }
            // A broken theme isn't worth refusing to start over, so it falls back to the default one
            dialog::error("{}\nUsing the default theme.", loaded.error_v());
        }
    }

    theme_overrides_t ov;
    read_color_tables(tbl.GetTbl(), ov, path);

    const std::string base = str_tolower(tbl.GetValue<std::string>("theme.base", "dark"));
    if (base == "light")
        ov.base = ThemeBase::Light;
    else if (base != "dark")
        spdlog::warn("Theme {}: unknown base '{}', using the dark one", path, base);

    const toml::node_view include    = tbl.GetTbl().at_path("theme.include");
    bool                  include_ok = !include || include.is_string() || include.is_array();
    if (const toml::value<std::string>* one = include.as_string())
    {
        ov.includes.push_back(one->get());
    }
    else if (const toml::array* many = include.as_array())
    {
        for (const toml::node& n : *many)
        {
            if (const toml::value<std::string>* str = n.as_string())
                ov.includes.push_back(str->get());
            else
                include_ok = false;
        }
    }

    if (!include_ok)
        spdlog::warn("Theme {}: include must be a path or a list of paths", path);

    ov.smooth_animations = tbl.GetValue<bool>("theme.smooth-animations", false);
    ov.window_rounding   = tbl.GetValue<float>("theme.style.window-rounding", -1.f);
    ov.frame_rounding    = tbl.GetValue<float>("theme.style.frame-rounding", -1.f);
    ov.grab_rounding     = tbl.GetValue<float>("theme.style.grab-rounding", -1.f);
    ov.tab_rounding      = tbl.GetValue<float>("theme.style.tab-rounding", -1.f);
    ov.window_border     = tbl.GetValue<float>("theme.style.window-border", -1.f);
    ov.frame_border      = tbl.GetValue<float>("theme.style.frame-border", -1.f);

    // Theme files from before the light theme listed every dark color, which would hide any other base
    if (ov.base != ThemeBase::Dark)
        if (const size_t n = theme::drop_base_copies(ov.colors, ThemeBase::Dark))
            spdlog::info("Theme {}: ignoring {} colors that repeat the dark theme's", path, n);

    theme_included_t included;
    const fs::path   dir = fs::path(path).parent_path();
    for (const std::string& written : ov.includes)
    {
        fs::path p = expand_var(written);
        if (p.is_relative())
            p = dir / p;
        const std::string inc_path = p.lexically_normal().string();
        files.emplace_back(inc_path, file_stamp(inc_path));

        TomlAPI inc;
        bool    ok = fs::exists(inc_path);
        if (!ok)
        {
            spdlog::warn("Theme {}: the include {} doesn't exist", path, inc_path);
        }
        else if (const Result<>& loaded = inc.LoadFile(inc_path); !loaded.ok())
        {
            spdlog::error("{}", loaded.error_v());
            ok = false;
        }
        else
        {
            read_color_tables(inc.GetTbl(), included, inc_path);
        }
        included.files.emplace_back(inc_path, ok);
    }

    theme_overrides = std::move(ov);
    theme_included  = std::move(included);
    m_theme_files   = std::move(files);

    const auto check = [&](const theme_colors_t& t, const std::string_view file) {
        for (const auto& [name, value] : t.palette)
            if (rgba_t col; !parse_hex_rgba(value, col))
                spdlog::warn(R"(Theme {}: palette color {} = "{}" isn't "#RRGGBB[AA]")", file, name, value);
        for (const auto* map : { &t.colors, &t.oshot_colors })
            for (const auto& [name, value] : *map)
                if (!theme::parse_color(value, *this))
                    spdlog::warn(R"(Theme {}: {} = "{}" isn't "#RRGGBB[AA]" or a "$name[/opacity]" in the palette)",
                                 file,
                                 name,
                                 value);
    };
    check(theme_overrides, path);
    if (!theme_included.files.empty())
        check(theme_included, "includes");
}

bool Config::ThemeFilesChanged() const
{
    return std::ranges::any_of(m_theme_files, [](const auto& f) { return file_stamp(f.first) != f.second; });
}

// fmt::output_file() throws if the file can't be created (missing folder, no permission)
static fmt::ostream open_output_file(const std::string& filename)
{
    try
    {
        return fmt::output_file(filename.data());
    }
    catch (const std::system_error& err)
    {
        dialog::die("Failed to write the file: {}", err.what());
    }
}

void Config::GenerateConfig(const std::string& filename, const bool force)
{
    if (!force && fs::exists(filename) &&
        !dialog::ask_user_yn(false, "WARNING: config file '{}' already exists. Do you want to overwrite it?", filename))
        std::exit(1);

    auto f = open_output_file(filename);

    std::string fonts_str;
    if (!File.fonts.empty())
    {
        for (const std::string& font : File.fonts)
            fonts_str += '"' + EscapeString(font) + "\", ";
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
            File.fullscreen,
            File.enable_vsync,
            File.allow_multi_tray,
            File.allow_multi_overlay,
            EscapeString(File.glx_vendor),
            File.allow_out_edit,
            File.show_text_tools,
            File.pref_conf_to_env,
            File.render_anns,
            File.old_toolbar_pos,
            File.aa_icons,
            File.ctrl_c_copy_img,
            EscapeString(File.hotkey),
            EscapeString(File.language),
            fonts_str,
            File.builtin_font,
            File.font_size,
            EscapeString(File.image_out_type.first),
            EscapeString(File.image_out_fmt),
            EscapeString(File.image_out_size_fmt),
            EscapeString(File.theme_file_path));
}

void Config::GenerateTheme(const std::string& filename, const bool force)
{
    if (!force && fs::exists(filename) &&
        !dialog::ask_user_yn(false, "WARNING: theme file '{}' already exists. Do you want to overwrite it?", filename))
        std::exit(1);

    CdGuard guard(m_config_dir_path);

    auto f = open_output_file(filename);
    if (!force)
    {
        f.print("{}{}{}", AUTOTHEME, THEME_COLORS_HEADER, AUTOTHEME_COLORS);
        for (size_t i = 0; i < idx(AppCol::COUNT); ++i)
            f.print("# {} = \"{}\"\n",
                    theme::app_color_name(toe<AppCol>(i)),
                    col_to_hexstr(theme::base_app_color(ThemeBase::Dark, toe<AppCol>(i))));
        return;
    }

    const theme_overrides_t& ov = theme_overrides;

    std::string includes;
    for (const std::string& inc : ov.includes)
        includes += fmt::format("{}\"{}\"", includes.empty() ? "" : ", ", EscapeString(inc));

    f.print(R"([theme]
# The built-in theme the settings below apply on top of: "dark" or "light"
base = "{}"

# Other theme files whose palette and color tables apply first, so this file's win.
# Relative to this file, ~ and $VARS work. A palette generator can write one of them
{}include = [{}]

smooth-animations = {}

# ---------------------------------------------------------------
# Rounding (pixels, 0 = sharp corners, max ~12)
# Border width in pixels. 0 = none, 1 = thin line.
# ---------------------------------------------------------------
[theme.style]
)",
            ov.base == ThemeBase::Light ? "light" : "dark",
            ov.includes.empty() ? "# " : "",
            ov.includes.empty() ? R"("~/.cache/wal/oshot.toml")" : includes,
            ov.smooth_animations);

    const auto print_size = [&](const std::string_view key, const float val) {
        if (val >= 0.f)
            f.print("{} = {:.1f}\n", key, val);
    };
    print_size("window-rounding", ov.window_rounding);
    print_size("frame-rounding", ov.frame_rounding);
    print_size("grab-rounding", ov.grab_rounding);
    print_size("tab-rounding", ov.tab_rounding);
    print_size("window-border", ov.window_border);
    print_size("frame-border", ov.frame_border);

    const auto print_table = [&](const theme::table_t& map) {
        for (const auto& [name, value] : map)
            f.print("{} = \"{}\"\n", name, EscapeString(value));
    };

    f.print("{}", THEME_COLORS_HEADER);
    f.print("\n[theme.palette]\n");
    print_table(ov.palette);
    f.print("\n[theme.colors]\n");
    print_table(ov.colors);
    f.print("\n[theme.oshot-colors]\n");
    print_table(ov.oshot_colors);
}
