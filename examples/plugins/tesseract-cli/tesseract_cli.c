/* Example oshot plugin: an OCR engine that runs the `tesseract` program.
 *
 * - register_ocr_provider(): adds "Tesseract CLI" to the engine picker in the text
 *                            tools and Preferences, with the languages
 *                            `tesseract --list-langs` prints as its models
 * - recognize():             runs on a worker thread oshot starts for each scan.
 *                            It writes the image to a PGM file in the plugin's
 *                            folder, runs `tesseract <file> stdout -l <lang> tsv`
 *                            with run_process() and turns the TSV into words with
 *                            boxes. While it waits it checks req->cancelled(), and
 *                            kills the program when oshot stops waiting (the
 *                            overlay closed, or another scan started)
 * - OSHOT_OCR_PREPARED:      the "Read oshot's prepared image" setting picks the
 *                            image oshot prepares for its own Tesseract instead of
 *                            the selection as it is; changing it registers the
 *                            engine again with the other flag
 *
 * oshot joins the words into the text and averages their confidence, since the
 * output leaves both empty. run_process() calls on_exit on the UI thread, so the
 * worker waits for it with a condition variable.
 *
 * Its manifest declares the "processes" capability, since it runs a program.
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
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#include "oshot/oshot_plugin.h"

#define ENGINE_ID "tesseract-cli"

/* A run of `tesseract` for one scan. It's freed by whichever of recognize() and
   on_exit is done with it last, or by destroy() if oshot drops the on_exit */
typedef struct job
{
    struct job* next;
    char        path[1024]; /* the image file, removed once tesseract exited */
    bool        exited;     /* on_exit ran */
    bool        abandoned;  /* recognize() returned */
    int32_t     exit_code;
    char*       out; /* tesseract's stdout and stderr, NUL-terminated */
    char*       err;
} job_t;

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;

    /* What `tesseract --list-langs` printed, without "osd" */
    char**      languages;
    size_t      n_languages;
    bool        registered;
    bool        prepared; /* the setting, as last saved */
    atomic_uint next_file;

    /* Guards the jobs, and wakes the recognize() calls waiting for them */
#ifdef _WIN32
    CRITICAL_SECTION   lock;
    CONDITION_VARIABLE changed;
#else
    pthread_mutex_t lock;
    pthread_cond_t  changed;
#endif
    job_t* jobs;
} state_t;

static const oshot_setting_t settings[] = {
    {
        .kind     = OSHOT_SETTING_BOOL,
        .key      = "prepared-image",
        .label    = "Read oshot's prepared image",
        .help     = "The grey, scaled up and straightened image oshot makes for its own Tesseract, instead of the "
                    "selection as it is",
        .fallback = { .kind = OSHOT_VAL_BOOL, .b = true },
    },
};

/* ------------------------------------------------------------------
 * Locking
 * ---------------------------------------------------------------- */

static void lock(state_t* s)
{
#ifdef _WIN32
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
}

