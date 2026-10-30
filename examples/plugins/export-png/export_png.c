/* Example oshot plugin: an export action that saves the annotated selection as a PNG.
 *
 * - register_action():     a toolbar button with its own icon, and the same action in
 *                          the text tools' Plugins menu
 * - request_final_image(): the selection with the annotations drawn on it, as Save
 *                          would write it. It arrives in on_ready() a frame later,
 *                          and belongs to the plugin until capture_free()
 * - a worker thread:       encodes and writes the PNG into the plugin's own folder,
 *                          <data dir>/exports, so the overlay doesn't freeze on a big
 *                          selection. The encoder is a plain one without compression,
 *                          to keep the example free of libraries
 * - report_progress(), report_result(): shown in the overlay's corner; the result
 *                          offers the file's path to copy. If the overlay was closed
 *                          meanwhile, oshot sends the result as a notification
 *
 * It only writes inside its own folder, so its manifest declares no capability.
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <direct.h>
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sys/stat.h>
#endif

#include "oshot/oshot_plugin.h"

#define ICON_SIZE 24

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;
    int32_t             toolbar_action;
    int32_t             menu_action;

    /* The running export. Only the UI thread starts and joins it */
    bool            busy;
    int32_t         action; /* the one that started it, for its reports */
    oshot_capture_t image;
    char            path[1024];
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    atomic_bool done; /* set by the thread as it ends */
} state_t;

