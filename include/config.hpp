
#ifndef _CONFIG_HPP
#define _CONFIG_HPP

#include <memory>

#include "util.hpp"
#define TOML_HEADER_ONLY 0

#include <cstdint>
#include <type_traits>
#include <unordered_map>

#include "toml++/toml.hpp"

enum types
{
    TYPE_STR,
    TYPE_BOOL,
    TYPE_INT
};

struct override_configs_types
{
    types       value_type;
    std::string string_value = "";
    bool        bool_value   = false;
    int         int_value    = 0;
};

class Config
{
public:
    // Create .config directories and files and load the config file (args or default)
    Config(const std::string& configFile, const std::string& configDir);

    // Variables of config file in [default] table
    std::string ocr_path;
    std::string ocr_model;
    // std::string lang_from;
    // std::string lang_to;
    std::string gawk_path;
    std::string trans_path;
    std::string trans_awk_path;
    bool        use_trans_gawk;

    std::unordered_map<std::string, override_configs_types> overrides;

    /**
     * Load config file and parse every config variables
     * @param filename The config file path
     * @param colors The colors struct where we'll put the default config colors.
     *               It doesn't include the colors in config.alias-colors
     */
    void loadConfigFile(const std::string& filename);

    /**
     * Generate a config file
     * @param filename The config file path
     */
    void generateConfig(const std::string& filename);

    /**
     * Override a config value from --override
     * @param str The value to override.
     *            Must have a '=' for separating the name and value to override.
     *            NO spaces between
     */
    void overrideOption(const std::string& opt);

private:
    // Parsed config from loadConfigFile()
    toml::table m_tbl;

    /**
     * Get value of config variables
     * @param value The config variable "path" (e.g "config.source-path")
     * @param fallback Default value if couldn't retrive value
     */
    template <typename T>
    T getValue(const std::string_view value, const T&& fallback, bool dont_expand_var = false) const
    {
        const auto& overridePos = overrides.find(value.data());

        // user wants a bool (overridable), we found an override matching the name, and the override is a bool.
        if constexpr (std::is_same<T, bool>())
            if (overridePos != overrides.end() && overrides.at(value.data()).value_type == TYPE_BOOL)
                return overrides.at(value.data()).bool_value;

        // user wants a str (overridable), we found an override matching the name, and the override is a str.
        if constexpr (std::is_same<T, std::string>())
            if (overridePos != overrides.end() && overrides.at(value.data()).value_type == TYPE_STR)
                return overrides.at(value.data()).string_value;

        if constexpr (std::is_same<T, std::uint16_t>())
            if (overridePos != overrides.end() && overrides.at(value.data()).value_type == TYPE_INT)
                return overrides.at(value.data()).int_value;

        const std::optional<T> ret = this->m_tbl.at_path(value).value<T>();
        if constexpr (toml::is_string<T>)  // if we want to get a value that's a string
            return ret ? expandVar(ret.value(), dont_expand_var) : expandVar(fallback, dont_expand_var);
        else
            return ret.value_or(fallback);
    }
};

extern std::unique_ptr<Config> config;

// default config
inline constexpr std::string_view AUTOCONFIG = R"#([default]
# Default Path to where we'll use all the '.traineddata' tesseract models.
ocr-path = "/usr/share/tessdata/"

# Default OCR model.
ocr-model = "eng"

# Default from language translate
lang-from = "auto"

# Default to language translate
lang-to = "en-us"

# Path or executable to the gawk binary
gawk-path = "gawk"

# Path or executable to the trans shell script
trans-path = "trans"

# If to use the "trans.awk" file or the bash shell script one
use-trans-gawk = false

# Path to the "trans.awk" translation file
trans-awk-path = "trans.awk"
)#";

inline constexpr std::string_view oshot_help = (R"(Usage: oshot [OPTIONS]...
Screenshot tool to extract and translate text on the fly.

NOTE: Boolean flags [<BOOL>] accept: "true", 1, "enable", or empty. Any other value is treated as false.

GENERAL OPTIONS:
    -h, --help                  Print this help menu.
    -V, --version               Print version and other infos about the build.
    -C, --config <PATH>         Path to the config file to use (default: ~/.config/oshot/config.toml).

    --gen-config [<PATH>]       Generate default config file. If PATH is omitted, saves to default location.
                                Prompts before overwriting.
)");
#endif  // _CONFIG_HPP
