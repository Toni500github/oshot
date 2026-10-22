/* Example oshot plugin in C++: uppercases the OCR output. It's the C example
 * (../c/uppercase.c) written the C++ way: a class for the plugin's state,
 * RAII for the strings oshot returns, and static noexcept functions as the
 * C callbacks.
 *
 * - render():      a button that uppercases the text, and the selection's size
 * - on_ocr_done(): optionally uppercases new text right away
 * - settings:      that option and a prefix, which oshot draws in Preferences > Plugins
 *                  and saves in the plugin's config.toml
 *
 * It draws only with the host's ui_* functions (imgui_version = 0).
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "oshot/oshot_plugin.h"

namespace
{

/* A string oshot returned: freed with host->str_free when it goes out of scope */
class HostString
{
public:
    HostString(const oshot_host_t* host, oshot_str_t str) : m_host(host), m_str(str) {}
    ~HostString() { m_host->str_free(&m_str); }

    HostString(const HostString&)            = delete;
    HostString& operator=(const HostString&) = delete;

    std::string_view view() const { return m_str.p ? std::string_view(m_str.p, m_str.len) : std::string_view(); }

private:
    const oshot_host_t* m_host;
    oshot_str_t         m_str;
};

oshot_str_t borrow(std::string_view s)
{
    return { s.data(), s.size() };
}

/* oshot draws these in Preferences > Plugins and saves them in config.toml */
constexpr std::array settings = {
    oshot_setting_t{
        .kind     = OSHOT_SETTING_BOOL,
        .key      = "auto-uppercase",
        .label    = "Uppercase as soon as text is extracted",
        .fallback = { .kind = OSHOT_VAL_BOOL, .b = false },
    },
    oshot_setting_t{
        .kind     = OSHOT_SETTING_STRING,
        .key      = "prefix",
        .label    = "Prefix",
        .help     = "Put before the uppercased text",
        .fallback = { .kind = OSHOT_VAL_STRING, .s = { "", 0 } },
    },
};

class UppercasePlugin
{
public:
    UppercasePlugin(const oshot_host_t* host, oshot_ctx_t* ctx) : m_host(host), m_ctx(ctx)
    {
        read_settings();
        log(OSHOT_LOG_INFO, "uppercase plugin (C++) loaded");
    }

    /* Reads the settings with the same keys and fallbacks as `settings` */
    void read_settings()
    {
        m_auto_uppercase = m_host->get_bool(m_ctx, OSHOT_STORE_CONFIG, "auto-uppercase", false);
        m_prefix = HostString(m_host, m_host->get_string(m_ctx, OSHOT_STORE_CONFIG, "prefix", borrow(""))).view();
    }

    void render()
    {
        if (m_host->ui_button(m_ctx, borrow("Uppercase the OCR output")))
        {
            if (const std::optional<std::string> text = get_text(OSHOT_TEXT_OCR))
                uppercase_text(*text);

            /* get_capture() copies the whole image: only on demand, never every frame */
            oshot_capture_t cap = m_host->get_capture(m_ctx);
            m_cap_w             = cap.w;
            m_cap_h             = cap.h;
            m_host->capture_free(&cap);
        }

        if (m_cap_w > 0)
        {
            const std::string line = "Selection: " + std::to_string(m_cap_w) + "x" + std::to_string(m_cap_h);
            m_host->ui_text(m_ctx, borrow(line), OSHOT_UI_DISABLED);
        }
    }

    void on_ocr_done(const oshot_ocr_result_t& result)
    {
        if (m_auto_uppercase)
            uppercase_text(std::string_view(result.text.p, result.text.len));
    }

private:
    const oshot_host_t* m_host;
    oshot_ctx_t*        m_ctx;

    /* The settings, as last saved */
    bool        m_auto_uppercase = false; /* uppercase as soon as OCR is done */
    std::string m_prefix;                 /* put before the uppercased text */

    int32_t m_cap_w = 0, m_cap_h = 0; /* size of the selection when "Uppercase" was last pressed */

    void log(oshot_log_level_t level, std::string_view msg) { m_host->log(m_ctx, level, borrow(msg)); }

    std::optional<std::string> get_text(const char* id)
    {
        oshot_str_t text;
        if (!m_host->get_text(m_ctx, id, &text))
            return std::nullopt;
        return std::string(HostString(m_host, text).view());
    }

    /* Replaces oshot's OCR output with prefix + UPPERCASE(text) */
    void uppercase_text(std::string_view text)
    {
        std::string out = m_prefix;
        out.reserve(out.size() + text.size());
        std::transform(text.begin(), text.end(), std::back_inserter(out),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        m_host->set_text(m_ctx, OSHOT_TEXT_OCR, borrow(out));
    }
};

/* The C callbacks. Exceptions must not cross into oshot: these are noexcept,
 * so one escaping terminates oshot right here instead of corrupting it. */

UppercasePlugin* self(void* state)
{
    return static_cast<UppercasePlugin*>(state);
}

bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state) noexcept
{
    /* The ui_* functions came after the first plugin API: check the last one */
    if (!OSHOT_HOST_HAS(host, ui_end_section))
    {
        host->log(ctx, OSHOT_LOG_ERROR, borrow("this oshot is too old for this plugin: update it"));
        return false;
    }

    auto* plugin = new (std::nothrow) UppercasePlugin(host, ctx);
    *state       = plugin;
    return plugin != nullptr;
}

void plugin_destroy(void* state) noexcept
{
    delete self(state);
}

void plugin_render(void* state) noexcept
{
    self(state)->render();
}

void plugin_on_ocr_done(void* state, const oshot_ocr_result_t* result) noexcept
{
    self(state)->on_ocr_done(*result);
}

void plugin_on_settings_saved(void* state) noexcept
{
    self(state)->read_settings();
}

constexpr oshot_plugin_t descriptor = {
    .struct_size       = sizeof(oshot_plugin_t),
    .abi_version       = OSHOT_API_VERSION,
    .imgui_version     = 0, /* no cimgui */
    .id                = "io.github.toni500github.uppercase-cpp",
    .name              = "Uppercase (C++)",
    .init              = plugin_init,
    .destroy           = plugin_destroy,
    .render            = plugin_render,
    .on_ocr_done       = plugin_on_ocr_done,
    .settings          = settings.data(),
    .n_settings        = settings.size(),
    .on_settings_saved = plugin_on_settings_saved,
};

}  // namespace

/* oshot_plugin.h declares it extern "C" */
OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &descriptor;
}