static void unlock(state_t* s)
{
#ifdef _WIN32
    LeaveCriticalSection(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif
}

static void wake_all(state_t* s)
{
#ifdef _WIN32
    WakeAllConditionVariable(&s->changed);
#else
    pthread_cond_broadcast(&s->changed);
#endif
}

/* Waits for wake_all() with the lock held, for at most 50 ms so the caller can check cancelled() */
static void wait_a_bit(state_t* s)
{
#ifdef _WIN32
    SleepConditionVariableCS(&s->changed, &s->lock, 50);
#else
    struct timespec t;
    timespec_get(&t, TIME_UTC); /* the clock pthread_cond_timedwait() uses by default */
    t.tv_nsec += 50 * 1000 * 1000;
    if (t.tv_nsec >= 1000 * 1000 * 1000)
    {
        t.tv_sec += 1;
        t.tv_nsec -= 1000 * 1000 * 1000;
    }
    pthread_cond_timedwait(&s->changed, &s->lock, &t);
#endif
}

/* ------------------------------------------------------------------
 * Jobs
 * ---------------------------------------------------------------- */

static void job_free(job_t* j)
{
    free(j->out);
    free(j->err);
    free(j);
}

/* Unlinks and frees a job, with the lock held */
static void job_drop(state_t* s, job_t* j)
{
    for (job_t** p = &s->jobs; *p; p = &(*p)->next)
    {
        if (*p == j)
        {
            *p = j->next;
            break;
        }
    }
    job_free(j);
}

static char* dup_str(oshot_str_t str)
{
    char* out = malloc(str.len + 1);
    if (out)
    {
        if (str.len)
            memcpy(out, str.p, str.len);
        out[str.len] = '\0';
    }
    return out;
}

/* run_process()'s on_exit, on the UI thread. `user` is the job */
typedef struct
{
    state_t* s;
    job_t*   job;
} exit_ctx_t;

static void on_tesseract_exit(void* user, const oshot_process_result_t* res)
{
    exit_ctx_t* e = user;
    state_t*    s = e->s;
    job_t*      j = e->job;
    free(e);

    remove(j->path);
    lock(s);
    j->exit_code = res->killed ? -1 : res->exit_code;
    j->out       = dup_str(res->stdout_data);
    j->err       = dup_str(res->stderr_data);
    j->exited    = true;
    if (j->abandoned)
        job_drop(s, j);
    wake_all(s);
    unlock(s);
}

/* ------------------------------------------------------------------
 * The image and the TSV
 * ---------------------------------------------------------------- */

/* Writes the image as a binary PGM (grey), which tesseract reads through Leptonica */
static bool write_pgm(const char* path, const oshot_capture_t* img)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;

    bool           ok  = fprintf(f, "P5\n%d %d\n255\n", img->w, img->h) > 0;
    unsigned char* row = malloc((size_t)img->w);
    ok                 = ok && row;
    for (int32_t y = 0; ok && y < img->h; ++y)
    {
        const uint8_t* p = img->rgba + (size_t)y * img->w * 4;
        for (int32_t x = 0; x < img->w; ++x, p += 4)
            row[x] = (unsigned char)((p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000);
        ok = fwrite(row, 1, (size_t)img->w, f) == (size_t)img->w;
    }
    free(row);
    return fclose(f) == 0 && ok;
}

/* The next tab-separated field of a line, NUL-terminated in place */
static char* next_field(char** cursor)
{
    char* start = *cursor;
    if (!start)
        return NULL;
    char* tab = strchr(start, '\t');
    if (tab)
    {
        *tab    = '\0';
        *cursor = tab + 1;
    }
    else
    {
        *cursor = NULL;
    }
    return start;
}

/**
 * Turns tesseract's TSV into words. Each line is: level page block paragraph
 * line word left top width height confidence text, and level 5 is a word.
 * With `out` NULL it only counts them
 **/
static size_t parse_tsv(state_t* s, char* tsv, oshot_ocr_word_t* out)
{
    size_t n          = 0;
    int    line_index = -1, block_index = -1;
    long   last_block = -1, last_par = -1, last_line = -1;

    for (char* line = tsv; line && *line;)
    {
        char* end = strchr(line, '\n');
        if (end)
            *end = '\0';

        char* cursor = line;
        char* f[12];
        int   nf = 0;
        while (nf < 12 && (f[nf] = next_field(&cursor)))
            ++nf;

        if (nf == 12 && strcmp(f[0], "5") == 0 && f[11][0] != '\0' && strcmp(f[11], "\r") != 0)
        {
            const long block = strtol(f[2], NULL, 10), par = strtol(f[3], NULL, 10), ln = strtol(f[4], NULL, 10);
            if (block != last_block)
                ++block_index;
            if (block != last_block || par != last_par || ln != last_line)
                ++line_index;
            last_block = block;
            last_par   = par;
            last_line  = ln;

            if (out)
            {
                size_t len = strlen(f[11]);
                if (len && f[11][len - 1] == '\r')
                    --len;
                const double  conf       = strtod(f[10], NULL);
                const int32_t confidence = conf < 0 ? 0 : conf > 100 ? 100 : (int32_t)(conf + 0.5);
                out[n]                   = (oshot_ocr_word_t){
                    .text       = s->host->str_new(f[11], len),
                    .x          = strtof(f[6], NULL),
                    .y          = strtof(f[7], NULL),
                    .w          = strtof(f[8], NULL),
                    .h          = strtof(f[9], NULL),
                    .confidence = confidence,
                    .line       = line_index,
                    .block      = block_index,
                };
            }
            ++n;
        }

        /* Put the separators back for the second pass */
        for (int i = 0; i < nf - 1; ++i)
            f[i][strlen(f[i])] = '\t';
        if (end)
            *end = '\n';
        line = end ? end + 1 : NULL;
    }
    return n;
}

/* ------------------------------------------------------------------
 * The engine
 * ---------------------------------------------------------------- */

static oshot_err_t fail(state_t* s, oshot_str_t* err, const char* msg)
{
    *err = s->host->str_new(msg, strlen(msg));
    return OSHOT_ERR_FAILED;
}

static oshot_err_t recognize(void* user, const oshot_ocr_request_t* req, oshot_ocr_output_t* out, oshot_str_t* err)
{
    state_t* s = user;

    oshot_str_t dir = s->host->get_data_dir(s->ctx);
    job_t*      j   = calloc(1, sizeof(*j));
    exit_ctx_t* e   = malloc(sizeof(*e));
    if (!j || !e)
    {
        free(j);
        free(e);
        s->host->str_free(&dir);
        return fail(s, err, "out of memory");
    }
    snprintf(j->path, sizeof(j->path), "%.*s/scan-%u.pgm", (int)dir.len, dir.p, atomic_fetch_add(&s->next_file, 1));
    s->host->str_free(&dir);

    if (!write_pgm(j->path, req->image))
    {
        remove(j->path);
        free(j);
        free(e);
        return fail(s, err, "couldn't write the image to the plugin's folder");
    }

    /* tesseract <image> stdout [-l <lang>] [--dpi 300] tsv */
    oshot_str_t argv[8];
    size_t      argc = 0;
    argv[argc++]     = oshot_str_borrow("tesseract");
    argv[argc++]     = oshot_str_borrow(j->path);
    argv[argc++]     = oshot_str_borrow("stdout");
    if (req->language.len > 0)
    {
        argv[argc++] = oshot_str_borrow("-l");
        argv[argc++] = req->language;
    }
    if (s->prepared)
    {
        /* oshot scales the prepared image to the glyph size of 10 pt text at 300 DPI */
        argv[argc++] = oshot_str_borrow("--dpi");
        argv[argc++] = oshot_str_borrow("300");
    }
    argv[argc++] = oshot_str_borrow("tsv");

    lock(s);
    j->next = s->jobs;
    s->jobs = j;
    unlock(s);

    e->s            = s;
    e->job          = j;
    int64_t     pid = 0;
    oshot_err_t res = s->host->run_process(s->ctx, argv, argc, oshot_str_borrow(""), on_tesseract_exit, e, &pid, err);
    if (res != OSHOT_OK)
    {
        free(e);
        remove(j->path);
        lock(s);
        job_drop(s, j);
        unlock(s);
        return res; /* *err says why, e.g. tesseract isn't in PATH */
    }

    lock(s);
    while (!j->exited)
    {
        if (req->cancelled(req))
        {
            /* on_exit still runs (killed) and frees the job then */
            j->abandoned = true;
            unlock(s);
            s->host->kill_process(s->ctx, pid, NULL);
            return OSHOT_ERR_FAILED;
        }
        wait_a_bit(s);
    }
    unlock(s);

    /* on_exit is done with the job, so it's this call's alone now */
    if (j->exit_code != 0)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "tesseract exited with %d: %.400s", j->exit_code, j->err ? j->err : "");
        char* nl = strchr(msg, '\n');
        if (nl)
            *nl = '\0';
        lock(s);
        job_drop(s, j);
        unlock(s);
        return fail(s, err, msg);
    }

    const size_t n = j->out ? parse_tsv(s, j->out, NULL) : 0;
    if (n > 0)
    {
        out->words = s->host->mem_alloc(n * sizeof(*out->words));
        if (out->words)
            out->n_words = parse_tsv(s, j->out, out->words);
    }
    lock(s);
    job_drop(s, j);
    unlock(s);
    return OSHOT_OK;
}

