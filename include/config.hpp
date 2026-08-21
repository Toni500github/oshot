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

#ifndef _CONFIG_HPP_
#define _CONFIG_HPP_

#include <filesystem>
#include <memory>
#include <unordered_map>

#include "fmt/format.h"
#include "toml_api.hpp"
#include "util.hpp"

class Config : public TomlAPI
{
public:
    // Create .config directories and files and load the config file (args or default)
    Config(const std::filesystem::path& configFile, const std::filesystem::path& configDir);

    // Variables of config file in [default] table.
    // They can be overwritten from CLI arguments
    struct config_file_t
    {
        // Since we package the eng.traineddata file on Windows/MacOS,
        // because the user may not know how to download one or doesn't want to,
        // just for out-of-box experience sake, let's use the relative
        // ./models directory for the OCR models.
#ifdef __linux__
        std::string ocr_path = "/usr/share/tessdata/";
#else
        std::string ocr_path = "./models";
#endif
        std::string ocr_get_repo       = "tesseract-ocr/tessdata";
        std::string ocr_model          = "eng";
        std::string theme_file_path    = "theme.toml";
        std::string image_out_fmt      = "oshot_{:%F_%H-%M}";
        std::string image_out_size_fmt = "auto";
        int         delay              = 0;
        int         color_picker       = 0;  // 0 = "Bar - Square"; 1 = "Wheel - Triangle";
        int         cpa_mode           = 2;  // color_picker_alpha_mode
        bool        allow_out_edit     = false;
        bool        real_full_screen   = false;
        bool        show_text_tools    = true;
        bool        enable_vsync       = true;
        bool        render_anns        = true;
        bool        pref_conf_to_env   = false;
        bool        ctrl_c_copy_img    = true;

        std::pair<std::string, ImageExt> image_out_type = { "png", ImageExt::PNG };
        std::vector<std::string>         fonts;

        bool operator==(const config_file_t&) const = default;
    } File;

    // Only from CLI arguments
    // Or ImGUI window
    struct runtime_settings_t
    {
        std::string source_file;
        int         preferred_psm     = 0;
        bool        enable_handles    = true;
        bool        only_launch_tray  = false;
        bool        only_launch_gui   = false;
        SavingOp    instant_copy_save = SavingOp::kNone;

        bool operator==(const runtime_settings_t&) const = default;
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

        // custom oshot specific theme options
        bool smooth_animations = false;

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

    using TomlAPI::GetValue;
    using TomlAPI::SetValue;

    template <typename T>
    T GetThemeValue(const std::string_view value, const T& fallback, bool dont_expand_var = true) const
    {
        return m_theme.GetValue<T>(fmt::format("theme.{}", value), fallback, dont_expand_var);
    }

    template <typename T>
    T GetThemeStyleValue(const std::string_view value, const T& fallback, bool dont_expand_var = true) const
    {
        return m_theme.GetValue<T>(fmt::format("theme.style.{}", value), fallback, dont_expand_var);
    }

    uint32_t GetThemeColorValue(const std::string_view value,
                                const std::string&     fallback,
                                bool                   dont_expand_var = true) const
    {
        uint32_t out;
        hexstr_to_col(m_theme.GetValue<std::string>(fmt::format("theme.colors.{}", value), fallback, dont_expand_var),
                      out);
        return out;
    }

    const std::string& GetConfigPath() const { return m_config_path; }
    const std::string& GetThemePath() const { return m_theme_path; }
    const std::string& GetConfigDirPath() const { return m_config_dir_path; }

private:
    // Parsed theme from LoadThemeFile()
    TomlAPI m_theme;

    std::string m_config_path;
    std::string m_theme_path;
    std::string m_config_dir_path;
};

extern std::unique_ptr<Config> g_config;

void apply_imgui_theme();
#endif  // !_CONFIG_HPP_
