#ifndef _CONFIG_HPP_
#define _CONFIG_HPP_

#include <memory>
#include <type_traits>
#include <unordered_map>

// util.hpp
std::string expand_var(std::string ret, bool dont = false);

#define TOML_HEADER_ONLY 0
#include "toml++/toml.hpp"

enum class ValueType
{
    kNone,
    kString,
    kBool,
    kInt
};

struct override_config_value_t
{
    ValueType   value_type   = ValueType::kNone;
    std::string string_value = "";
    bool        bool_value   = false;
    int         int_value    = 0;
};

class Config
{
public:
    // Create .config directories and files and load the config file (args or default)
    Config(const std::string& configFile, const std::string& configDir);

    // Variables of config file in [default] table.
    // They can be overwritten from CLI arguments
    struct config_file_t
    {
        std::string ocr_path;
        std::string ocr_model;
        std::string lang_from;
        std::string lang_to;
        std::string font;
        int         delay          = -1;
        bool        allow_ocr_edit = false;

        std::unordered_map<std::string, std::string> lang_fonts_paths;
    } File;

    // Only from CLI arguments
    // Or ImGUI window
    struct runtime_settings_t
    {
        std::string source_file;
        int         preferred_psm    = 0;
        bool        enable_handles   = true;
        bool        only_launch_tray = false;
#if DEBUG
        bool debug_print = true;
#else
        bool debug_print = false;
#endif
    } Runtime;

    /**
     * Load config file and parse every config variables
     * @param filename The config file path
     * @param colors The colors struct where we'll put the default config colors.
     *               It doesn't include the colors in config.alias-colors
     */
    void LoadConfigFile(const std::string& filename);

    /**
     * Generate a config file
     * @param filename The config file path
     */
    void GenerateConfig(const std::string& filename);

    /**
     * Override a config value from --override
     * @param str The value to override.
     *            Must have a '=' for separating the name and value to override.
     *            NO spaces between
     */
    void OverrideOption(const std::string& opt);

    /**
     * Override a config value from --override
     * @param key The value name to override.
     *            Must have a '=' for separating the name and value to override.
     *            NO spaces between
     * @param value The value that will overwrite
     */
    template <typename T>
    void OverrideOption(const std::string& key, const T& value)
    {
        override_config_value_t o;
        if constexpr (std::is_same_v<T, bool>)
        {
            o.value_type = ValueType::kBool;
            o.bool_value = value;
        }
        else if constexpr (std::is_convertible_v<T, std::string>)
        {
            o.value_type   = ValueType::kString;
            o.string_value = value;
        }
        else if constexpr (std::is_convertible_v<T, int>)
        {
            o.value_type = ValueType::kInt;
            o.int_value  = value;
        }

        m_overrides[key] = std::move(o);
    }

private:
    // Parsed config from loadConfigFile()
    toml::table m_tbl;

    std::unordered_map<std::string, override_config_value_t> m_overrides;

    /**
     * Get value of config variables
     * @param value The config variable "path" (e.g "config.source-path")
     * @param fallback Default value if couldn't retrive value
     */
    template <typename T>
    T GetValue(const std::string_view value, const T& fallback, bool dont_expand_var = false) const
    {
        const auto& overridePos = m_overrides.find(value.data());

        // user wants a bool (overridable), we found an override matching the name, and the override is a bool.
        if constexpr (std::is_same<T, bool>())
            if (overridePos != m_overrides.end() && m_overrides.at(value.data()).value_type == ValueType::kBool)
                return m_overrides.at(value.data()).bool_value;

        // user wants a str (overridable), we found an override matching the name, and the override is a str.
        if constexpr (std::is_same<T, std::string>())
            if (overridePos != m_overrides.end() && m_overrides.at(value.data()).value_type == ValueType::kString)
                return m_overrides.at(value.data()).string_value;

        if constexpr (std::is_same<T, int>())
            if (overridePos != m_overrides.end() && m_overrides.at(value.data()).value_type == ValueType::kInt)
                return m_overrides.at(value.data()).int_value;

        const std::optional<T>& ret = this->m_tbl.at_path(value).value<T>();
        if constexpr (toml::is_string<T>)
            return ret ? expand_var(ret.value(), dont_expand_var) : expand_var(fallback, dont_expand_var);
        else
            return ret.value_or(fallback);
    }
};

extern std::unique_ptr<Config> g_config;

// default config
inline constexpr std::string_view AUTOCONFIG = R"#([default]
# Default Path to where we'll use all the '.traineddata' models.
#ocr-path = "/usr/share/tessdata/"
ocr-path = "./models"

# Default OCR model.
ocr-model = "eng"

# Delay the app before acquiring a screenshot (in milliseconds)
# Doesn't affect if opening external image (i.e. -f flag)
delay = 200

# Default from language codename translate
lang-from = "auto"

# Default to language codename translate
lang-to = "en-us"

# Allow the extracted output to be editable
allow-edit = false

# Default font (absolute path or just name) for the whole application.
# Leave/Make it empty, or commment it, to use ImGUI default font.
font = "arial.ttf"

# These sections are dedicated for being able to display languages with their appropriated fonts.
# based on the language code, you can write a table (e.g [lang.en-us]) and
# put a variable called "font" which can be an absolute path or just the name of the font.
#[lang.en-us]
#font = "DejaVuSans.ttf" # Or C:\\Windows\\Fonts\\DejaVuSans.ttf or ~/.fonts/DejaVuSans.ttf
)#";

inline constexpr std::string_view oshot_help = (R"(Usage: oshot [OPTIONS]...
Lightweight Screenshot tool to extract and translate text on the fly.

GENERAL OPTIONS:
    -h, --help                  Print this help menu.
    -V, --version               Print version and other infos about the build.
    -f, --source <PATH>         Path to the image to use as background (use '-' for reading from stdin)
    -C, --config <PATH>         Path to the config file to use (default: ~/.config/oshot/config.toml).
    -d, --delay <MILLIS>        Delay the app before acquiring the screenshot by milliseconds.
                                Won't affect if using the -f flag

    -l, --list                  List all available translatable languages along side their codenames.
    -t, --tray                  Launch system tray
    --debug                     Print debug statments
    --gen-config [<PATH>]       Generate default config file. If PATH is omitted, saves to default location.
                                Prompts before overwriting.
)");

#endif  // !_CONFIG_HPP_
