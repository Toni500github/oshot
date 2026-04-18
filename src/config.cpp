#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "fmt/base.h"
#include "fmt/os.h"
#include "toml++/toml.hpp"
#include "util.hpp"

static fs::path old_pwd;

static void cd(const fs::path& path)
{
    if (!path.empty())
    {
        old_pwd = fs::current_path();
        fs::current_path(path);
    }
}

static void cd_back()
{
    if (!old_pwd.empty())
        fs::current_path(old_pwd);
}

Config::Config(const std::string& configFile, const std::string& configDir)
    : m_config_path(configFile), m_config_dir_path(configDir)
{
    if (!fs::exists(configDir))
    {
        warn("oshot config folder was not found, Creating folders at {}!", configDir);
        fs::create_directories(configDir);
    }

    if (!fs::exists(configFile))
    {
        warn("config file {} not found, generating new one", configFile);
        GenerateConfig(configFile);
    }
}

void Config::LoadConfigFile(const std::string& filename)
{
    try
    {
        m_tbl = toml::parse_file(filename);
    }
    catch (const toml::parse_error& err)
    {
        die("Parsing config file '{}' failed:\n"
            "{}\n"
            "\t(error occurred at line {} column {})",
            filename,
            err.description(),
            err.source().begin.line,
            err.source().begin.column);
    }

    File.ocr_path         = GetValue<std::string>("default.ocr-path", "/usr/share/tessdata");
    File.ocr_model        = GetValue<std::string>("default.ocr-model", "eng");
    File.theme_style      = GetValue<std::string>("default.theme", "auto");
    File.theme_file_path  = GetValue<std::string>("default.theme-file", "");
    File.delay            = GetValue<int>("default.delay", -1);
    File.show_text_tools  = GetValue<bool>("default.show-text-tools", true);
    File.enable_vsync     = GetValue<bool>("default.vsync", true);
    File.real_full_screen = GetValue<bool>("default.real-full-screen", false);
    File.render_anns      = GetValue<bool>("default.annotations-in-text-tools", true);

    File.fonts = GetValueArrayStr("default.fonts", { GetValue<std::string>("default.font", "") });

    File.allow_out_edit = GetValue<bool>("default.allow-edit-ocr", false);  // deprecated
    File.allow_out_edit = GetValue<bool>("default.allow-text-edit", File.allow_out_edit);
}

void Config::LoadThemeFile(const std::string& filename)
{
    m_theme_path = filename;

    // Since the filename (default.theme-file) will be likely
    // related to relative path of the config directory, let's
    // snapshot and switch to that directory.
    cd(m_config_dir_path);

    if (fs::exists(filename))
    {
        try
        {
            m_theme_tbl = toml::parse_file(filename);
        }
        catch (const toml::parse_error& err)
        {
            // Snap back
            fs::current_path(old_pwd);
            die("Parsing theme file '{}' failed:\n"
                "{}\n"
                "\t(error occurred at line {} column {})",
                filename,
                err.description(),
                err.source().begin.line,
                err.source().begin.column);
        }
    }

    cd_back();

    theme_overrides_t& ov = theme_overrides;
    if (const toml::table* colors = m_theme_tbl.at_path("theme.colors").as_table())
    {
        colors->for_each(
            [&](const toml::key& k, const toml::value<std::string>& v) { ov.colors[std::string(k.str())] = v.get(); });
    }

    ov.window_rounding = GetThemeStyleValue("window-rounding", -1.f);
    ov.frame_rounding  = GetThemeStyleValue("frame-rounding", -1.f);
    ov.grab_rounding   = GetThemeStyleValue("grab-rounding", -1.f);
    ov.tab_rounding    = GetThemeStyleValue("tab-rounding", -1.f);
    ov.window_border   = GetThemeStyleValue("window-border", -1.f);
    ov.frame_border    = GetThemeStyleValue("frame-border", -1.f);
}

void Config::OverrideOption(const std::string& opt)
{
    const size_t pos = opt.find('=');
    if (pos == std::string::npos)
        die("option to override '{}' does NOT have an equal sign '=' for separating name and value\n"
            "For more check with --help",
            opt);

    std::string        name{ opt.substr(0, pos) };
    const std::string& value = opt.substr(pos + 1);

    // usually the user finds incovinient to write "default.foo"
    // for general config options
    if (name.find('.') == name.npos)
        name.insert(0, "default.");

    if (value == "true")
        m_overrides[name] = { .value_type = ValueType::kBool, .bool_value = true };
    else if (value == "false")
        m_overrides[name] = { .value_type = ValueType::kBool, .bool_value = false };
    else if ((value[0] == '"' && value.back() == '"') || (value[0] == '\'' && value.back() == '\''))
        m_overrides[name] = { .value_type = ValueType::kString, .string_value = value.substr(1, value.size() - 2) };
    else if (std::ranges::all_of(value, ::isdigit))
        m_overrides[name] = { .value_type = ValueType::kInt, .int_value = std::stoi(value) };
    else
        die("looks like override value '{}' from '{}' is neither a bool, int or string value", value, name);
}

void Config::GenerateConfig(const std::string& filename, const bool force)
{
    if (!force && fs::exists(filename) &&
        !ask_user_yn(false, "WARNING: config file '{}' already exists. Do you want to overwrite it?", filename))
        std::exit(1);

    cd(m_config_dir_path);

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
            File.ocr_path,
            File.ocr_model,
            File.delay,
            File.real_full_screen,
            File.enable_vsync,
            File.allow_out_edit,
            File.show_text_tools,
            File.render_anns,
            fonts_str,
            File.theme_style,
            File.theme_file_path);

    cd_back();
}

void Config::GenerateTheme(const std::string& filename, const bool force)
{
    if (!force && fs::exists(filename) &&
        !ask_user_yn(false, "WARNING: theme file '{}' already exists. Do you want to overwrite it?", filename))
        std::exit(1);

    cd(m_config_dir_path);

    auto f = fmt::output_file(filename.data());
    if (!force)
    {
        f.print("{}", AUTOTHEME);
        return;
    }

    theme_overrides_t& ov = theme_overrides;
    f.print(R"([theme]
# Drop this next to config.toml or point theme-file at its path.
# All sections and keys are optional — omit anything you don't want to override.

# ---------------------------------------------------------------
# Rounding (pixels, 0 = sharp corners, max ~12)
# ---------------------------------------------------------------
[style]
window-rounding = {}
frame-rounding  = {}
grab-rounding   = {}
tab-rounding    = {}

# Border width in pixels. 0 = none, 1 = thin line.
window-border = {}
frame-border  = {}
)",
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
[colors]
)");

    for (const auto& [name, hex] : ov.colors)
        f.print("{} = \"{}\"\n", name, hex);

    cd_back();
}
