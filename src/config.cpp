#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "fmt/os.h"
#include "util.hpp"

Config::Config(const std::string& configFile, const std::string& configDir)
{
    if (!std::filesystem::exists(configDir))
    {
        warn(_("customfetch config folder was not found, Creating folders at {}!"), configDir);
        std::filesystem::create_directories(configDir);
    }

    if (!std::filesystem::exists(configFile))
    {
        warn(_("config file {} not found, generating new one"), configFile);
        generateConfig(configFile);
    }
}

void Config::loadConfigFile(const std::string& filename)
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

    ocr_path       = getValue<std::string>("default.ocr-path", "/usr/share/tessdata");
    ocr_model      = getValue<std::string>("default.ocr-model", "eng");
    gawk_path      = getValue<std::string>("default.gawk-path", "gawk");
    trans_path     = getValue<std::string>("default.trans-path", "trans");
    trans_awk_path = getValue<std::string>("default.trans-awk-path", "trans.awk");
    use_trans_gawk = getValue<bool>("default.use-trans-gawk", false);
}

static bool is_str_digital(const std::string& str)
{
    for (size_t i = 0; i < str.size(); ++i)
        if (!(str[i] >= '0' && str[i] <= '9'))
            return false;

    return true;
}

void Config::overrideOption(const std::string& opt)
{
    const size_t pos = opt.find('=');
    if (pos == std::string::npos)
        die(_("alias color '{}' does NOT have an equal sign '=' for separating color name and value\n"
              "For more check with --help"),
            opt);

    std::string        name{ opt.substr(0, pos) };
    const std::string& value = opt.substr(pos + 1);

    // usually the user finds incovinient to write "config.foo"
    // for general config options
    if (name.find('.') == name.npos)
        name.insert(0, "config.");

    if (value == "true")
        overrides[name] = { .value_type = BOOL, .bool_value = true };
    else if (value == "false")
        overrides[name] = { .value_type = BOOL, .bool_value = false };
    else if ((value[0] == '"' && value.back() == '"') || (value[0] == '\'' && value.back() == '\''))
        overrides[name] = { .value_type = STR, .string_value = value.substr(1, value.size() - 2) };
    else if (is_str_digital(value))
        overrides[name] = { .value_type = INT, .int_value = std::stoi(value) };
    else
        die(_("looks like override value '{}' from '{}' is neither a bool, int or string value"), value, name);
}

void Config::generateConfig(const std::string& filename)
{
    if (std::filesystem::exists(filename))
    {
        if (!askUserYorN(false, "WARNING: config file '{}' already exists. Do you want to overwrite it?", filename))
            std::exit(1);
    }

    auto f = fmt::output_file(filename.data());
    f.print("{}", AUTOCONFIG);
}