/* ------------------------------------------------------------------
 * PNG, with "stored" (uncompressed) deflate blocks
 * ---------------------------------------------------------------- */

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; ++n)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void put_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool write_chunk(FILE* f, const char type[4], const uint8_t* data, size_t len)
{
    uint8_t head[8];
    put_u32(head, (uint32_t)len);
    memcpy(head + 4, type, 4);
    uint8_t crc[4];
    put_u32(crc, crc_update(crc_update(0xFFFFFFFFu, head + 4, 4), data, len) ^ 0xFFFFFFFFu);
    return fwrite(head, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(crc, 1, 4, f) == 4;
}

/* Writes the image, one IDAT chunk per row, and reports the progress every few rows */
static bool write_png(state_t* s, FILE* f)
{
    const oshot_capture_t* img    = &s->image;
    static const uint8_t   sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t                ihdr[13];
    put_u32(ihdr, (uint32_t)img->w);
    put_u32(ihdr + 4, (uint32_t)img->h);
    ihdr[8]  = 8; /* bits per channel */
    ihdr[9]  = 6; /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (fwrite(sig, 1, 8, f) != 8 || !write_chunk(f, "IHDR", ihdr, sizeof(ihdr)))
        return false;

    /* A row is a filter byte (0, none) and the pixels, split into stored blocks of at most 65535 bytes */
    const size_t row_len  = (size_t)img->w * 4 + 1;
    const size_t n_blocks = (row_len + 65534) / 65535;
    uint8_t*     chunk    = malloc(2 + row_len + n_blocks * 5 + 4);
    uint8_t*     row      = malloc(row_len);
    if (!chunk || !row)
    {
        free(chunk);
        free(row);
        return false;
    }

    uint32_t adler_a = 1, adler_b = 0;
    bool     ok = true;
    for (int32_t y = 0; ok && y < img->h; ++y)
    {
        row[0] = 0;
        memcpy(row + 1, img->rgba + (size_t)y * img->w * 4, row_len - 1);
        for (size_t i = 0; i < row_len; ++i)
        {
            adler_a = (adler_a + row[i]) % 65521;
            adler_b = (adler_b + adler_a) % 65521;
        }

        size_t n = 0;
        if (y == 0)
        {
            chunk[n++] = 0x78; /* zlib header: deflate, 32K window */
            chunk[n++] = 0x01;
        }
        for (size_t off = 0; off < row_len; off += 65535)
        {
            const size_t   len  = row_len - off < 65535 ? row_len - off : 65535;
            const bool     last = y == img->h - 1 && off + len == row_len;
            const uint16_t l = (uint16_t)len, nl = (uint16_t)~l;
            chunk[n++] = last ? 1 : 0;
            chunk[n++] = (uint8_t)l;
            chunk[n++] = (uint8_t)(l >> 8);
            chunk[n++] = (uint8_t)nl;
            chunk[n++] = (uint8_t)(nl >> 8);
            memcpy(chunk + n, row + off, len);
            n += len;
        }
        if (y == img->h - 1)
        {
            put_u32(chunk + n, (adler_b << 16) | adler_a);
            n += 4;
        }
        ok = write_chunk(f, "IDAT", chunk, n);

        if (y % 64 == 0)
            s->host->report_progress(
                s->ctx, s->action, (float)y / (float)img->h, oshot_str_borrow("Writing the PNG..."), NULL);
    }
    free(chunk);
    free(row);
    return ok && write_chunk(f, "IEND", NULL, 0);
}

/* ------------------------------------------------------------------
 * The export, on its own thread
 * ---------------------------------------------------------------- */

static void run_export(state_t* s)
{
    FILE* f  = fopen(s->path, "wb");
    bool  ok = f && write_png(s, f);
    if (f && fclose(f) != 0)
        ok = false;
    s->host->capture_free(&s->image);

    if (ok)
        s->host->report_result(s->ctx,
                               s->action,
                               true,
                               oshot_str_borrow("Saved the annotated selection"),
                               oshot_str_borrow(s->path),
                               NULL);
    else
        s->host->report_result(
            s->ctx, s->action, false, oshot_str_borrow("Couldn't write the PNG"), oshot_str_borrow(s->path), NULL);
    s->done = true;
}

#ifdef _WIN32
static DWORD WINAPI export_thread(LPVOID arg)
{
    run_export(arg);
    return 0;
}
#else
static void* export_thread(void* arg)
{
    run_export(arg);
    return NULL;
}
#endif

/* Waits for the last export, if there was one */
static void join_export(state_t* s)
{
    if (!s->busy)
        return;
#ifdef _WIN32
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
#else
    pthread_join(s->thread, NULL);
#endif
    s->busy = false;
}

/* <data dir>/exports/oshot-<date>-<time>.png */
static bool make_path(state_t* s)
{
    oshot_str_t dir = s->host->get_data_dir(s->ctx);
    if (!dir.p)
        return false;

    char folder[900];
    snprintf(folder, sizeof(folder), "%s/exports", dir.p);
    s->host->str_free(&dir);
#ifdef _WIN32
    _mkdir(folder);
#else
    mkdir(folder, 0755);
#endif

    char       stamp[32];
    time_t     now = time(NULL);
    struct tm* tm  = localtime(&now);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
    snprintf(s->path, sizeof(s->path), "%s/oshot-%s.png", folder, stamp);
    return true;
}

static void on_image_ready(void* user, oshot_capture_t image)
{
    state_t* s = user;
    if (!image.rgba)
    {
        s->host->report_result(s->ctx,
                               s->action,
                               false,
                               oshot_str_borrow("The overlay closed before the image was ready"),
                               oshot_str_borrow(NULL),
                               NULL);
        return;
    }

    join_export(s);
    s->image = image;
    s->done  = false;
    if (!make_path(s))
    {
        s->host->capture_free(&s->image);
        return;
    }

    s->host->report_progress(s->ctx, s->action, 0.0f, oshot_str_borrow("Starting..."), NULL);
#ifdef _WIN32
    s->thread = CreateThread(NULL, 0, export_thread, s, 0, NULL);
    s->busy   = s->thread != NULL;
#else
    s->busy = pthread_create(&s->thread, NULL, export_thread, s) == 0;
#endif
    if (!s->busy)
    {
        s->host->capture_free(&s->image);
        s->host->report_result(
            s->ctx, s->action, false, oshot_str_borrow("Couldn't start a thread"), oshot_str_borrow(NULL), NULL);
    }
}

static void on_export_clicked(void* user, int32_t action)
{
    state_t* s = user;
    if (s->busy && !s->done)
    {
        s->host->report_result(
            s->ctx, action, false, oshot_str_borrow("Still saving the last one"), oshot_str_borrow(NULL), NULL);
        return;
    }

    s->action       = action;
    oshot_str_t err = { NULL, 0 };
    if (s->host->request_final_image(s->ctx, on_image_ready, s, &err) != OSHOT_OK)
    {
        s->host->report_result(s->ctx, action, false, err, oshot_str_borrow(NULL), NULL);
        s->host->str_free(&err);
    }
}

/* ------------------------------------------------------------------
 * Plugin
 * ---------------------------------------------------------------- */

/* A white arrow pointing down into a tray, on a transparent background */
static void draw_icon(uint8_t* px)
{
    memset(px, 0, ICON_SIZE * ICON_SIZE * 4);
    for (int y = 0; y < ICON_SIZE; ++y)
    {
        for (int x = 0; x < ICON_SIZE; ++x)
        {
            const int  dx   = x - ICON_SIZE / 2;
            const int  adx  = dx < 0 ? -dx - 1 : dx;
            const bool stem = y >= 2 && y < 12 && adx < 2;
            const bool head = y >= 9 && y < 16 && adx < 16 - y;
            const bool tray = (y >= 19 && y < 21 && x >= 3 && x < 21) || (y >= 15 && y < 21 && (x == 3 || x == 20));
            if (stem || head || tray)
                memset(px + ((size_t)y * ICON_SIZE + x) * 4, 0xFF, 4);
        }
    }
}

static bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    if (!OSHOT_HOST_HAS(host, report_result))
    {
        host->log(ctx, OSHOT_LOG_ERROR, oshot_str_borrow("needs an oshot with plugin actions, update oshot"));
        return false;
    }

    state_t* s = calloc(1, sizeof(*s));
    if (!s)
        return false;
    s->host = host;
    s->ctx  = ctx;
    crc_init();

    static uint8_t        icon_px[ICON_SIZE * ICON_SIZE * 4];
    const oshot_capture_t icon = { ICON_SIZE, ICON_SIZE, icon_px };
    draw_icon(icon_px);

    oshot_str_t err = { NULL, 0 };
    if (host->register_action(ctx,
                              OSHOT_PLACE_TOOLBAR,
                              oshot_str_borrow("Export as PNG"),
                              &icon,
                              on_export_clicked,
                              s,
                              &s->toolbar_action,
                              &err) != OSHOT_OK ||
        host->register_action(ctx,
                              OSHOT_PLACE_TEXT_TOOLS,
                              oshot_str_borrow("Export as PNG"),
                              NULL,
                              on_export_clicked,
                              s,
                              &s->menu_action,
                              &err) != OSHOT_OK)
    {
        host->log(ctx, OSHOT_LOG_ERROR, err);
        host->str_free(&err);
        free(s);
        return false;
    }

    *state = s;
    return true;
}

static void plugin_destroy(void* state)
{
    /* An export still running gets to finish, it's the person's image */
    join_export(state);
    free(state);
}

static const oshot_plugin_t descriptor = {
    .struct_size   = sizeof(oshot_plugin_t),
    .abi_version   = OSHOT_API_VERSION,
    .imgui_version = 0, /* no cimgui */
    .id            = "io.github.toni500github.export-png",
    .name          = "Export PNG",
    .init          = plugin_init,
    .destroy       = plugin_destroy,
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &descriptor;
}
