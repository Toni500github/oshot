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

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "oshot/oshot_plugin.h"
#include "stb_image_write.h"

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
    FILE*           f;
    bool            write_failed;

#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif

    atomic_bool done; /* set by the thread as it ends */
} state_t;

static void callback_stbi(void* context, void* data, int size)
{
    state_t* s = (state_t*)context;
    if (fwrite(data, 1, (size_t)size, s->f) != (size_t)size)
        s->write_failed = true;
}

static bool write_png(state_t* s)
{
    const oshot_capture_t* img = &s->image;
    stbi_write_png_to_func(callback_stbi, s, img->w, img->h, 4, img->rgba, img->w * 4);
    return !s->write_failed;
}

/* ------------------------------------------------------------------
 * The export, on its own thread
 * ---------------------------------------------------------------- */

static void run_export(state_t* s)
{
    s->f    = fopen(s->path, "wb");
    bool ok = s->f && write_png(s);
    if (s->f && fclose(s->f) != 0)
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
    s->f    = NULL;

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
