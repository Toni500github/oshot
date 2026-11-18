#include "config.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "catch2/catch_amalgamated.hpp"
#include "common.hpp"
#include "theme.hpp"
#include "util/image.hpp"
#include "util/paths.hpp"
#include "util/strings.hpp"

// What LoadConfigFile() makes of what GenerateConfig() wrote. The extension gets
// uppercased, and a relative theme file ends up under the config folder
static Config::config_file_t as_loaded(Config::config_file_t file)
{
    file.image_out_type.first = str_toupper(file.image_out_type.first);
    return file;
}

// Each option on its own, so a failure names it, then the whole struct, which
// also catches an option added to config_file_t but missing from LoadConfigFile(),
// the AUTOCONFIG template in texts.hpp or its print call in GenerateConfig()
static void check_same(const Config::config_file_t& got, const Config::config_file_t& want)
{
    CHECK(got.ocr_path == want.ocr_path);
    CHECK(got.ocr_model == want.ocr_model);
    CHECK(got.ocr_get_repo == want.ocr_get_repo);
    CHECK(got.delay == want.delay);
    CHECK(got.color_picker == want.color_picker);
    CHECK(got.cpa_mode == want.cpa_mode);
    CHECK(got.fullscreen == want.fullscreen);
    CHECK(got.enable_vsync == want.enable_vsync);
    CHECK(got.glx_vendor == want.glx_vendor);
    CHECK(got.allow_out_edit == want.allow_out_edit);
    CHECK(got.show_text_tools == want.show_text_tools);
    CHECK(got.allow_multi_tray == want.allow_multi_tray);
    CHECK(got.allow_multi_overlay == want.allow_multi_overlay);
    CHECK(got.pref_conf_to_env == want.pref_conf_to_env);
    CHECK(got.render_anns == want.render_anns);
    CHECK(got.old_toolbar_pos == want.old_toolbar_pos);
    CHECK(got.aa_icons == want.aa_icons);
    CHECK(got.ctrl_c_copy_img == want.ctrl_c_copy_img);
    CHECK(got.hotkey == want.hotkey);
    CHECK(got.language == want.language);
    CHECK(got.fonts == want.fonts);
    CHECK(got.builtin_font == want.builtin_font);
    CHECK(got.font_size == want.font_size);
    CHECK(got.image_out_type.first == want.image_out_type.first);
    CHECK(got.image_out_type.second == want.image_out_type.second);
    CHECK(got.image_out_fmt == want.image_out_fmt);
    CHECK(got.image_out_size_fmt == want.image_out_size_fmt);
    CHECK(got.theme_file_path == want.theme_file_path);
    CHECK(got == want);
}

TEST_CASE("Config: the generated config loads back the same", "[config]")
{
    const fs::path dir  = test_temp_dir() / "config-roundtrip";
    const fs::path file = dir / "config.toml";
    fs::remove_all(dir);

    SECTION("The defaults")
    {
        // The constructor writes the defaults, as on first start
        const Config written(file, dir);

        Config loaded(file, dir);
        loaded.LoadConfigFile(file.string());

        Config::config_file_t want = as_loaded(written.File);
        if (const char* env = std::getenv("TESSDATA_PREFIX"))
            want.ocr_path = env;  // the default config-over-env = false lets it win
        check_same(loaded.File, want);
    }

    SECTION("Every option changed")
    {
        Config written(file, dir);

        Config::config_file_t& f = written.File;
        // Backslashes, quotes and non-ASCII, which EscapeString() has to escape
        f.ocr_path             = R"(C:\Users\Тест\"oshot"\models)";
        f.ocr_model            = "ita+eng";
        f.ocr_get_repo         = "tesseract-ocr/tessdata_best\twith\ncontrol\x01chars";
        f.delay                = 250;
        f.color_picker         = 1;
        f.cpa_mode             = 0;
        f.fullscreen           = !f.fullscreen;
        f.enable_vsync         = !f.enable_vsync;
        f.glx_vendor           = "mesa";
        f.allow_out_edit       = !f.allow_out_edit;
        f.allow_multi_tray     = !f.allow_multi_tray;
        f.allow_multi_overlay  = !f.allow_multi_overlay;
        f.show_text_tools      = !f.show_text_tools;
        f.pref_conf_to_env     = true;  // else TESSDATA_PREFIX would replace ocr_path
        f.render_anns          = !f.render_anns;
        f.old_toolbar_pos      = !f.old_toolbar_pos;
        f.aa_icons             = !f.aa_icons;
        f.ctrl_c_copy_img      = !f.ctrl_c_copy_img;
        f.hotkey               = "Ctrl+Alt+P";
        f.language             = "pt_BR";
        f.fonts                = { "Noto Sans", R"(A "quoted" \ font)" };
        f.builtin_font         = !f.builtin_font;
        f.font_size            = 20;
        f.image_out_type       = { "jpeg", ImageExt::JPEG };
        f.image_out_fmt        = "shot_{:%F}";
        f.image_out_size_fmt   = "MiB";
        f.theme_file_path      = "mytheme.toml";
        written.GenerateConfig(file.string(), true);

        Config loaded(file, dir);
        loaded.LoadConfigFile(file.string());
        check_same(loaded.File, as_loaded(written.File));
    }

    SECTION("An absolute theme file is kept as it is")
    {
        Config written(file, dir);
        written.File.theme_file_path = (dir / "themes" / "dark.toml").string();
        written.GenerateConfig(file.string(), true);

        Config loaded(file, dir);
        loaded.LoadConfigFile(file.string());
        CHECK(loaded.File.theme_file_path == written.File.theme_file_path);
        CHECK(loaded.GetThemeFilePath() == written.File.theme_file_path);
    }

    SECTION("A relative theme file stays relative, and resolves against the config folder")
    {
        Config written(file, dir);
        written.File.theme_file_path = "mytheme.toml";
        written.GenerateConfig(file.string(), true);

        Config loaded(file, dir);
        loaded.LoadConfigFile(file.string());
        CHECK(loaded.File.theme_file_path == "mytheme.toml");
        CHECK(fs::path(loaded.GetThemeFilePath()) == dir / "mytheme.toml");
    }
}

