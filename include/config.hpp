#ifndef _CONFIG_HPP_
#define _CONFIG_HPP_

#include <memory>
#include <type_traits>
#include <unordered_map>

#include "fmt/format.h"

// util.hpp
std::string expand_var(std::string ret);
bool        hexstr_to_col(const std::string_view hex, uint32_t& out);

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
        std::string              ocr_path         = "./models";
        std::string              ocr_model        = "eng";
        std::string              theme_style      = "auto";
        std::string              theme_file_path  = "theme.toml";
        std::string              image_out_fmt    = "oshot_{:%F_%H-%M}";
        int                      delay            = 0;
        bool                     allow_out_edit   = false;
        bool                     real_full_screen = false;
        bool                     show_text_tools  = true;
        bool                     enable_vsync     = true;
        bool                     render_anns      = true;
        bool                     ctrl_c_copy_img  = true;
        std::vector<std::string> fonts;

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

    struct theme_overrides_t
    {
        std::unordered_map<std::string, std::string> colors;  // ImGuiCol name -> "#RRGGBB[AA]"

        float window_rounding = -1.f;
        float frame_rounding  = -1.f;
        float grab_rounding   = -1.f;
        float tab_rounding    = -1.f;
        float window_border   = -1.f;
        float frame_border    = -1.f;

        bool operator==(const theme_overrides_t&) const = default;
    } theme_overrides;

    /**
     * Load config file and parse every config variables
     * @param filename The config file path
     */
    void LoadConfigFile(const std::string& filename);

    /**
     * Parse the theme file (aka "theme.toml")
     *  @param filename The directory of the theme file
     */
    void LoadThemeFile(const std::string& filename);

    /**
     * Generate a config file
     * @param filename The config file path
     * @param force Overwrite without asking
     */
    void GenerateConfig(const std::string& filename, const bool force = false);

    /**
     * Generate a theme file
     * @param filename The theme file path
     * @param force Overwrite without asking
     */
    void GenerateTheme(const std::string& filename, const bool force = false);

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

    /**
     * Get value of config variables
     * @param value The config variable "path" (e.g "config.source-path")
     * @param fallback Default value if couldn't retrive value
     */
    template <typename T>
    T GetValue(const std::string_view value,
               const T&               fallback,
               bool                   dont_expand_var = false,
               bool                   is_theme        = false) const
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

        const std::optional<T>& ret =
            is_theme ? m_theme_tbl.at_path(value).value<T>() : m_tbl.at_path(value).value<T>();
        if constexpr (toml::is_string<T>)
            if (!dont_expand_var)
                return ret ? expand_var(ret.value()) : expand_var(fallback);
            else
                return ret ? ret.value() : fallback;
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

    /**
     * Get the theme color variable and return a rgba type value
     * @param value The value we want
     * @param fallback The default value if it doesn't exists
     * @return rgba type variable
     */
    template <typename T>
    T GetThemeStyleValue(const std::string_view value, const T& fallback, bool dont_expand_var = true) const
    {
        return GetValue<T>(fmt::format("theme.style.{}", value), fallback, dont_expand_var, true);
    }

    /**
     * Get the theme color variable and return a rgba type value
     * @param value The value we want
     * @param fallback The default value if it doesn't exists
     * @return rgba type variable
     */
    uint32_t GetThemeColorValue(const std::string_view value,
                                const std::string&     fallback,
                                bool                   dont_expand_var = true) const
    {
        uint32_t out;
        hexstr_to_col(GetValue<std::string>(fmt::format("theme.colors.{}", value), fallback, dont_expand_var, true),
                      out);
        return out;
    }

    const std::string& GetConfigPath() const { return m_config_path; }
    const std::string& GetThemePath() const { return m_theme_path; }
    const std::string& GetConfigDirPath() const { return m_config_dir_path; }

private:
    // Parsed config from LoadConfigFile()
    toml::table m_tbl;

    // Parsed theme from LoadThemeFile()
    toml::table m_theme_tbl;

    std::unordered_map<std::string, override_config_value_t> m_overrides;

    std::string m_config_path;
    std::string m_theme_path;
    std::string m_config_dir_path;
};

extern std::unique_ptr<Config> g_config;

void apply_imgui_theme();

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

# Copy image shortcut to use.
# true: CTRL+C
# false: CTRL+SHIFT+C
ctrl-c-copy-img = {}

# Fonts to use for the application. Can be an absolute path, or just a name.
# You can combine multiple fonts for multiple language support.
# for example, using "Roboto-Regular.ttf" and "RobotoCJK-Regular.ttc" for Chinese, Japanese, and Korean support alongside English support.
# If empty, or non-existent (or commented out), oshot will use the default font for ImGUI.
fonts = [{}]

# Format of the output image filename when saving.
# The .png extension is appended automatically.
# Uses {{fmt}} chrono specifiers — the colon inside {{}} is required: {{:%F}} correct, {{%F}} will error.
#
# Default: "oshot_{{:%F_%H-%M}}"
image-out-fmt = "{}"

# Base UI theme: "auto" (follow OS dark/light), "dark", "light", or "classic".
# Fine-grained overrides live in theme.toml.
theme = "{}"

# Path to a theme file. Absolute or relative to this config's directory.
# Delete or comment out to use only the base theme above.
theme-file = "{}"
)#";

inline constexpr std::string_view AUTOTHEME = (R"(
# Drop this next to config.toml or point theme-file at its path.
# All sections and keys are optional — omit anything you don't want to override.

# ---------------------------------------------------------------
# Rounding (pixels, 0 = sharp corners, max ~12)
# ---------------------------------------------------------------
[theme.style]
window-rounding = 8.0
frame-rounding  = 4.0
grab-rounding   = 4.0
tab-rounding    = 4.0

# Border width in pixels. 0 = none, 1 = thin line.
window-border = 1.0
frame-border  = 0.0

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
# --- Text ---
Text         = "#cdd6f4FF"
TextDisabled = "#6c7086FF"

# --- Backgrounds ---
WindowBg       = "#1e1e2eFF"
ChildBg        = "#181825FF"
PopupBg        = "#1e1e2eFF"
FrameBg        = "#313244FF"
FrameBgHovered = "#45475aFF"
FrameBgActive  = "#585b70FF"
MenuBarBg      = "#181825FF"

# --- Title bar ---
TitleBg       = "#181825FF"
TitleBgActive = "#313244FF"

# --- Borders ---
Border       = "#585b70FF"
BorderShadow = "#00000000"

# --- Scrollbar ---
ScrollbarBg          = "#181825FF"
ScrollbarGrab        = "#585b70FF"
ScrollbarGrabHovered = "#6c7086FF"
ScrollbarGrabActive  = "#7f849cFF"

# --- Buttons ---
Button        = "#313244FF"
ButtonHovered = "#45475aFF"
ButtonActive  = "#585b70FF"

# --- Headers (selectables, tree nodes, collapsing headers) ---
Header        = "#313244FF"
HeaderHovered = "#45475aFF"
HeaderActive  = "#585b70FF"

# --- Sliders / checkmarks ---
CheckMark        = "#cba6f7FF"
SliderGrab       = "#cba6f7FF"
SliderGrabActive = "#b4befeff"

# --- Tabs ---
Tab         = "#313244FF"
TabHovered  = "#cba6f7FF"
TabSelected = "#45475aFF"

# --- Misc ---
Separator         = "#585b70FF"
ResizeGrip        = "#cba6f7FF"
ResizeGripHovered = "#cba6f7FF"
ResizeGripActive  = "#cba6f7FF"
)");

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
