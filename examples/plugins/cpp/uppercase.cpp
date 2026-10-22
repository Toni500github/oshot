/* Example oshot plugin in C++: uppercases the OCR output. It's the C example
 * (../c/uppercase.c) written the C++ way: a class for the plugin's state,
 * RAII for the strings oshot returns, and static noexcept functions as the
 * C callbacks.
 *
 * - render():             a button that uppercases the text, and the selection's size
 * - on_ocr_done():        optionally uppercases new text right away
 * - render_preferences(): a setting for that, plus a prefix, saved in the plugin's config.toml
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "oshot/cimgui.h"
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

struct Prefs
{
    bool                 auto_uppercase = false; /* uppercase as soon as OCR is done */
    std::array<char, 64> prefix{};               /* put before the uppercased text; a buffer for igInputText */

    bool operator==(const Prefs& other) const
    {
        return auto_uppercase == other.auto_uppercase && std::strcmp(prefix.data(), other.prefix.data()) == 0;
    }
};

class UppercasePlugin
{
public:
    UppercasePlugin(const oshot_host_t* host, oshot_ctx_t* ctx) : m_host(host), m_ctx(ctx)
    {
        m_saved.auto_uppercase = host->get_bool(ctx, OSHOT_STORE_CONFIG, "auto-uppercase", false);

        const HostString prefix(host, host->get_string(ctx, OSHOT_STORE_CONFIG, "prefix", oshot_str_borrow("")));
        const std::string_view sv = prefix.view().substr(0, m_saved.prefix.size() - 1);
        std::copy(sv.begin(), sv.end(), m_saved.prefix.begin());
        m_editing = m_saved;

        log(OSHOT_LOG_INFO, "uppercase plugin (C++) loaded");
    }

    void render()
    {
        if (igButton("Uppercase the OCR output", ImVec2_c{ 0, 0 }))
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
            igTextDisabled("Selection: %dx%d", m_cap_w, m_cap_h);
    }

    void on_ocr_done(const oshot_ocr_result_t& result)
    {
        if (m_saved.auto_uppercase)
            uppercase_text(std::string_view(result.text.p, result.text.len));
    }

    bool render_preferences()
    {
        igCheckbox("Uppercase as soon as text is extracted", &m_editing.auto_uppercase);
        igInputText("Prefix", m_editing.prefix.data(), m_editing.prefix.size(), ImGuiInputTextFlags_None, nullptr,
                    nullptr);

        return !(m_editing == m_saved);
    }

    void on_save_preferences()
    {
        m_saved = m_editing;
        m_host->set_bool(m_ctx, OSHOT_STORE_CONFIG, "auto-uppercase", m_saved.auto_uppercase);
        m_host->set_string(m_ctx, OSHOT_STORE_CONFIG, "prefix", oshot_str_borrow(m_saved.prefix.data()));
    }

    void on_discard_preferences() { m_editing = m_saved; }

private:
    const oshot_host_t* m_host;
    oshot_ctx_t*        m_ctx;

    Prefs m_saved;   /* what's in config.toml */
    Prefs m_editing; /* what the Preferences window shows, until saved */

    int32_t m_cap_w = 0, m_cap_h = 0; /* size of the selection when "Uppercase" was last pressed */

    void log(oshot_log_level_t level, const char* msg) { m_host->log(m_ctx, level, oshot_str_borrow(msg)); }

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
        std::string out = m_saved.prefix.data();
        out.reserve(out.size() + text.size());
        std::transform(text.begin(), text.end(), std::back_inserter(out),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        m_host->set_text(m_ctx, OSHOT_TEXT_OCR, oshot_str_t{ out.data(), out.size() });
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

bool plugin_render_preferences(void* state) noexcept
{
    return self(state)->render_preferences();
}

void plugin_on_save_preferences(void* state) noexcept
{
    self(state)->on_save_preferences();
}

void plugin_on_discard_preferences(void* state) noexcept
{
    self(state)->on_discard_preferences();
}

constexpr oshot_plugin_t descriptor = {
    .struct_size            = sizeof(oshot_plugin_t),
    .abi_version            = OSHOT_API_VERSION,
    .imgui_version          = OSHOT_IMGUI_VERSION,
    .id                     = "io.github.toni500github.uppercase-cpp",
    .name                   = "Uppercase (C++)",
    .init                   = plugin_init,
    .destroy                = plugin_destroy,
    .render                 = plugin_render,
    .on_ocr_done            = plugin_on_ocr_done,
    .render_preferences     = plugin_render_preferences,
    .on_save_preferences    = plugin_on_save_preferences,
    .on_discard_preferences = plugin_on_discard_preferences,
};

}  // namespace

/* oshot_plugin.h declares it extern "C" */
OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &descriptor;
}