TEST_CASE("Config: invalid values fall back to the defaults", "[config]")
{
    const fs::path dir  = test_temp_dir() / "config-invalid";
    const fs::path file = dir / "config.toml";
    fs::remove_all(dir);

    write_file(file, R"([default]
image-out-ext = "webp"
image-out-size-ind = "furlongs"
)");

    Config loaded(file, dir);
    loaded.LoadConfigFile(file.string());
    CHECK(loaded.File.image_out_type.first == "PNG");
    CHECK(loaded.File.image_out_type.second == ImageExt::PNG);
    CHECK(loaded.File.image_out_size_fmt == "auto");
}

TEST_CASE("Config: the saved theme loads back the same", "[config][theme]")
{
    const fs::path dir  = test_temp_dir() / "theme-roundtrip";
    const fs::path file = dir / "config.toml";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Config                     written(file, dir);
    Config::theme_overrides_t& ov = written.theme_overrides;
    ov.base                       = ThemeBase::Light;
    ov.smooth_animations          = false;
    ov.window_rounding            = 6.5f;
    ov.frame_border               = 1.0f;
    ov.includes                   = { "~/.cache/wal/oshot.toml", R"(C:\Users\"q"\p.toml)" };
    ov.palette["accent"]          = "#5274f0ff";
    ov.colors["Text"]             = "#112233ff";
    ov.colors["Button"]           = "$accent/50";
    ov.oshot_colors["warning"]    = "$accent";
    written.GenerateTheme("theme.toml", true);

    Config loaded(file, dir);
    loaded.LoadThemeFile("theme.toml");
    CHECK(loaded.theme_overrides.base == ThemeBase::Light);
    CHECK(loaded.theme_overrides.window_rounding == 6.5f);
    CHECK(loaded.theme_overrides.frame_rounding == -1.f);
    CHECK(loaded.theme_overrides.includes == ov.includes);
    CHECK(loaded.theme_overrides == written.theme_overrides);
}

TEST_CASE("Config: the default theme file uses the dark base without overrides", "[config][theme]")
{
    const fs::path dir  = test_temp_dir() / "theme-default";
    const fs::path file = dir / "config.toml";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Config written(file, dir);
    written.GenerateTheme((dir / "theme.toml").string());

    Config loaded(file, dir);
    loaded.LoadThemeFile("theme.toml");
    CHECK(loaded.theme_overrides.base == ThemeBase::Dark);
    CHECK(loaded.theme_overrides.includes.empty());
    CHECK(loaded.theme_overrides.palette.empty());
    CHECK(loaded.theme_overrides.colors.empty());
    CHECK(loaded.theme_overrides.oshot_colors.empty());
}

