/* Example oshot plugin: uppercases the OCR output.
 *
 * - render():             a button that uppercases the text, and the selection's size
 * - on_ocr_done():        optionally uppercases new text right away
 * - render_preferences(): a setting for that, plus a prefix, saved in the plugin's config.toml
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oshot/cimgui.h"
#include "oshot/oshot_plugin.h"

typedef struct
{
    bool auto_uppercase; /* uppercase as soon as OCR is done */
    char prefix[64];     /* put before the uppercased text */
} prefs_t;

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;

    prefs_t saved;   /* what's in config.toml */
    prefs_t editing; /* what the Preferences window shows, until saved */

    int32_t cap_w, cap_h; /* size of the selection when "Uppercase" was last pressed */
} state_t;

static void log_info(state_t* s, const char* msg)
{
    s->host->log(s->ctx, OSHOT_LOG_INFO, oshot_str_borrow(msg));
}

/* Replaces oshot's OCR output with prefix + UPPERCASE(text) */
static void uppercase_text(state_t* s, oshot_str_t text)
{
    const size_t prefix_len = strlen(s->saved.prefix);
    char*        out        = malloc(prefix_len + text.len + 1);
    if (!out)
        return;

    memcpy(out, s->saved.prefix, prefix_len);
    for (size_t i = 0; i < text.len; ++i)
        out[prefix_len + i] = (char)toupper((unsigned char)text.p[i]);
    out[prefix_len + text.len] = '\0';

    oshot_str_t result = { out, prefix_len + text.len };
    s->host->set_text(s->ctx, OSHOT_TEXT_OCR, result);
    free(out);
}

static bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    state_t* s = calloc(1, sizeof(*s));
    if (!s)
        return false;
    s->host = host;
    s->ctx  = ctx;

    s->saved.auto_uppercase = host->get_bool(ctx, OSHOT_STORE_CONFIG, "auto-uppercase", false);
    oshot_str_t prefix      = host->get_string(ctx, OSHOT_STORE_CONFIG, "prefix", oshot_str_borrow(""));
    snprintf(s->saved.prefix, sizeof(s->saved.prefix), "%s", prefix.p ? prefix.p : ""); /* host strings end in '\0' */
    host->str_free(&prefix);
    s->editing = s->saved;

    log_info(s, "uppercase plugin loaded");
    *state = s;
    return true;
}

static void plugin_destroy(void* state)
{
    free(state);
}

static void plugin_render(void* state)
{
    state_t* s = state;

    if (igButton("Uppercase the OCR output", (ImVec2_c){ 0, 0 }))
    {
        oshot_str_t text;
        if (s->host->get_text(s->ctx, OSHOT_TEXT_OCR, &text))
        {
            uppercase_text(s, text);
            s->host->str_free(&text);
        }

        /* get_capture() copies the whole image: only on demand, never every frame */
        oshot_capture_t cap = s->host->get_capture(s->ctx);
        s->cap_w            = cap.w;
        s->cap_h            = cap.h;
        s->host->capture_free(&cap);
    }

    if (s->cap_w > 0)
        igTextDisabled("Selection: %dx%d", s->cap_w, s->cap_h);
}

static void plugin_on_ocr_done(void* state, const oshot_ocr_result_t* result)
{
    state_t* s = state;
    if (s->saved.auto_uppercase)
        uppercase_text(s, result->text);
}

static bool plugin_render_preferences(void* state)
{
    state_t* s = state;

    igCheckbox("Uppercase as soon as text is extracted", &s->editing.auto_uppercase);
    igInputText("Prefix", s->editing.prefix, sizeof(s->editing.prefix), ImGuiInputTextFlags_None, NULL, NULL);

    return s->editing.auto_uppercase != s->saved.auto_uppercase || strcmp(s->editing.prefix, s->saved.prefix) != 0;
}

static void plugin_on_save_preferences(void* state)
{
    state_t* s = state;
    s->saved   = s->editing;
    s->host->set_bool(s->ctx, OSHOT_STORE_CONFIG, "auto-uppercase", s->saved.auto_uppercase);
    s->host->set_string(s->ctx, OSHOT_STORE_CONFIG, "prefix", oshot_str_borrow(s->saved.prefix));
}

static void plugin_on_discard_preferences(void* state)
{
    state_t* s = state;
    s->editing = s->saved;
}

static const oshot_plugin_t descriptor = {
    .struct_size            = sizeof(oshot_plugin_t),
    .abi_version            = OSHOT_API_VERSION,
    .imgui_version          = OSHOT_IMGUI_VERSION,
    .id                     = "io.github.toni500github.uppercase",
    .name                   = "Uppercase",
    .init                   = plugin_init,
    .destroy                = plugin_destroy,
    .render                 = plugin_render,
    .on_ocr_done            = plugin_on_ocr_done,
    .render_preferences     = plugin_render_preferences,
    .on_save_preferences    = plugin_on_save_preferences,
    .on_discard_preferences = plugin_on_discard_preferences,
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &descriptor;
}
