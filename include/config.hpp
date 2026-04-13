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
        std::string              ocr_path  = "./models";
        std::string              ocr_model = "eng";
        std::vector<std::string> fonts;
        int                      delay            = 0;
        bool                     allow_out_edit   = false;
        bool                     real_full_screen = false;
        bool                     show_text_tools  = true;
        bool                     enable_vsync     = true;
        bool                     render_anns      = true;

        // C++20: Automatic generation of ==, !=, <, <=, >, >=
        auto operator<=>(const config_file_t&) const = default;
    } File;

    // Only from CLI arguments
    // Or ImGUI window
    struct runtime_settings_t
    {
        std::string source_file;
        int         preferred_psm    = 0;
        bool        enable_handles   = true;
        bool        only_launch_tray = false;
        bool        only_launch_gui  = false;
#if DEBUG || (defined(_WIN32) && WINDOWS_CMD)
        bool debug_print = true;
#else
        bool debug_print = false;
#endif
        auto operator<=>(const runtime_settings_t&) const = default;
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
     * @param force Overwrite without asking
     */
    void GenerateConfig(const std::string& filename, const bool force = false);

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

    const std::string& GetConfigPath() const { return m_config_path; }
    const std::string& GetConfigDirPath() const { return m_config_dir_path; }

private:
    // Parsed config from loadConfigFile()
    toml::table m_tbl;

    std::unordered_map<std::string, override_config_value_t> m_overrides;

    std::string m_config_path;
    std::string m_config_dir_path;

    /**
     * Get value of config variables
     * @param value The config variable "path" (e.g "config.source-path")
     * @param fallback Default value if couldn't retrive value
     */
    template <typename T>
    T GetValue(const std::string_view value, const T& fallback, bool dont_expand_var = false) const
    {
        const auto& overridePos = m_overrides.find(value.data());

        if (overridePos != m_overrides.end())
        {
            const auto& ov = overridePos->second;
            if constexpr (std::is_same<T, bool>())
                if (ov.value_type == ValueType::kBool)
                    return ov.bool_value;
            if constexpr (std::is_same<T, std::string>())
                if (ov.value_type == ValueType::kString)
                    return ov.string_value;
            if constexpr (std::is_same<T, int>())
                if (ov.value_type == ValueType::kInt)
                    return ov.int_value;
        }

        const std::optional<T>& ret = this->m_tbl.at_path(value).value<T>();
        if constexpr (toml::is_string<T>)
            return ret ? expand_var(ret.value(), dont_expand_var) : expand_var(fallback, dont_expand_var);
        else
            return ret.value_or(fallback);
    }

    std::vector<std::string> GetValueArrayStr(const std::string_view          value,
                                              const std::vector<std::string>& fallback) const
    {
        std::vector<std::string> ret;

        // https://stackoverflow.com/a/78266628
        if (const toml::array* array_it = m_tbl.at_path(value).as_array())
        {
            ret.reserve(array_it->size());
            array_it->for_each([&](auto&& el) {
                if (const toml::value<std::string>* str_elem = el.as_string())
                    ret.push_back((*str_elem)->data());
            });

            return ret;
        }
        else
        {
            return fallback;
        }
    }
};

extern std::unique_ptr<Config> g_config;

// default config
inline constexpr std::string_view AUTOCONFIG = R"#([default]
# Default Path to where we'll use all the '.traineddata' models.
#ocr-path = "/usr/share/tessdata/"
ocr-path = "{}"

# Default OCR model.
ocr-model = "{}"

# Delay the app before acquiring a screenshot (in milliseconds)
# Doesn't affect if opening external image (i.e. -f flag)
delay = {}

# On some desktop environments (e.g. MATE), the compositor may cause
# the capture window to look grainy or pixelated. Enabling this uses exclusive
# fullscreen mode which bypasses the compositor and fixes it.
# Downside: the window may briefly take over the display on some setups.
real-full-screen = {}

# Controls vertical sync (VSync). When enabled, the capture window renders in sync
# with your monitor's refresh rate, thus being smoother visually but uses slightly more CPU/GPU.
# Disable if the overlay feels sluggish or unresponsive.
vsync = {}

# Allow the extracted output to be editable.
allow-text-edit = {}

# Display the text tools (OCR, Bar/QR code scan) by default.
show-text-tools = {}

# Consider annotations when scanning (true)
# or only when saving the selection (false).
annotations-in-text-tools = {}

# Fonts to use for the application. Can be an absolute path, or just a name.
# You can combine multiple fonts for multiple language support.
# for example, using "Roboto-Regular.ttf" and "RobotoCJK-Regular.ttc" for Chinese, Japanese, and Korean support alongside English support.
# If empty, or non-existent (or commented out), oshot will use the default font for ImGUI.
fonts = [{}]
)#";

inline constexpr std::string_view oshot_help = (R"(Usage: oshot [OPTIONS]...
Lightweight Screenshot tool to extract text on the fly.

GENERAL OPTIONS:
    -h, --help                  Print this help menu.
    -V, --version               Print version and other infos about the build.
    -f, --source <PATH>         Path to the image to use as background (use '-' for reading from stdin).
    -C, --config <PATH>         Path to the config file to use (default: ~/.config/oshot/config.toml).
    -O, --override <OPTION>     Override a config option (e.g "delay=200", "default.ocr-model='jpn'").
    -d, --delay <MILLIS>        Delay the app before acquiring the screenshot by milliseconds.
                                Won't affect if using the -f flag

    -g, --gui                   Only launch the GUI.
    -t, --tray                  Only launch system tray.
    --debug                     Print debug statments.
    --gen-config [<PATH>]       Generate default config file. If PATH is omitted, saves to default location.
                                Prompts before overwriting.
)");

#endif  // !_CONFIG_HPP_
