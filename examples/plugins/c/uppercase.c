/* Example oshot plugin: uppercases the OCR output.
 *
 * - render():      a button that uppercases the text, and the selection's size
 * - on_ocr_done(): optionally uppercases new text right away
 * - settings:      that option and a prefix, which oshot draws in Preferences > Plugins
 *                  and saves in the plugin's config.toml
 *
 * It draws only with the host's ui_* functions, so it works with any Dear ImGui
 * version oshot is built with (imgui_version = 0). ../cimgui is an example that
 * draws with cimgui instead.
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oshot/oshot_plugin.h"

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;

    /* The settings, as last saved */
    bool        auto_uppercase; /* uppercase as soon as OCR is done */
    oshot_str_t prefix;         /* put before the uppercased text; a host string */

    int32_t cap_w, cap_h; /* size of the selection when "Uppercase" was last pressed */
} state_t;

/* oshot draws these in Preferences > Plugins and saves them in config.toml */
static const oshot_setting_t settings[] = {
    {
        .kind     = OSHOT_SETTING_BOOL,
        .key      = "auto-uppercase",
        .label    = "Uppercase as soon as text is extracted",
        .fallback = { .kind = OSHOT_VAL_BOOL, .b = false },
    },
    {
        .kind     = OSHOT_SETTING_STRING,
        .key      = "prefix",
        .label    = "Prefix",
        .help     = "Put before the uppercased text",
        .fallback = { .kind = OSHOT_VAL_STRING, .s = { "", 0 } },
    },
};

static void log_msg(state_t* s, oshot_log_level_t level, const char* msg)
{
    s->host->log(s->ctx, level, oshot_str_borrow(msg));
}

/* Reads the settings with the same keys and fallbacks as `settings` */
static void read_settings(state_t* s)
{
    s->auto_uppercase = s->host->get_bool(s->ctx, OSHOT_STORE_CONFIG, "auto-uppercase", false);
    s->host->str_free(&s->prefix);
    s->prefix = s->host->get_string(s->ctx, OSHOT_STORE_CONFIG, "prefix", oshot_str_borrow(""));
}

/* Replaces oshot's OCR output with prefix + UPPERCASE(text) */
static void uppercase_text(state_t* s, oshot_str_t text)
{
    const size_t prefix_len = s->prefix.len;
    char*        out        = malloc(prefix_len + text.len + 1);
    if (!out)
        return;

    if (prefix_len)
        memcpy(out, s->prefix.p, prefix_len);
    for (size_t i = 0; i < text.len; ++i)
        out[prefix_len + i] = (char)toupper((unsigned char)text.p[i]);
    out[prefix_len + text.len] = '\0';

    oshot_str_t result = { out, prefix_len + text.len };
    s->host->set_text(s->ctx, OSHOT_TEXT_OCR, result);
    free(out);
}

static bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    /* The ui_* functions came after the first plugin API: check the last one */
    if (!OSHOT_HOST_HAS(host, ui_end_section))
    {
        host->log(ctx, OSHOT_LOG_ERROR, oshot_str_borrow("this oshot is too old for this plugin: update it"));
        return false;
    }

    state_t* s = calloc(1, sizeof(*s));
    if (!s)
        return false;
    s->host = host;
    s->ctx  = ctx;
    read_settings(s);

    log_msg(s, OSHOT_LOG_INFO, "uppercase plugin loaded");
    *state = s;
    return true;
}

static void plugin_destroy(void* state)
{
    state_t* s = state;
    s->host->str_free(&s->prefix);
    free(s);
}

static void plugin_render(void* state)
{
    state_t*            s    = state;
    const oshot_host_t* host = s->host;

    if (host->ui_button(s->ctx, oshot_str_borrow("Uppercase the OCR output")))
    {
        oshot_str_t text;
        if (host->get_text(s->ctx, OSHOT_TEXT_OCR, &text))
        {
            uppercase_text(s, text);
            host->str_free(&text);
        }

        /* get_capture() copies the whole image: only on demand, never every frame */
        oshot_capture_t cap = host->get_capture(s->ctx);
        s->cap_w            = cap.w;
        s->cap_h            = cap.h;
        host->capture_free(&cap);
    }

    if (s->cap_w > 0)
    {
        char line[64];
        snprintf(line, sizeof(line), "Selection: %dx%d", s->cap_w, s->cap_h);
        host->ui_text(s->ctx, oshot_str_borrow(line), OSHOT_UI_DISABLED);
    }
}

static void plugin_on_ocr_done(void* state, const oshot_ocr_result_t* result)
{
    state_t* s = state;
    if (s->auto_uppercase && result->text.len > 0) /* empty when nothing was read */
        uppercase_text(s, result->text);
}

static void plugin_on_settings_saved(void* state)
{
    read_settings(state);
}

static const oshot_plugin_t descriptor = {
    .struct_size       = sizeof(oshot_plugin_t),
    .abi_version       = OSHOT_API_VERSION,
    .imgui_version     = 0, /* no cimgui */
    .id                = "io.github.toni500github.uppercase",
    .name              = "Uppercase",
    .init              = plugin_init,
    .destroy           = plugin_destroy,
    .render            = plugin_render,
    .on_ocr_done       = plugin_on_ocr_done,
    .settings          = settings,
    .n_settings        = sizeof(settings) / sizeof(settings[0]),
    .on_settings_saved = plugin_on_settings_saved,
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &descriptor;
}
