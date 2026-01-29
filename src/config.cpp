#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "fmt/os.h"
#include "toml++/toml.hpp"
#include "util.hpp"

Config::Config(const std::string& configFile, const std::string& configDir)
{
    if (!std::filesystem::exists(configDir))
    {
        warn(_("oshot config folder was not found, Creating folders at {}!"), configDir);
        std::filesystem::create_directories(configDir);
    }

    if (!std::filesystem::exists(configFile))
    {
        warn(_("config file {} not found, generating new one"), configFile);
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
        die(_("Parsing config file '{}' failed:\n"
              "{}\n"
              "\t(error occurred at line {} column {})"),
            filename,
            err.description(),
            err.source().begin.line,
            err.source().begin.column);
    }

    File.ocr_path       = GetValue<std::string>("default.ocr-path", "/usr/share/tessdata");
    File.ocr_model      = GetValue<std::string>("default.ocr-model", "eng");
    File.lang_from      = GetValue<std::string>("default.lang-from", "auto");
    File.lang_to        = GetValue<std::string>("default.lang-to", "en-us");
    File.font           = GetValue<std::string>("default.font", "");
    File.allow_ocr_edit = GetValue<bool>("default.allow-edit-ocr", false);

    const toml::table* all_langs_tbl = m_tbl["lang"].as_table();
    if (!all_langs_tbl)
        return;

    for (const auto& [lang_code, lang_node] : *all_langs_tbl)
    {
        const toml::table* lang_tbl = lang_node.as_table();
        if (!lang_tbl)
            continue;

        const std::optional<std::string>& font_str = lang_tbl->at_path("font").value<std::string>();
        if (font_str)
            this->File.lang_fonts_paths[lang_code.data()] = font_str.value();
    }
}

void Config::OverrideOption(const std::string& opt)
{
    const size_t pos = opt.find('=');
    if (pos == std::string::npos)
        die(_("alias color '{}' does NOT have an equal sign '=' for separating color name and value\n"
              "For more check with --help"),
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
        die(_("looks like override value '{}' from '{}' is neither a bool, int or string value"), value, name);
}

void Config::GenerateConfig(const std::string& filename)
{
    if (std::filesystem::exists(filename))
    {
        if (!ask_user_yn(false, "WARNING: config file '{}' already exists. Do you want to overwrite it?", filename))
            std::exit(1);
    }

    auto f = fmt::output_file(filename.data());
    f.print("{}", AUTOCONFIG);
}