TEST_CASE("Config: a light theme drops the colors older files copied from the dark one", "[config][theme]")
{
    const fs::path dir  = test_temp_dir() / "theme-old-copies";
    const fs::path file = dir / "config.toml";
    fs::remove_all(dir);

    write_file(dir / "theme.toml", R"([theme]
base = "light"
[theme.colors]
Text = "#DCDDE1FF"
WindowBg = "#0d1015ff"
Button = "#FF0000FF"
)");

    Config cfg(file, dir);
    cfg.LoadThemeFile("theme.toml");
    CHECK(cfg.theme_overrides.colors.size() == 1);
    CHECK(cfg.theme_overrides.colors.at("Button") == "#FF0000FF");

    SECTION("but keeps them with the dark base")
    {
        write_file(dir / "theme.toml", "[theme.colors]\nText = \"#DCDDE1FF\"\n");
        cfg.LoadThemeFile("theme.toml");
        CHECK(cfg.theme_overrides.colors.size() == 1);
    }
}

// A resolved color as 0xRRGGBBAA, 0 for none
static uint32_t packed(const std::optional<rgba_t>& col)
{
    return col ? col->to_rgba() : 0;
}

TEST_CASE("Theme: includes, palette references and oshot colors", "[config][theme]")
{
    const fs::path dir  = test_temp_dir() / "theme-include";
    const fs::path file = dir / "config.toml";
    fs::remove_all(dir);

    write_file(dir / "colors.toml", R"([theme.palette]
bg = "#101010"
accent = "#ff0000"
[theme.colors]
WindowBg = "$bg"
Button = "$accent"
[theme.oshot-colors]
error = "$accent"
)");
    write_file(dir / "theme.toml", R"([theme]
include = "colors.toml"
[theme.palette]
accent = "#00ff00"
[theme.colors]
Button = "$accent/50"
NotAColor = "#ffffff"
Text = "$missing"
)");

    Config cfg(file, dir);
    cfg.LoadThemeFile("theme.toml");
    REQUIRE(cfg.theme_included.files.size() == 1);
    CHECK(cfg.theme_included.files[0].second);
    CHECK_FALSE(cfg.theme_overrides.colors.contains("NotAColor"));

    SECTION("The theme file's palette wins over the include's, everywhere it's used")
    {
        const theme::resolved_t button = theme::resolve_imgui_color("Button", cfg);
        CHECK(button.source == theme::Source::File);
        CHECK(packed(button.value) == 0x00FF0080u);

        const theme::resolved_t error = theme::resolve_app_color(AppCol::Error, cfg);
        CHECK(error.source == theme::Source::Include);
        CHECK(packed(error.value) == 0x00FF00FFu);
    }

    SECTION("Include colors apply, and a broken value falls through to the next layer")
    {
        CHECK(packed(theme::resolve_imgui_color("WindowBg", cfg).value) == 0x101010FFu);
        const theme::resolved_t text = theme::resolve_imgui_color("Text", cfg);
        CHECK(text.source == theme::Source::Base);
        CHECK_FALSE(text.value.has_value());
        CHECK(theme::resolve_app_color(AppCol::Warning, cfg).source == theme::Source::Base);
    }

    SECTION("Color values")
    {
        CHECK(packed(theme::parse_color("#abcdef", cfg)) == 0xABCDEFFFu);
        CHECK(packed(theme::parse_color("$bg/0", cfg)) == 0x10101000u);
        CHECK_FALSE(theme::parse_color("$bg/101", cfg));
        CHECK_FALSE(theme::parse_color("$bg/x", cfg));
        CHECK_FALSE(theme::parse_color("$nope", cfg));
        CHECK_FALSE(theme::parse_color("red", cfg));
    }

    SECTION("Changing, adding or removing a file is noticed")
    {
        CHECK_FALSE(cfg.ThemeFilesChanged());
        fs::last_write_time(dir / "colors.toml", fs::last_write_time(dir / "colors.toml") + std::chrono::seconds(5));
        CHECK(cfg.ThemeFilesChanged());
        cfg.LoadThemeFile("theme.toml");
        CHECK_FALSE(cfg.ThemeFilesChanged());
        fs::remove(dir / "colors.toml");
        CHECK(cfg.ThemeFilesChanged());
        cfg.LoadThemeFile("theme.toml");
        CHECK_FALSE(cfg.theme_included.files[0].second);
        write_file(dir / "colors.toml", "");
        CHECK(cfg.ThemeFilesChanged());
    }

    SECTION("A reload keeps the theme when the file stops parsing")
    {
        write_file(dir / "theme.toml", "[theme\n");
        fs::last_write_time(dir / "theme.toml", fs::last_write_time(dir / "theme.toml") + std::chrono::seconds(5));
        cfg.LoadThemeFile("theme.toml", true);
        CHECK(cfg.theme_overrides.palette.at("accent") == "#00ff00");
        CHECK_FALSE(cfg.ThemeFilesChanged());
    }
}
