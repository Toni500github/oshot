/* Example oshot plugin that draws with cimgui: a histogram of the line lengths
 * in the OCR output, which the host's ui_* functions can't draw.
 *
 * A plugin that calls cimgui sets imgui_version = OSHOT_IMGUI_VERSION, and then
 * only loads in an oshot built with that exact Dear ImGui. oshot changes ImGui
 * only in minor releases (x.y.0), so such a plugin needs a rebuild for each
 * one. Prefer the ui_* functions (see ../c) for anything they can draw; they
 * can be mixed with cimgui calls in the same render().
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <stdlib.h>

#include "oshot/cimgui.h"
#include "oshot/oshot_plugin.h"

#define MAX_LINES 256

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;

    float lengths[MAX_LINES]; /* characters in each line of the last OCR output */
    int   n_lines;
    float longest;
} state_t;

static bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
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
    *state  = s;
    return true;
}

static void plugin_destroy(void* state)
{
    free(state);
}

static void plugin_on_ocr_done(void* state, const oshot_ocr_result_t* result)
{
    state_t* s = state;
    s->n_lines = 0;
    s->longest = 0;

    size_t len = 0;
    for (size_t i = 0; i <= result->text.len && s->n_lines < MAX_LINES; ++i)
    {
        if (i < result->text.len && result->text.p[i] != '\n')
        {
            /* UTF-8: count characters, not continuation bytes */
            if (((unsigned char)result->text.p[i] & 0xC0) != 0x80)
                ++len;
            continue;
        }
        s->lengths[s->n_lines++] = (float)len;
        if ((float)len > s->longest)
            s->longest = (float)len;
        len = 0;
    }
}

static void plugin_render(void* state)
{
    state_t* s = state;

    if (s->n_lines == 0)
    {
        /* ui_* functions and cimgui mix freely */
        s->host->ui_text(s->ctx, oshot_str_borrow("Extract some text to see its line lengths"), OSHOT_UI_DISABLED);
        return;
    }

    igPlotHistogram_FloatPtr("##lengths",
                             s->lengths,
                             s->n_lines,
                             0,
                             "Characters per line",
                             0.0f,
                             s->longest,
                             (ImVec2_c){ 0, 80 },
                             sizeof(float));
}

static const oshot_plugin_t descriptor = {
    .struct_size   = sizeof(oshot_plugin_t),
    .abi_version   = OSHOT_API_VERSION,
    .imgui_version = OSHOT_IMGUI_VERSION, /* calls cimgui */
    .id            = "io.github.toni500github.line-lengths",
    .name          = "Line lengths (cimgui)",
    .init          = plugin_init,
    .destroy       = plugin_destroy,
    .render        = plugin_render,
    .on_ocr_done   = plugin_on_ocr_done,
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &descriptor;
}
