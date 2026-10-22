/* Example oshot plugin: lists what the scans found, with oshot's host services.
 *
 * - on_barcodes_done(): every QR code and barcode with its position, with buttons
 *                       to open or copy its data, or copy its image (get_capture()
 *                       cropped to its box, with copy_image()), and a notification
 *                       when some are found
 * - on_ocr_done():      the words Tesseract wasn't sure about, with their boxes
 * - on_capture_start(), on_capture_end(), on_selection_changed(): per-capture state;
 *                       the boxes belong to the selection that was scanned, so a new
 *                       selection clears them
 * - run_process():      runs `uname -a` (`cmd /c ver` on Windows) and shows its
 *                       output, or a long `sleep` to try kill_process()
 *
 * Host functions that can fail return an oshot_err_t and take an err out-parameter,
 * which report() shows under the plugin's section.
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oshot/oshot_plugin.h"

typedef struct
{
    char* text; /* "QR-Code: https://..." or "word (42%)" */
    char* data; /* what Open and Copy use: the code's data; NULL for words */
    float x, y, w, h;
} item_t;

typedef struct
{
    item_t* items;
    size_t  n;
} list_t;

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;

    /* The settings, as last saved */
    int64_t min_confidence;
    bool    notify_codes;

    /* This capture's */
    list_t codes;
    list_t words;
    float  sel_x, sel_y, sel_w, sel_h;

    int64_t process;        /* the running process, or 0 */
    char*   process_output; /* what the last one printed */
    char    status[512];    /* the last error, or what happened */
} state_t;

static const oshot_setting_t settings[] = {
    {
        .kind     = OSHOT_SETTING_INT,
        .key      = "min-confidence",
        .label    = "List words read with less confidence than",
        .help     = "Tesseract's confidence for each word, 0-100",
        .fallback = { .kind = OSHOT_VAL_INT64, .i = 70 },
        .min      = 0,
        .max      = 100,
    },
    {
        .kind     = OSHOT_SETTING_BOOL,
        .key      = "notify-codes",
        .label    = "Notify when QR codes or barcodes are found",
        .fallback = { .kind = OSHOT_VAL_BOOL, .b = true },
    },
};

static char* dup_str(oshot_str_t s)
{
    char* out = malloc(s.len + 1);
    if (out)
    {
        if (s.len)
            memcpy(out, s.p, s.len);
        out[s.len] = '\0';
    }
    return out;
}

static void list_clear(list_t* l)
{
    for (size_t i = 0; i < l->n; ++i)
    {
        free(l->items[i].text);
        free(l->items[i].data);
    }
    free(l->items);
    l->items = NULL;
    l->n     = 0;
}

static item_t* list_add(list_t* l)
{
    item_t* items = realloc(l->items, (l->n + 1) * sizeof(*items));
    if (!items)
        return NULL;
    l->items = items;
    memset(&items[l->n], 0, sizeof(*items));
    return &items[l->n++];
}

static void log_msg(state_t* s, oshot_log_level_t level, const char* msg)
{
    s->host->log(s->ctx, level, oshot_str_borrow(msg));
}

/* Shows what a host call returned: its error message, or `ok` */
static void report(state_t* s, oshot_err_t code, oshot_str_t* err, const char* ok)
{
    if (code == OSHOT_OK)
        snprintf(s->status, sizeof(s->status), "%s", ok);
    else
        snprintf(s->status, sizeof(s->status), "Error %d: %.*s", (int)code, (int)err->len, err->p ? err->p : "");
    s->host->str_free(err);
}

static void read_settings(state_t* s)
{
    s->min_confidence = s->host->get_int64(s->ctx, OSHOT_STORE_CONFIG, "min-confidence", 70);
    s->notify_codes   = s->host->get_bool(s->ctx, OSHOT_STORE_CONFIG, "notify-codes", true);
}

static void clear_capture(state_t* s)
{
    list_clear(&s->codes);
    list_clear(&s->words);
    s->sel_w = s->sel_h = 0;
    s->status[0]        = '\0';
}

static bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    /* The host services came after the widgets: check the last one */
    if (!OSHOT_HOST_HAS(host, kill_process))
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
    *state = s;
    return true;
}

static void plugin_destroy(void* state)
{
    /* oshot has already killed the processes still running, without calling on_exit */
    state_t* s = state;
    clear_capture(s);
    free(s->process_output);
    free(s);
}

static void on_process_exit(void* user, const oshot_process_result_t* result)
{
    state_t* s = user;
    if (s->process == result->id)
        s->process = 0;

    free(s->process_output);
    s->process_output = dup_str(result->stdout_data.len ? result->stdout_data : result->stderr_data);
    snprintf(s->status,
             sizeof(s->status),
             "Process %" PRId64 " %s %d%s",
             result->id,
             result->killed ? "killed, exit code" : "exited with code",
             (int)result->exit_code,
             result->truncated ? " (output cut)" : "");
    log_msg(s, OSHOT_LOG_INFO, s->status);
}

static void start_process(state_t* s, const char* const* args, size_t argc)
{
    oshot_str_t argv[4];
    for (size_t i = 0; i < argc; ++i)
        argv[i] = oshot_str_borrow(args[i]);

    oshot_str_t       err  = { NULL, 0 };
    const oshot_str_t cwd  = { NULL, 0 }; /* the plugin's folder */
    oshot_err_t       code = s->host->run_process(s->ctx, argv, argc, cwd, on_process_exit, s, &s->process, &err);
    report(s, code, &err, "Started");
}

/* Copies the part of the selection inside a box, with a margin, as an image */
static void copy_box(state_t* s, const item_t* it)
{
    oshot_capture_t cap = s->host->get_capture(s->ctx);
    if (!cap.rgba)
    {
        snprintf(s->status, sizeof(s->status), "No image to copy");
        return;
    }

    /* The box is in the pixels of the image get_capture() returns for the same selection */
    const float margin = it->w > it->h ? it->w / 10 + 4 : it->h / 10 + 4;
    int32_t     x0 = (int32_t)(it->x - margin), y0 = (int32_t)(it->y - margin);
    int32_t     x1 = (int32_t)(it->x + it->w + margin), y1 = (int32_t)(it->y + it->h + margin);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > cap.w ? cap.w : x1;
    y1 = y1 > cap.h ? cap.h : y1;

    oshot_capture_t crop = { x1 - x0, y1 - y0, NULL };
    if (crop.w > 0 && crop.h > 0)
        crop.rgba = malloc((size_t)crop.w * (size_t)crop.h * 4);
    if (crop.rgba)
    {
        for (int32_t y = 0; y < crop.h; ++y)
            memcpy(crop.rgba + ((size_t)y * (size_t)crop.w * 4),
                   cap.rgba + ((((size_t)(y0 + y) * (size_t)cap.w) + (size_t)x0) * 4),
                   (size_t)crop.w * 4);

        /* oshot copies the pixels, so they stay the plugin's to free */
        oshot_str_t err = { NULL, 0 };
        report(s, s->host->copy_image(s->ctx, &crop, &err), &err, "Copied the image");
        free(crop.rgba);
    }
    s->host->capture_free(&cap);
}

static void draw_items(state_t* s, const list_t* l, bool buttons)
{
    const oshot_host_t* host = s->host;
    char                line[640];
    for (size_t i = 0; i < l->n; ++i)
    {
        const item_t* it = &l->items[i];
        snprintf(line, sizeof(line), "%s  [%.0f,%.0f %.0fx%.0f]", it->text, it->x, it->y, it->w, it->h);
        host->ui_text(s->ctx, oshot_str_borrow(line), OSHOT_UI_WRAPPED);
        if (!buttons || !it->data)
            continue;

        oshot_str_t err = { NULL, 0 };
        snprintf(line, sizeof(line), "Open##%zu", i);
        if (host->ui_button(s->ctx, oshot_str_borrow(line)))
            report(s, host->open_url(s->ctx, oshot_str_borrow(it->data), &err), &err, "Opened");
        host->ui_same_line(s->ctx);
        snprintf(line, sizeof(line), "Copy text##%zu", i);
        if (host->ui_button(s->ctx, oshot_str_borrow(line)))
            report(s, host->copy_text(s->ctx, oshot_str_borrow(it->data), &err), &err, "Copied");
        host->ui_same_line(s->ctx);
        snprintf(line, sizeof(line), "Copy image##%zu", i);
        if (host->ui_button(s->ctx, oshot_str_borrow(line)))
            copy_box(s, it);
    }
}

