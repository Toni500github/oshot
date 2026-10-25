#include "config.hpp"

#include <cstdlib>
#include <string>

#include "catch2/catch_amalgamated.hpp"
#include "common.hpp"
#include "util.hpp"

// What LoadConfigFile() makes of what GenerateConfig() wrote. The extension gets
// uppercased, and a relative theme file ends up under the config folder
static Config::config_file_t as_loaded(Config::config_file_t file, const fs::path& dir)
{
    file.image_out_type.first = str_toupper(file.image_out_type.first);
    if (fs::path(file.theme_file_path).is_relative())
        file.theme_file_path = dir.string() + DIR_SEP_STR + file.theme_file_path;
    return file;
}

// Each option on its own, so a failure names it, then the whole struct, which
// also catches an option added to config_file_t but missing in one of the
// places that writes or reads it (see CLAUDE.md, "Config")
static void check_same(const Config::config_file_t& got, const Config::config_file_t& want)
{
    CHECK(got.ocr_path == want.ocr_path);
    CHECK(got.ocr_model == want.ocr_model);
    CHECK(got.ocr_get_repo == want.ocr_get_repo);
    CHECK(got.delay == want.delay);
    CHECK(got.color_picker == want.color_picker);
    CHECK(got.cpa_mode == want.cpa_mode);
    CHECK(got.real_full_screen == want.real_full_screen);
    CHECK(got.enable_vsync == want.enable_vsync);
    CHECK(got.glx_vendor == want.glx_vendor);
    CHECK(got.allow_out_edit == want.allow_out_edit);
    CHECK(got.show_text_tools == want.show_text_tools);
    CHECK(got.pref_conf_to_env == want.pref_conf_to_env);
    CHECK(got.render_anns == want.render_anns);
    CHECK(got.old_toolbar_pos == want.old_toolbar_pos);
    CHECK(got.ctrl_c_copy_img == want.ctrl_c_copy_img);
    CHECK(got.hotkey == want.hotkey);
    CHECK(got.language == want.language);
    CHECK(got.fonts == want.fonts);
    CHECK(got.builtin_font == want.builtin_font);
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

        Config::config_file_t want = as_loaded(written.File, dir);
        if (const char* env = std::getenv("TESSDATA_PREFIX"))
            want.ocr_path = env;  // the default config-over-env = false lets it win
        check_same(loaded.File, want);
    }

    SECTION("Every option changed")
    {
        Config written(file, dir);

        Config::config_file_t& f = written.File;
        // Backslashes, quotes and non-ASCII, which EscapeString() has to escape
        f.ocr_path           = R"(C:\Users\Тест\"oshot"\models)";
        f.ocr_model          = "ita+eng";
        f.ocr_get_repo       = "tesseract-ocr/tessdata_best\twith\ncontrol\x01chars";
        f.delay              = 250;
        f.color_picker       = 1;
        f.cpa_mode           = 0;
        f.real_full_screen   = !f.real_full_screen;
        f.enable_vsync       = !f.enable_vsync;
        f.glx_vendor         = "mesa";
        f.allow_out_edit     = !f.allow_out_edit;
        f.show_text_tools    = !f.show_text_tools;
        f.pref_conf_to_env   = true;  // else TESSDATA_PREFIX would replace ocr_path
        f.render_anns        = !f.render_anns;
        f.old_toolbar_pos    = !f.old_toolbar_pos;
        f.ctrl_c_copy_img    = !f.ctrl_c_copy_img;
        f.hotkey             = "Ctrl+Alt+P";
        f.language           = "pt_BR";
        f.fonts              = { "Noto Sans", R"(A "quoted" \ font)" };
        f.builtin_font       = !f.builtin_font;
        f.image_out_type     = { "jpeg", ImageExt::JPEG };
        f.image_out_fmt      = "shot_{:%F}";
        f.image_out_size_fmt = "MiB";
        f.theme_file_path    = "mytheme.toml";
        written.GenerateConfig(file.string(), true);

        Config loaded(file, dir);
        loaded.LoadConfigFile(file.string());
        check_same(loaded.File, as_loaded(written.File, dir));
    }

    SECTION("An absolute theme file is kept as it is")
    {
        Config written(file, dir);
        written.File.theme_file_path = (dir / "themes" / "dark.toml").string();
        written.GenerateConfig(file.string(), true);

        Config loaded(file, dir);
        loaded.LoadConfigFile(file.string());
        CHECK(loaded.File.theme_file_path == written.File.theme_file_path);
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
