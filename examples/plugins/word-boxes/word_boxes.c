/* Example oshot plugin: draws the OCR word boxes on the overlay.
 *
 * - on_ocr_done():          keeps the words' boxes, moved by the result's origin
 *                           into the pixels of the whole screenshot, so they stay
 *                           on the text when the selection moves afterwards
 * - render_overlay():       draws each box with draw_rect(), colored by how sure
 *                           the OCR engine was (green, yellow, red), and with the
 *                           "Show confidence" setting its percentage with draw_text().
 *                           Positions are in screenshot pixels, and oshot converts
 *                           them for a scaled (HiDPI) screen
 * - get_selection(), set_selection(): "Fit the selection to the text" in the text
 *                           tools' Plugins menu puts the selection around the words,
 *                           with a margin; the previous one is logged
 * - on_capture_end():       the boxes belong to that capture
 *
 * It draws only with the host's draw functions, so imgui_version is 0.
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oshot/oshot_plugin.h"

typedef struct
{
    float   x, y, w, h; /* screenshot pixels */
    int32_t confidence;
} box_t;

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;
    bool                show_confidence;

    box_t* boxes;
    size_t n;
} state_t;

static const oshot_setting_t settings[] = {
    {
        .kind     = OSHOT_SETTING_BOOL,
        .key      = "show-confidence",
        .label    = "Show confidence",
        .help     = "Write each word's confidence above its box",
        .fallback = { .kind = OSHOT_VAL_BOOL, .b = false },
    },
};

static void log_msg(state_t* s, oshot_log_level_t level, const char* msg)
{
    s->host->log(s->ctx, level, oshot_str_borrow(msg));
}

static void clear_boxes(state_t* s)
{
    free(s->boxes);
    s->boxes = NULL;
    s->n     = 0;
}

static void fit_selection(void* user, int32_t action)
{
    (void)action;
    state_t* s = user;
    if (s->n == 0)
    {
        log_msg(s, OSHOT_LOG_INFO, "No words to fit the selection to, extract the text first");
        return;
    }

    float x0 = s->boxes[0].x, y0 = s->boxes[0].y;
    float x1 = x0 + s->boxes[0].w, y1 = y0 + s->boxes[0].h;
    for (size_t i = 1; i < s->n; ++i)
    {
        const box_t* b = &s->boxes[i];
        x0             = b->x < x0 ? b->x : x0;
        y0             = b->y < y0 ? b->y : y0;
        x1             = b->x + b->w > x1 ? b->x + b->w : x1;
        y1             = b->y + b->h > y1 ? b->y + b->h : y1;
    }

    float       x = 0, y = 0, w = 0, h = 0;
    oshot_str_t err = { NULL, 0 };
    if (s->host->get_selection(s->ctx, &x, &y, &w, &h, NULL) == OSHOT_OK)
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "The selection was %gx%g+%g+%g", w, h, x, y);
        log_msg(s, OSHOT_LOG_INFO, msg);
    }

    const float margin = 6;
    if (s->host->set_selection(s->ctx, x0 - margin, y0 - margin, x1 - x0 + 2 * margin, y1 - y0 + 2 * margin, &err) !=
        OSHOT_OK)
    {
        /* e.g. UNSUPPORTED while the person drags the selection */
        s->host->log(s->ctx, OSHOT_LOG_WARN, err);
        s->host->str_free(&err);
    }
}

static bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    if (!OSHOT_HOST_HAS(host, draw_text))
    {
        host->log(ctx, OSHOT_LOG_ERROR, oshot_str_borrow("this oshot is too old to draw on the overlay"));
        return false;
    }

    state_t* s = calloc(1, sizeof(*s));
    if (!s)
        return false;
    s->host            = host;
    s->ctx             = ctx;
    s->show_confidence = host->get_bool(ctx, OSHOT_STORE_CONFIG, "show-confidence", false);
    *state             = s;

    host->register_action(ctx,
                          OSHOT_PLACE_TEXT_TOOLS,
                          oshot_str_borrow("Fit the selection to the text"),
                          NULL,
                          fit_selection,
                          s,
                          NULL,
                          NULL);
    return true;
}

static void plugin_destroy(void* state)
{
    state_t* s = state;
    clear_boxes(s);
    free(s);
}

static void plugin_on_ocr_done(void* state, const oshot_ocr_result_t* res)
{
    state_t* s = state;
    clear_boxes(s);
    if (res->n_words == 0)
        return;

    s->boxes = malloc(res->n_words * sizeof(*s->boxes));
    if (!s->boxes)
        return;
    s->n = res->n_words;
    for (size_t i = 0; i < res->n_words; ++i)
    {
        const oshot_ocr_word_t* w = &res->words[i];
        s->boxes[i]               = (box_t){
            .x          = res->origin_x + w->x,
            .y          = res->origin_y + w->y,
            .w          = w->w,
            .h          = w->h,
            .confidence = w->confidence,
        };
    }
}

static void plugin_render_overlay(void* state, const oshot_overlay_info_t* info)
{
    (void)info;
    state_t* s = state;
    for (size_t i = 0; i < s->n; ++i)
    {
        const box_t*   b     = &s->boxes[i];
        const uint32_t color = b->confidence >= 80 ? 0x22CC55FFu : b->confidence >= 50 ? 0xFFCC00FFu : 0xFF3344FFu;
        s->host->draw_rect(s->ctx, b->x, b->y, b->w, b->h, color & 0xFFFFFF40u, 0); /* a light fill */
        s->host->draw_rect(s->ctx, b->x, b->y, b->w, b->h, color, 1);

        if (s->show_confidence)
        {
            char label[8];
            snprintf(label, sizeof(label), "%d", (int)b->confidence);
            s->host->draw_text(s->ctx, b->x, b->y - 12, oshot_str_borrow(label), color, 11);
        }
    }
}

static void plugin_on_capture_end(void* state)
{
    clear_boxes(state);
}

static void plugin_on_settings_saved(void* state)
{
    state_t* s         = state;
    s->show_confidence = s->host->get_bool(s->ctx, OSHOT_STORE_CONFIG, "show-confidence", false);
}

static const oshot_plugin_t desc = {
    .struct_size       = sizeof(oshot_plugin_t),
    .abi_version       = OSHOT_API_VERSION,
    .imgui_version     = 0,
    .id                = "io.github.toni500github.word-boxes",
    .name              = "Word boxes",
    .init              = plugin_init,
    .destroy           = plugin_destroy,
    .on_ocr_done       = plugin_on_ocr_done,
    .settings          = settings,
    .n_settings        = sizeof(settings) / sizeof(settings[0]),
    .on_settings_saved = plugin_on_settings_saved,
    .on_capture_end    = plugin_on_capture_end,
    .render_overlay    = plugin_render_overlay,
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &desc;
}