static void plugin_render(void* state)
{
    state_t*            s    = state;
    const oshot_host_t* host = s->host;
    char                line[256];

    if (s->sel_w > 0)
    {
        snprintf(line, sizeof(line), "Selection: %.0fx%.0f at %.0f,%.0f", s->sel_w, s->sel_h, s->sel_x, s->sel_y);
        host->ui_text(s->ctx, oshot_str_borrow(line), OSHOT_UI_DISABLED);
    }

    snprintf(line, sizeof(line), "QR codes and barcodes (%zu)##codes", s->codes.n);
    if (host->ui_begin_section(s->ctx, oshot_str_borrow(line)))
        draw_items(s, &s->codes, true);
    host->ui_end_section(s->ctx);

    snprintf(line, sizeof(line), "Words under %" PRId64 "%% confidence (%zu)##words", s->min_confidence, s->words.n);
    if (host->ui_begin_section(s->ctx, oshot_str_borrow(line)))
        draw_items(s, &s->words, false);
    host->ui_end_section(s->ctx);

    if (host->ui_begin_section(s->ctx, oshot_str_borrow("Processes")))
    {
#ifdef _WIN32
        static const char* const info[]     = { "cmd", "/c", "ver" };
        static const char* const long_run[] = { "ping", "-n", "600", "127.0.0.1" };
#else
        static const char* const info[]     = { "uname", "-a" };
        static const char* const long_run[] = { "sleep", "600" };
#endif
        if (host->ui_button(s->ctx, oshot_str_borrow("Run uname")))
            start_process(s, info, sizeof(info) / sizeof(info[0]));
        host->ui_same_line(s->ctx);
        if (host->ui_button(s->ctx, oshot_str_borrow("Sleep for 10 minutes")))
            start_process(s, long_run, sizeof(long_run) / sizeof(long_run[0]));
        if (s->process)
        {
            host->ui_same_line(s->ctx);
            if (host->ui_button(s->ctx, oshot_str_borrow("Kill")))
            {
                oshot_str_t err = { NULL, 0 };
                report(s, host->kill_process(s->ctx, s->process, &err), &err, "Killing it");
            }
        }
        if (s->process_output)
            host->ui_text(s->ctx, oshot_str_borrow(s->process_output), OSHOT_UI_WRAPPED);
    }
    host->ui_end_section(s->ctx);

    if (s->status[0])
        host->ui_text(s->ctx, oshot_str_borrow(s->status), OSHOT_UI_DISABLED | OSHOT_UI_WRAPPED);
}

static void plugin_on_ocr_done(void* state, const oshot_ocr_result_t* result)
{
    state_t* s = state;
    list_clear(&s->words);
    if (!OSHOT_OCR_HAS_WORDS(s->host))
        return;

    char line[512];
    snprintf(line, sizeof(line), "OCR read %zu words", result->n_words);
    log_msg(s, OSHOT_LOG_INFO, line);

    for (size_t i = 0; i < result->n_words; ++i)
    {
        const oshot_ocr_word_t* w = &result->words[i];
        if (i < 5) /* enough to check the boxes against the image */
        {
            snprintf(line,
                     sizeof(line),
                     "word %zu '%.*s' at %.0f,%.0f %.0fx%.0f, %d%%, line %d, block %d",
                     i,
                     (int)w->text.len,
                     w->text.p,
                     w->x,
                     w->y,
                     w->w,
                     w->h,
                     (int)w->confidence,
                     (int)w->line,
                     (int)w->block);
            log_msg(s, OSHOT_LOG_INFO, line);
        }
        if (w->confidence >= s->min_confidence)
            continue;

        item_t* it = list_add(&s->words);
        if (!it)
            return;
        snprintf(line, sizeof(line), "%.*s (%d%%)", (int)w->text.len, w->text.p, (int)w->confidence);
        it->text = dup_str(oshot_str_borrow(line));
        it->x    = w->x;
        it->y    = w->y;
        it->w    = w->w;
        it->h    = w->h;
    }
}