static void register_engine(state_t* s)
{
    const oshot_ocr_provider_t provider = {
        .struct_size = sizeof(oshot_ocr_provider_t),
        .id          = ENGINE_ID,
        .name        = "Tesseract CLI",
        .languages   = (const char* const*)s->languages,
        .n_languages = s->n_languages,
        .flags       = s->prepared ? OSHOT_OCR_PREPARED : 0,
        .recognize   = recognize,
        .user        = s,
    };
    s->registered = s->host->register_ocr_provider(s->ctx, &provider, NULL) == OSHOT_OK;
}

/* on_exit of `tesseract --list-langs`, on the UI thread */
static void on_list_langs(void* user, const oshot_process_result_t* res)
{
    state_t* s = user;
    if (res->exit_code != 0)
    {
        s->host->log(s->ctx, OSHOT_LOG_ERROR, oshot_str_borrow("`tesseract --list-langs` failed, no engine added"));
        return;
    }

    /* The first line is "List of available languages in "<dir>" (<n>):" */
    char* text = dup_str(res->stdout_data);
    for (char* nl = text ? strchr(text, '\n') : NULL; nl;)
    {
        char* start = nl + 1;
        nl          = strchr(start, '\n');
        if (nl)
            *nl = '\0';
        size_t len = strlen(start);
        while (len && (start[len - 1] == '\r' || start[len - 1] == ' '))
            start[--len] = '\0';
        if (len == 0 || strcmp(start, "osd") == 0)
            continue;

        char** langs = realloc(s->languages, (s->n_languages + 1) * sizeof(*langs));
        if (!langs)
            break;
        s->languages                   = langs;
        s->languages[s->n_languages++] = dup_str(oshot_str_borrow(start));
    }
    free(text);
    register_engine(s);
}

