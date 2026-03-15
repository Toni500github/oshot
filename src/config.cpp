#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "fmt/base.h"
#include "fmt/os.h"
#include "toml++/toml.hpp"
#include "util.hpp"

Config::Config(const std::string& configFile, const std::string& configDir)
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
    File.delay            = GetValue<int>("default.delay", -1);
    File.show_text_tools  = GetValue<bool>("default.show-text-tools", true);
    File.enable_vsync     = GetValue<bool>("default.vsync", true);
    File.real_full_screen = GetValue<bool>("default.real-full-screen", false);
    File.render_anns      = GetValue<bool>("default.annotations-in-text-tools", true);

    File.fonts = GetValueArrayStr("default.fonts", { GetValue<std::string>("default.font", "") });

    File.allow_out_edit = GetValue<bool>("default.allow-edit-ocr", false);  // deprecated
    File.allow_out_edit = GetValue<bool>("default.allow-text-edit", File.allow_out_edit);
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

void Config::GenerateConfig(const std::string& filename)
{
    if (fs::exists(filename))
    {
        if (!ask_user_yn(false, "WARNING: config file '{}' already exists. Do you want to overwrite it?", filename))
            std::exit(1);
    }

    auto f = fmt::output_file(filename.data());
    f.print("{}", AUTOCONFIG);
}