static void plugin_on_barcodes_done(void* state, const oshot_barcode_t* codes, size_t n)
{
    state_t* s = state;
    list_clear(&s->codes);

    char line[512];
    for (size_t i = 0; i < n; ++i)
    {
        item_t* it = list_add(&s->codes);
        if (!it)
            return;
        snprintf(line,
                 sizeof(line),
                 "%.*s: %.*s",
                 (int)codes[i].type.len,
                 codes[i].type.p,
                 (int)codes[i].data.len,
                 codes[i].data.p);
        it->text = dup_str(oshot_str_borrow(line));
        it->data = dup_str(codes[i].data);
        it->x    = codes[i].x;
        it->y    = codes[i].y;
        it->w    = codes[i].w;
        it->h    = codes[i].h;

        snprintf(line, sizeof(line), "code %zu %s at %.0f,%.0f %.0fx%.0f", i, it->text, it->x, it->y, it->w, it->h);
        log_msg(s, OSHOT_LOG_INFO, line);
    }

    if (n > 0 && s->notify_codes)
    {
        snprintf(line, sizeof(line), "%zu QR code%s or barcode%s found", n, n == 1 ? "" : "s", n == 1 ? "" : "s");
        oshot_str_t err = { NULL, 0 };
        report(s,
               s->host->notify(s->ctx, oshot_str_borrow("Scan results"), oshot_str_borrow(line), &err),
               &err,
               "Notified");
    }
}

static void plugin_on_capture_start(void* state)
{
    log_msg(state, OSHOT_LOG_INFO, "capture started");
}

static void plugin_on_capture_end(void* state)
{
    /* The tray keeps the plugin loaded: nothing of this capture may show up in the next */
    state_t* s = state;
    clear_capture(s);
    log_msg(s, OSHOT_LOG_INFO, "capture ended");
}

static void plugin_on_selection_changed(void* state, float x, float y, float w, float h)
{
    state_t* s = state;
    s->sel_x   = x;
    s->sel_y   = y;
    s->sel_w   = w;
    s->sel_h   = h;
    list_clear(&s->codes);
    list_clear(&s->words);

    char line[128];
    snprintf(line, sizeof(line), "selection changed: %.0fx%.0f at %.0f,%.0f", w, h, x, y);
    log_msg(s, OSHOT_LOG_INFO, line);
}

static void plugin_on_settings_saved(void* state)
{
    read_settings(state);
}

static const oshot_plugin_t descriptor = {
    .struct_size          = sizeof(oshot_plugin_t),
    .abi_version          = OSHOT_API_VERSION,
    .imgui_version        = 0, /* no cimgui */
    .id                   = "io.github.toni500github.scan-results",
    .name                 = "Scan results",
    .init                 = plugin_init,
    .destroy              = plugin_destroy,
    .render               = plugin_render,
    .on_ocr_done          = plugin_on_ocr_done,
    .settings             = settings,
    .n_settings           = sizeof(settings) / sizeof(settings[0]),
    .on_settings_saved    = plugin_on_settings_saved,
    .on_barcodes_done     = plugin_on_barcodes_done,
    .on_capture_start     = plugin_on_capture_start,
    .on_capture_end       = plugin_on_capture_end,
    .on_selection_changed = plugin_on_selection_changed,
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &descriptor;
}