/* ------------------------------------------------------------------
 * Plugin callbacks
 * ---------------------------------------------------------------- */

static bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    if (!OSHOT_HOST_HAS(host, unregister_ocr_provider))
    {
        host->log(ctx, OSHOT_LOG_ERROR, oshot_str_borrow("this oshot is too old for OCR engines from plugins"));
        return false;
    }

    state_t* s = calloc(1, sizeof(*s));
    if (!s)
        return false;
    s->host     = host;
    s->ctx      = ctx;
    s->prepared = host->get_bool(ctx, OSHOT_STORE_CONFIG, "prepared-image", true);
#ifdef _WIN32
    InitializeCriticalSection(&s->lock);
    InitializeConditionVariable(&s->changed);
#else
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->changed, NULL);
#endif
    *state = s;

    /* Registered once the languages are known, in on_list_langs() */
    const oshot_str_t argv[] = { oshot_str_borrow("tesseract"), oshot_str_borrow("--list-langs") };
    oshot_str_t       err    = { NULL, 0 };
    if (host->run_process(ctx, argv, 2, oshot_str_borrow(""), on_list_langs, s, NULL, &err) != OSHOT_OK)
    {
        host->log(ctx, OSHOT_LOG_WARN, err);
        host->str_free(&err);
    }
    return true;
}

static void plugin_destroy(void* state)
{
    state_t* s = state;

    /* oshot has stopped the scans and the processes, and dropped their on_exit */
    while (s->jobs)
    {
        remove(s->jobs->path);
        job_drop(s, s->jobs);
    }
    for (size_t i = 0; i < s->n_languages; ++i)
        free(s->languages[i]);
    free(s->languages);
#ifdef _WIN32
    DeleteCriticalSection(&s->lock);
#else
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->changed);
#endif
    free(s);
}

static void plugin_on_settings_saved(void* state)
{
    state_t*   s        = state;
    const bool prepared = s->host->get_bool(s->ctx, OSHOT_STORE_CONFIG, "prepared-image", true);
    if (prepared == s->prepared)
        return;
    s->prepared = prepared;

    /* The flags are fixed once registered, so it's registered again */
    if (s->registered)
    {
        s->host->unregister_ocr_provider(s->ctx, ENGINE_ID, NULL);
        register_engine(s);
    }
}

static const oshot_plugin_t desc = {
    .struct_size       = sizeof(oshot_plugin_t),
    .abi_version       = OSHOT_API_VERSION,
    .imgui_version     = 0,
    .id                = "io.github.toni500github.tesseract-cli",
    .name              = "Tesseract CLI",
    .init              = plugin_init,
    .destroy           = plugin_destroy,
    .settings          = settings,
    .n_settings        = sizeof(settings) / sizeof(settings[0]),
    .on_settings_saved = plugin_on_settings_saved,
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &desc;
}
