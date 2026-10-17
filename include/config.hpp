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
#include <optional>
#include <unordered_map>

#include "screen_capture.hpp"
#include "toml_api.hpp"
#include "util.hpp"

class Config : public TomlAPI
{
public:
    /**
     * Create the config directory and files if missing, then load the config file
     * @param configFile The config file, from --config or the default one
     * @param configDir The config directory
     **/
    Config(const std::filesystem::path& configFile, const std::filesystem::path& configDir);

    // Options of the [default] table in the config file.
    // CLI arguments and --override can overwrite them
    struct config_file_t
    {
        // image-out-size-ind: unit of the estimated size under the selection, "off" or "auto"
        std::string image_out_size_fmt = "auto";

        // 0 = "Bar - Square"; 1 = "Wheel - Triangle"
        int color_picker = 0;

        // color-picker-alpha-mode, a ColorPickerAlpha
        int cpa_mode = 2;

        // allow-text-edit: the OCR/barcode output can be edited
        bool allow_out_edit = false;

        // Exclusive fullscreen, for compositors that make the overlay look grainy
        bool real_full_screen = false;

        // annotations-in-text-tools: scan the selection with its annotations
        bool render_anns = true;

        // config-over-env: the config wins over environment variables like TESSDATA_PREFIX
        bool pref_conf_to_env = false;

        // legacy-toolbar-pos: all the tools on one line instead of around the selection
        bool old_toolbar_pos = false;

        // Global capture hotkey, registered by the tray instance (Windows/macOS only).
        // Win+Shift+S and Cmd+Shift+3/4/5 are taken by the OS screenshot tools.
#if OSHOT_WINDOWS
        std::string hotkey = "Alt+Shift+S";
#elif OSHOT_MACOS
        std::string hotkey = "Cmd+Shift+2";
#else
        std::string hotkey;
#endif

        // image-out-ext as written in the config, and parsed
        std::pair<std::string, ImageExt> image_out_type = { "png", ImageExt::PNG };

        std::string              ocr_path        = get_default_ocr_path();
        std::string              ocr_get_repo    = "tesseract-ocr/tessdata";
        std::string              ocr_model       = "eng";
        std::string              theme_file_path = "theme.toml";
        std::string              image_out_fmt   = "oshot_{:%F_%H-%M}";
        int                      delay           = 0;
        bool                     show_text_tools = true;
        bool                     enable_vsync    = true;
        bool                     ctrl_c_copy_img = true;
        std::vector<std::string> fonts;

        bool operator==(const config_file_t&) const = default;
    } File;

    // Set only from CLI arguments or the overlay, never saved
    struct runtime_settings_t
    {
        // --instant-save=DIR: save there without asking
        std::string instant_save_dir;

        // --selection: region of the screenshot (or -f image) to use, skips the overlay
        std::optional<region_t> selection;

        // --instant-copy/--instant-save: copy or save without showing the overlay
        SavingOp instant_copy_save = SavingOp::kNone;

        // Tesseract page segmentation mode to force, 0 to pick one from the image
        int preferred_psm = 0;

        // Draw the selection's resize handles, toggled with Ctrl+G
        bool enable_handles = true;

        std::string source_file;
        bool        only_launch_tray = false;
        bool        only_launch_gui  = false;
        bool        extract_text     = false;
        bool        extract_barcode  = false;

        bool operator==(const runtime_settings_t&) const = default;
    } Runtime;

    // The theme file's settings. The rounding and border sizes are -1 when not set,
    // which keeps the default style's value
    struct theme_overrides_t
    {
        // ImGuiCol name -> "#RRGGBB[AA]"
        std::unordered_map<std::string, std::string> colors;

        // Not an ImGui style setting, oshot's own
        bool smooth_animations = false;

        float window_rounding = -1.f;
        float frame_rounding  = -1.f;
        float grab_rounding   = -1.f;
        float tab_rounding    = -1.f;
        float window_border   = -1.f;
        float frame_border    = -1.f;

        bool operator==(const theme_overrides_t&) const = default;
    } theme_overrides;

    using TomlAPI::GetValue;
    using TomlAPI::SetValue;

    /**
     * Load the config file and parse every option into File
     * @param filename The config file path
     **/
    void LoadConfigFile(const std::string& filename);

    /**
     * Copy the OCR models shipped with the app into models_dir, skipping existing ones
     * @param models_dir The writable models directory
     **/
    void SeedBundledModels(const std::filesystem::path& models_dir);

    /**
     * Parse the theme file into theme_overrides
     * @param filename The theme file path, absolute or relative to the config directory
     **/
    void LoadThemeFile(const std::string& filename);

    /**
     * Write the config file with the current File values
     * @param filename The config file path
     * @param force Overwrite without asking
     **/
    void GenerateConfig(const std::string& filename, const bool force = false);

    /**
     * Write the theme file
     * @param filename The theme file path, relative to the config directory
     * @param force Overwrite without asking, and write the current theme_overrides instead of the default theme
     **/
    void GenerateTheme(const std::string& filename, const bool force = false);

    [[nodiscard]] const std::string& GetConfigPath() const { return m_config_path; }
    [[nodiscard]] const std::string& GetConfigDirPath() const { return m_config_dir_path; }

private:
    // Parsed theme from LoadThemeFile()
    TomlAPI m_theme;

    std::string m_config_path;
    std::string m_config_dir_path;
};

extern std::unique_ptr<Config> g_config;

#endif  // !_CONFIG_HPP_
