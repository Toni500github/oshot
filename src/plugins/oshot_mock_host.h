/*
 * Copyright 2026 Toni500
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#pragma once

/**
 * A fake oshot for unit-testing plugins without running the overlay.
 *
 * It implements every function of oshot_host_t in memory. Config and cache
 * values, the OCR and barcode text, a capture image set by the test, the
 * clipboard, notifications, actions and reports are all kept in the mock, where
 * the test reads them back. Programs aren't run. run_process() records the
 * command and finishes with the result set by oshot_mock_set_process_result().
 * Widgets draw nothing. The mock records what was drawn, and can press a
 * button, toggle a checkbox or type into a field in the next frame.
 *
 * Header-only, C11 or C++. Compile the plugin's sources into the test program
 * and pass its descriptor to the mock:
 *
 *     #include "oshot/oshot_mock_host.h"
 *
 *     oshot_mock_t* m = oshot_mock_new();
 *     oshot_mock_set_bool(m, OSHOT_STORE_CONFIG, "auto-uppercase", true);
 *     if (!oshot_mock_load(m, oshot_plugin_entry()))
 *         return 1;
 *     oshot_mock_capture_start(m);
 *     oshot_mock_ocr(m, "hello", NULL, 0);
 *     assert(strcmp(oshot_mock_text(m, OSHOT_TEXT_OCR), "HELLO") == 0);
 *     oshot_mock_free(m);
 *     assert(oshot_mock_live_allocations() == 0);
 *
 * The thread that calls oshot_mock_new() is oshot's UI thread, so host
 * functions that oshot only allows there fail with OSHOT_ERR_THREAD (or log an
 * error) when a plugin's own thread calls them, as in oshot. The overlay counts as
 * open between oshot_mock_capture_start() and oshot_mock_capture_end(), which
 * get_capture(), open_url() and request_final_image() need.
 *
 * The mock prints the plugin's warnings and errors to stderr, along with the
 * mistakes it notices (a widget outside render(), a section left open, a
 * duplicate label, a UI-thread function called from another thread). It
 * doesn't check how fast transform_text() is.
 **/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "oshot_plugin.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oshot_mock oshot_mock_t;

/* ------------------------------------------------------------------
 * Internals. Everything after this block is the mock's API.
 * ---------------------------------------------------------------- */

#if defined(_WIN32)
typedef SRWLOCK oshot_mock__lock_t;
#  define OSHOT_MOCK__LOCK_INIT SRWLOCK_INIT
static inline void oshot_mock__lock(oshot_mock__lock_t* l)
{
    AcquireSRWLockExclusive(l);
}
static inline void oshot_mock__unlock(oshot_mock__lock_t* l)
{
    ReleaseSRWLockExclusive(l);
}
typedef DWORD                      oshot_mock__thread_t;
static inline oshot_mock__thread_t oshot_mock__this_thread(void)
{
    return GetCurrentThreadId();
}
static inline bool oshot_mock__same_thread(oshot_mock__thread_t a, oshot_mock__thread_t b)
{
    return a == b;
}
#else
typedef pthread_mutex_t oshot_mock__lock_t;
#  define OSHOT_MOCK__LOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void      oshot_mock__lock(oshot_mock__lock_t* l)
{
    pthread_mutex_lock(l);
}
static inline void oshot_mock__unlock(oshot_mock__lock_t* l)
{
    pthread_mutex_unlock(l);
}
typedef pthread_t                  oshot_mock__thread_t;
static inline oshot_mock__thread_t oshot_mock__this_thread(void)
{
    return pthread_self();
}
static inline bool oshot_mock__same_thread(oshot_mock__thread_t a, oshot_mock__thread_t b)
{
    return pthread_equal(a, b) != 0;
}
#endif

/* Counts host strings, mem_alloc() blocks and capture images not freed yet, over every mock */
static oshot_mock__lock_t oshot_mock__alloc_lock  = OSHOT_MOCK__LOCK_INIT;
static size_t             oshot_mock__alloc_count = 0;

static inline void* oshot_mock__alloc(size_t size)
{
    void* p = malloc(size ? size : 1);
    if (p)
    {
        oshot_mock__lock(&oshot_mock__alloc_lock);
        ++oshot_mock__alloc_count;
        oshot_mock__unlock(&oshot_mock__alloc_lock);
    }
    return p;
}

static inline void oshot_mock__free(void* p)
{
    if (!p)
        return;
    free(p);
    oshot_mock__lock(&oshot_mock__alloc_lock);
    --oshot_mock__alloc_count;
    oshot_mock__unlock(&oshot_mock__alloc_lock);
}

/* Whether the plugin's descriptor has `field`, which an older plugin's may end before */
#define OSHOT_MOCK__HAS(plugin, field) \
    (offsetof(oshot_plugin_t, field) + sizeof(((oshot_plugin_t*)0)->field) <= (plugin)->struct_size)

/* A plain NUL-terminated copy for the mock's own bookkeeping (not counted) */
static inline char* oshot_mock__strndup(const char* s, size_t len)
{
    char* r = (char*)malloc(len + 1);
    if (!r)
        abort();
    if (len)
        memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

static inline char* oshot_mock__strdup(const char* s)
{
    return oshot_mock__strndup(s ? s : "", s ? strlen(s) : 0);
}

/* A growable array of T, as `T* items; size_t n, cap;` */
#define OSHOT_MOCK__PUSH(arr, n, cap, T)                              \
    do                                                                \
    {                                                                 \
        if ((n) == (cap))                                             \
        {                                                             \
            (cap)     = (cap) ? (cap) * 2 : 8;                        \
            T* grown_ = (T*)realloc((void*)(arr), (cap) * sizeof(T)); \
            if (!grown_)                                              \
                abort();                                              \
            (arr) = grown_;                                           \
        }                                                             \
        memset((void*)&(arr)[(n)], 0, sizeof(T));                     \
        ++(n);                                                        \
    } while (0)

typedef struct
{
    oshot_store_t  store;
    char*          key;
    oshot_value_t  value; /* its string, if any, is a plain copy */
    oshot_value_t* array; /* set with oshot_mock_set_array(), else NULL */
    size_t         n_array;
} oshot_mock__entry_t;

typedef struct
{
    oshot_log_level_t level;
    char*             msg;
} oshot_mock__log_t;

typedef struct
{
    void (*fn)(void* user);
    void* user;
} oshot_mock__task_t;

typedef struct
{
    int64_t id;
    bool    killed;
    void (*on_exit)(void* user, const oshot_process_result_t* result);
    void* user;
} oshot_mock__process_t;

typedef struct
{
    void (*on_ready)(void* user, oshot_capture_t image);
    void* user;
} oshot_mock__image_request_t;

typedef struct
{
    int32_t       id;
    oshot_place_t where;
    char*         label;
    void (*fn)(void* user, int32_t action);
    void* user;
} oshot_mock__action_t;

typedef struct
{
    char* label;
    char* text; /* NULL for a click */
} oshot_mock__input_t;

struct oshot_mock
{
    oshot_host_t          host;
    const oshot_plugin_t* plugin;
    void*                 state;
    bool                  loaded;
    bool                  in_capture;
    bool                  in_ui;
    int                   open_sections;
    oshot_mock__thread_t  ui_thread;
    oshot_mock__lock_t    lock; /* for what plugin threads may touch (logs, tasks, processes, reports) */
    char*                 data_dir;

    oshot_mock__entry_t* entries;
    size_t               n_entries, cap_entries;

    char* ocr_text;
    char* barcode_text;
    char* transformed; /* oshot_mock_transform()'s last result */

    int32_t  cap_w, cap_h;
    uint8_t* cap_rgba;
    float    sel_x, sel_y, sel_w, sel_h;

    oshot_mock__log_t* logs;
    size_t             n_logs, cap_logs;

    oshot_mock__task_t* tasks;
    size_t              n_tasks, cap_tasks;

    int32_t                proc_exit_code;
    char*                  proc_stdout;
    char*                  proc_stderr;
    char*                  last_command;
    int64_t                next_proc_id;
    oshot_mock__process_t* procs;
    size_t                 n_procs, cap_procs;

    oshot_mock__image_request_t* image_requests;
    size_t                       n_image_requests, cap_image_requests;

    oshot_mock__action_t* actions;
    size_t                n_actions, cap_actions;
    int32_t               next_action_id;

    oshot_mock__input_t* inputs; /* applied in the next render */
    size_t               n_inputs, cap_inputs;

    char*  drawn; /* the last frame's labels and text, one per line */
    size_t drawn_len, drawn_cap;

    char*    clipboard_text;
    int32_t  clipboard_w, clipboard_h;
    uint8_t* clipboard_rgba;
    char*    notification;
    char*    url;
    char*    progress;
    float    progress_fraction;
    char*    result;
    char*    result_copyable;
    bool     result_ok;
};

static inline void oshot_mock__replace(char** dst, const char* src)
{
    free(*dst);
    *dst = src ? oshot_mock__strdup(src) : NULL;
}

static inline const char* oshot_mock__level_name(oshot_log_level_t level)
{
    switch (level)
    {
        case OSHOT_LOG_DEBUG: return "debug";
        case OSHOT_LOG_INFO:  return "info";
        case OSHOT_LOG_WARN:  return "warning";
        default:              return "error";
    }
}

static inline void oshot_mock__log_n(oshot_mock_t* m, oshot_log_level_t level, const char* msg, size_t len)
{
    oshot_mock__lock(&m->lock);
    OSHOT_MOCK__PUSH(m->logs, m->n_logs, m->cap_logs, oshot_mock__log_t);
    m->logs[m->n_logs - 1].level = level;
    m->logs[m->n_logs - 1].msg   = oshot_mock__strndup(msg ? msg : "", msg ? len : 0);
    oshot_mock__unlock(&m->lock);
    if (level >= OSHOT_LOG_WARN)
        fprintf(stderr,
                "[%s] %s: %.*s\n",
                m->plugin && m->plugin->id ? m->plugin->id : "oshot mock",
                oshot_mock__level_name(level),
                (int)(msg ? len : 0),
                msg ? msg : "");
}

/* A message the mock itself logs, the way oshot logs a plugin's mistakes */
static inline void oshot_mock__complain(oshot_mock_t* m, const char* what)
{
    oshot_mock__log_n(m, OSHOT_LOG_ERROR, what, strlen(what));
}

static inline oshot_str_t oshot_mock__str_new(const char* str, size_t len)
{
    oshot_str_t r;
    char*       p = (char*)oshot_mock__alloc(len + 1);
    if (!p)
    {
        r.p   = NULL;
        r.len = 0;
        return r;
    }
    if (len && str)
        memcpy(p, str, len);
    p[len] = '\0';
    r.p    = p;
    r.len  = len;
    return r;
}

static inline void oshot_mock__str_free(oshot_str_t* str)
{
    if (!str)
        return;
    oshot_mock__free((void*)str->p);
    str->p   = NULL;
    str->len = 0;
}

/* Like fail() in oshot, the message goes to *err, or to the log without one */
static inline oshot_err_t oshot_mock__fail(oshot_mock_t* m, oshot_str_t* err, oshot_err_t code, const char* msg)
{
    if (err)
        *err = oshot_mock__str_new(msg, strlen(msg));
    else
        oshot_mock__complain(m, msg);
    return code;
}

static inline bool oshot_mock__on_ui_thread(oshot_mock_t* m, const char* fn)
{
    if (oshot_mock__same_thread(m->ui_thread, oshot_mock__this_thread()))
        return true;
    char msg[160];
    snprintf(msg, sizeof(msg), "%s() called outside the UI thread: use run_on_ui_thread()", fn);
    oshot_mock__complain(m, msg);
    return false;
}

static inline oshot_err_t oshot_mock__ui_only(oshot_mock_t* m, oshot_str_t* err, const char* fn)
{
    if (oshot_mock__same_thread(m->ui_thread, oshot_mock__this_thread()))
        return OSHOT_OK;
    char msg[160];
    snprintf(msg, sizeof(msg), "%s() can only be called from the UI thread", fn);
    return oshot_mock__fail(m, err, OSHOT_ERR_THREAD, msg);
}

static inline oshot_mock__entry_t* oshot_mock__find(oshot_mock_t* m, oshot_store_t store, const char* key)
{
    if (!key)
        return NULL;
    for (size_t i = 0; i < m->n_entries; ++i)
        if (m->entries[i].store == store && strcmp(m->entries[i].key, key) == 0)
            return &m->entries[i];
    return NULL;
}

static inline void oshot_mock__clear_value(oshot_value_t* v)
{
    if (v->kind == OSHOT_VAL_STRING)
        free((void*)v->s.p);
    v->kind = OSHOT_VAL_INT64;
    v->i    = 0;
}

static inline void oshot_mock__clear_entry(oshot_mock__entry_t* e)
{
    oshot_mock__clear_value(&e->value);
    for (size_t i = 0; i < e->n_array; ++i)
        oshot_mock__clear_value(&e->array[i]);
    free(e->array);
    e->array   = NULL;
    e->n_array = 0;
}

static inline oshot_value_t oshot_mock__copy_value(oshot_value_t v)
{
    if (v.kind == OSHOT_VAL_STRING)
        v.s.p = oshot_mock__strndup(v.s.p ? v.s.p : "", v.s.p ? v.s.len : 0);
    return v;
}

/* The entry for key, emptied, created if missing */
static inline oshot_mock__entry_t* oshot_mock__entry(oshot_mock_t* m, oshot_store_t store, const char* key)
{
    oshot_mock__entry_t* e = oshot_mock__find(m, store, key);
    if (e)
    {
        oshot_mock__clear_entry(e);
        return e;
    }
    OSHOT_MOCK__PUSH(m->entries, m->n_entries, m->cap_entries, oshot_mock__entry_t);
    e             = &m->entries[m->n_entries - 1];
    e->store      = store;
    e->key        = oshot_mock__strdup(key);
    e->value.kind = OSHOT_VAL_INT64;
    return e;
}

static inline oshot_value_t oshot_mock__lookup(oshot_mock_t* m,
                                               oshot_store_t store,
                                               const char*   key,
                                               oshot_value_t fallback)
{
    const oshot_mock__entry_t* e = oshot_mock__find(m, store, key);
    return e && !e->array ? e->value : fallback;
}

static inline char** oshot_mock__buffer(oshot_mock_t* m, const char* id)
{
    if (id && strcmp(id, OSHOT_TEXT_OCR) == 0)
        return &m->ocr_text;
    if (id && strcmp(id, OSHOT_TEXT_BARCODE) == 0)
        return &m->barcode_text;
    return NULL;
}

/* The visible part of a widget label, before "##" */
static inline size_t oshot_mock__visible_len(oshot_str_t label)
{
    for (size_t i = 0; i + 1 < label.len; ++i)
        if (label.p[i] == '#' && label.p[i + 1] == '#')
            return i;
    return label.len;
}

static inline bool oshot_mock__label_is(oshot_str_t label, const char* name)
{
    const size_t n = strlen(name);
    if (label.len == n && memcmp(label.p, name, n) == 0)
        return true;
    return oshot_mock__visible_len(label) == n && memcmp(label.p, name, n) == 0;
}

static inline void oshot_mock__draw(oshot_mock_t* m, const char* kind, oshot_str_t text)
{
    const size_t kind_len = strlen(kind);
    const size_t len      = text.p ? text.len : 0;
    const size_t need     = m->drawn_len + kind_len + 1 + len + 2;
    if (need > m->drawn_cap)
    {
        m->drawn_cap = need * 2;
        char* grown  = (char*)realloc(m->drawn, m->drawn_cap);
        if (!grown)
            abort();
        m->drawn = grown;
    }
    memcpy(m->drawn + m->drawn_len, kind, kind_len);
    m->drawn_len += kind_len;
    m->drawn[m->drawn_len++] = ' ';
    if (len)
        memcpy(m->drawn + m->drawn_len, text.p, len);
    m->drawn_len += len;
    m->drawn[m->drawn_len++] = '\n';
    m->drawn[m->drawn_len]   = '\0';
}

/* Records a widget with a label, and complains about a label used twice in a frame */
static inline bool oshot_mock__widget(oshot_mock_t* m, const char* kind, oshot_str_t label)
{
    if (!oshot_mock__on_ui_thread(m, kind))
        return false;
    if (!m->in_ui)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "ui_%s() called outside render() and render_preferences()", kind);
        oshot_mock__complain(m, msg);
        return false;
    }

    /* The frame is "<kind> <label>\n" lines, so look for the same label after any kind */
    if (m->drawn && label.p)
    {
        for (const char* line = m->drawn; *line;)
        {
            const char* end   = strchr(line, '\n');
            const char* space = strchr(line, ' ');
            if (!end)
                break;
            if (space && space < end && (size_t)(end - space - 1) == label.len &&
                memcmp(space + 1, label.p, label.len) == 0 && label.len > 0 && strncmp(line, "text ", 5) != 0 &&
                strncmp(line, "tooltip ", 8) != 0 && strncmp(line, "separator ", 10) != 0 &&
                strncmp(line, "progress ", 9) != 0)
            {
                char msg[200];
                snprintf(msg,
                         sizeof(msg),
                         "the label \"%.*s\" is used twice in one frame: add \"##suffix\" to one",
                         (int)label.len,
                         label.p);
                oshot_mock__complain(m, msg);
                break;
            }
            line = end + 1;
        }
    }
    oshot_mock__draw(m, kind, label);
    return true;
}

/* The click or text queued for this label, removed from the queue, or NULL */
static inline oshot_mock__input_t* oshot_mock__take_input(oshot_mock_t* m, oshot_str_t label, oshot_mock__input_t* out)
{
    for (size_t i = 0; i < m->n_inputs; ++i)
    {
        if (oshot_mock__label_is(label, m->inputs[i].label))
        {
            *out = m->inputs[i];
            memmove((void*)&m->inputs[i], (void*)&m->inputs[i + 1], (m->n_inputs - i - 1) * sizeof(*m->inputs));
            --m->n_inputs;
            return out;
        }
    }
    return NULL;
}

/* ----- Host functions ----- */

#define OSHOT_MOCK__SELF(ctx) ((oshot_mock_t*)(void*)(ctx))

static inline void oshot_mock__value_array_free(oshot_value_t* arr, size_t n)
{
    for (size_t i = 0; arr && i < n; ++i)
        if (arr[i].kind == OSHOT_VAL_STRING)
            oshot_mock__str_free(&arr[i].s);
}

static inline void oshot_mock__h_log(oshot_ctx_t* ctx, oshot_log_level_t level, oshot_str_t msg)
{
    oshot_mock__log_n(OSHOT_MOCK__SELF(ctx), level, msg.p, msg.len);
}

static inline oshot_str_t oshot_mock__h_get_data_dir(oshot_ctx_t* ctx)
{
    const char* dir = OSHOT_MOCK__SELF(ctx)->data_dir;
    return oshot_mock__str_new(dir, strlen(dir));
}

static inline void oshot_mock__h_run_on_ui_thread(oshot_ctx_t* ctx, void (*fn)(void* user), void* user)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!fn)
        return;
    oshot_mock__lock(&m->lock);
    OSHOT_MOCK__PUSH(m->tasks, m->n_tasks, m->cap_tasks, oshot_mock__task_t);
    m->tasks[m->n_tasks - 1].fn   = fn;
    m->tasks[m->n_tasks - 1].user = user;
    oshot_mock__unlock(&m->lock);
}

static inline oshot_str_t oshot_mock__h_get_string(oshot_ctx_t*  ctx,
                                                   oshot_store_t store,
                                                   const char*   key,
                                                   oshot_str_t   fallback)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (oshot_mock__on_ui_thread(m, "get_string"))
    {
        const oshot_mock__entry_t* e = oshot_mock__find(m, store, key);
        if (e && !e->array && e->value.kind == OSHOT_VAL_STRING)
            return oshot_mock__str_new(e->value.s.p, e->value.s.len);
    }
    return oshot_mock__str_new(fallback.p, fallback.p ? fallback.len : 0);
}

static inline bool oshot_mock__h_get_bool(oshot_ctx_t* ctx, oshot_store_t store, const char* key, bool fallback)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "get_bool"))
        return fallback;
    const oshot_mock__entry_t* e = oshot_mock__find(m, store, key);
    return e && !e->array && e->value.kind == OSHOT_VAL_BOOL ? e->value.b : fallback;
}

static inline int64_t oshot_mock__h_get_int64(oshot_ctx_t* ctx, oshot_store_t store, const char* key, int64_t fallback)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "get_int64"))
        return fallback;
    const oshot_mock__entry_t* e = oshot_mock__find(m, store, key);
    if (!e || e->array)
        return fallback;
    if (e->value.kind == OSHOT_VAL_INT64)
        return e->value.i;
    return e->value.kind == OSHOT_VAL_DOUBLE ? (int64_t)e->value.d : fallback;
}

static inline double oshot_mock__h_get_double(oshot_ctx_t* ctx, oshot_store_t store, const char* key, double fallback)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "get_double"))
        return fallback;
    const oshot_mock__entry_t* e = oshot_mock__find(m, store, key);
    if (!e || e->array)
        return fallback;
    if (e->value.kind == OSHOT_VAL_DOUBLE)
        return e->value.d;
    return e->value.kind == OSHOT_VAL_INT64 ? (double)e->value.i : fallback;
}

/* A copy of v whose string is a host string, as get_value() and get_array() return */
static inline oshot_value_t oshot_mock__host_value(oshot_value_t v)
{
    if (v.kind == OSHOT_VAL_STRING)
        v.s = oshot_mock__str_new(v.s.p, v.s.p ? v.s.len : 0);
    return v;
}

static inline size_t oshot_mock__h_get_array(oshot_ctx_t*   ctx,
                                             oshot_store_t  store,
                                             const char*    key,
                                             oshot_value_t* out,
                                             size_t         max)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "get_array") || !out)
        return 0;
    const oshot_mock__entry_t* e = oshot_mock__find(m, store, key);
    if (!e || !e->array)
        return 0;
    size_t n = 0;
    for (; n < e->n_array && n < max; ++n)
        out[n] = oshot_mock__host_value(e->array[n]);
    return n;
}

static inline oshot_value_t oshot_mock__h_get_value(oshot_ctx_t*  ctx,
                                                    oshot_store_t store,
                                                    const char*   key,
                                                    oshot_value_t fallback)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "get_value"))
        return oshot_mock__host_value(fallback);
    return oshot_mock__host_value(oshot_mock__lookup(m, store, key, fallback));
}

static inline void oshot_mock__h_set_value(oshot_ctx_t*         ctx,
                                           oshot_store_t        store,
                                           const char*          key,
                                           const oshot_value_t* val)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "set_value") || !key || !val)
        return;
    oshot_mock__entry(m, store, key)->value = oshot_mock__copy_value(*val);
}

static inline void oshot_mock__h_set_string(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_str_t val)
{
    oshot_value_t v;
    v.kind = OSHOT_VAL_STRING;
    v.s    = val;
    oshot_mock__h_set_value(ctx, store, key, &v);
}

static inline void oshot_mock__h_set_bool(oshot_ctx_t* ctx, oshot_store_t store, const char* key, bool val)
{
    oshot_value_t v;
    v.kind = OSHOT_VAL_BOOL;
    v.b    = val;
    oshot_mock__h_set_value(ctx, store, key, &v);
}

static inline void oshot_mock__h_set_int64(oshot_ctx_t* ctx, oshot_store_t store, const char* key, int64_t val)
{
    oshot_value_t v;
    v.kind = OSHOT_VAL_INT64;
    v.i    = val;
    oshot_mock__h_set_value(ctx, store, key, &v);
}

static inline void oshot_mock__h_set_double(oshot_ctx_t* ctx, oshot_store_t store, const char* key, double val)
{
    oshot_value_t v;
    v.kind = OSHOT_VAL_DOUBLE;
    v.d    = val;
    oshot_mock__h_set_value(ctx, store, key, &v);
}

static inline bool oshot_mock__h_get_text(oshot_ctx_t* ctx, const char* id, oshot_str_t* out)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "get_text") || !out)
        return false;
    char** buf = oshot_mock__buffer(m, id);
    if (!buf)
        return false;
    *out = oshot_mock__str_new(*buf ? *buf : "", *buf ? strlen(*buf) : 0);
    return true;
}

static inline bool oshot_mock__h_set_text(oshot_ctx_t* ctx, const char* id, oshot_str_t value)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "set_text"))
        return false;
    char** buf = oshot_mock__buffer(m, id);
    if (!buf)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "set_text(): unknown text buffer '%s'", id ? id : "(null)");
        oshot_mock__complain(m, msg);
        return false;
    }
    free(*buf);
    *buf = oshot_mock__strndup(value.p ? value.p : "", value.p ? value.len : 0);
    return true;
}

static inline oshot_capture_t oshot_mock__copy_image(int32_t w, int32_t h, const uint8_t* rgba)
{
    oshot_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    if (!rgba || w <= 0 || h <= 0)
        return cap;
    const size_t size = (size_t)w * (size_t)h * 4;
    cap.rgba          = (uint8_t*)oshot_mock__alloc(size);
    if (!cap.rgba)
        return cap;
    memcpy(cap.rgba, rgba, size);
    cap.w = w;
    cap.h = h;
    return cap;
}

static inline oshot_capture_t oshot_mock__h_get_capture(oshot_ctx_t* ctx)
{
    oshot_mock_t*   m = OSHOT_MOCK__SELF(ctx);
    oshot_capture_t none;
    memset(&none, 0, sizeof(none));
    if (!oshot_mock__on_ui_thread(m, "get_capture") || !m->in_capture)
        return none;
    return oshot_mock__copy_image(m->cap_w, m->cap_h, m->cap_rgba);
}

static inline void oshot_mock__h_capture_free(oshot_capture_t* cap)
{
    if (!cap)
        return;
    oshot_mock__free(cap->rgba);
    cap->rgba = NULL;
    cap->w = cap->h = 0;
}

static inline void oshot_mock__h_ui_text(oshot_ctx_t* ctx, oshot_str_t text, uint32_t flags)
{
    (void)flags;
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__on_ui_thread(m, "ui_text"))
        return;
    if (m->in_ui)
        oshot_mock__draw(m, "text", text);
    else
        oshot_mock__complain(m, "ui_text() called outside render() and render_preferences()");
}

static inline bool oshot_mock__h_ui_button(oshot_ctx_t* ctx, oshot_str_t label)
{
    oshot_mock_t*       m = OSHOT_MOCK__SELF(ctx);
    oshot_mock__input_t in;
    if (!oshot_mock__widget(m, "button", label) || !oshot_mock__take_input(m, label, &in))
        return false;
    free(in.label);
    free(in.text);
    return true;
}

static inline bool oshot_mock__h_ui_checkbox(oshot_ctx_t* ctx, oshot_str_t label, bool* value)
{
    oshot_mock_t*       m = OSHOT_MOCK__SELF(ctx);
    oshot_mock__input_t in;
    if (!oshot_mock__widget(m, "checkbox", label) || !value || !oshot_mock__take_input(m, label, &in))
        return false;
    free(in.label);
    free(in.text);
    *value = !*value;
    return true;
}

static inline bool oshot_mock__h_ui_input_text(oshot_ctx_t* ctx, oshot_str_t label, oshot_str_t* value, uint32_t flags)
{
    (void)flags;
    oshot_mock_t*       m = OSHOT_MOCK__SELF(ctx);
    oshot_mock__input_t in;
    if (!oshot_mock__widget(m, "input", label) || !value || !oshot_mock__take_input(m, label, &in))
        return false;
    oshot_mock__str_free(value);
    *value = oshot_mock__str_new(in.text ? in.text : "", in.text ? strlen(in.text) : 0);
    free(in.label);
    free(in.text);
    return true;
}

static inline bool oshot_mock__h_ui_combo(oshot_ctx_t*       ctx,
                                          oshot_str_t        label,
                                          int32_t*           current,
                                          const oshot_str_t* items,
                                          size_t             n)
{
    (void)current, (void)items, (void)n;
    oshot_mock__widget(OSHOT_MOCK__SELF(ctx), "combo", label);
    return false;
}

static inline bool oshot_mock__h_ui_slider_int(oshot_ctx_t* ctx,
                                               oshot_str_t  label,
                                               int64_t*     value,
                                               int64_t      min,
                                               int64_t      max)
{
    (void)value, (void)min, (void)max;
    oshot_mock__widget(OSHOT_MOCK__SELF(ctx), "slider", label);
    return false;
}

static inline bool oshot_mock__h_ui_slider_float(oshot_ctx_t* ctx,
                                                 oshot_str_t  label,
                                                 double*      value,
                                                 double       min,
                                                 double       max)
{
    (void)value, (void)min, (void)max;
    oshot_mock__widget(OSHOT_MOCK__SELF(ctx), "slider", label);
    return false;
}

static inline bool oshot_mock__h_ui_color(oshot_ctx_t* ctx, oshot_str_t label, float rgba[4])
{
    (void)rgba;
    oshot_mock__widget(OSHOT_MOCK__SELF(ctx), "color", label);
    return false;
}

static inline void oshot_mock__h_ui_progress(oshot_ctx_t* ctx, float fraction, oshot_str_t overlay)
{
    (void)fraction;
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (m->in_ui && oshot_mock__on_ui_thread(m, "ui_progress"))
        oshot_mock__draw(m, "progress", overlay);
}

static inline void oshot_mock__h_ui_separator(oshot_ctx_t* ctx, oshot_str_t label)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (m->in_ui && oshot_mock__on_ui_thread(m, "ui_separator"))
        oshot_mock__draw(m, "separator", label);
}

static inline void oshot_mock__h_ui_same_line(oshot_ctx_t* ctx)
{
    (void)ctx;
}

static inline void oshot_mock__h_ui_tooltip(oshot_ctx_t* ctx, oshot_str_t text)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (m->in_ui && oshot_mock__on_ui_thread(m, "ui_tooltip"))
        oshot_mock__draw(m, "tooltip", text);
}

/* Sections are always open, so the test sees everything inside them */
static inline bool oshot_mock__h_ui_begin_section(oshot_ctx_t* ctx, oshot_str_t label)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__widget(m, "section", label))
        return false;
    ++m->open_sections;
    return true;
}

static inline void oshot_mock__h_ui_end_section(oshot_ctx_t* ctx)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!m->in_ui)
        return;
    if (m->open_sections == 0)
        oshot_mock__complain(m, "ui_end_section() without ui_begin_section()");
    else
        --m->open_sections;
}

static inline void* oshot_mock__h_mem_alloc(size_t size)
{
    return oshot_mock__alloc(size);
}

static inline void oshot_mock__h_mem_free(void* ptr)
{
    oshot_mock__free(ptr);
}

static inline oshot_err_t oshot_mock__h_copy_text(oshot_ctx_t* ctx, oshot_str_t text, oshot_str_t* err)
{
    oshot_mock_t*     m = OSHOT_MOCK__SELF(ctx);
    const oshot_err_t r = oshot_mock__ui_only(m, err, "copy_text");
    if (r != OSHOT_OK)
        return r;
    free(m->clipboard_text);
    m->clipboard_text = oshot_mock__strndup(text.p ? text.p : "", text.p ? text.len : 0);
    return OSHOT_OK;
}

static inline oshot_err_t oshot_mock__h_copy_image(oshot_ctx_t* ctx, const oshot_capture_t* image, oshot_str_t* err)
{
    oshot_mock_t*     m = OSHOT_MOCK__SELF(ctx);
    const oshot_err_t r = oshot_mock__ui_only(m, err, "copy_image");
    if (r != OSHOT_OK)
        return r;
    if (!image || !image->rgba || image->w <= 0 || image->h <= 0)
        return oshot_mock__fail(m, err, OSHOT_ERR_INVALID, "copy_image(): the image is empty");
    const size_t size = (size_t)image->w * (size_t)image->h * 4;
    free(m->clipboard_rgba);
    m->clipboard_rgba = (uint8_t*)malloc(size);
    if (!m->clipboard_rgba)
        abort();
    memcpy(m->clipboard_rgba, image->rgba, size);
    m->clipboard_w = image->w;
    m->clipboard_h = image->h;
    return OSHOT_OK;
}

static inline oshot_err_t oshot_mock__h_notify(oshot_ctx_t* ctx, oshot_str_t title, oshot_str_t body, oshot_str_t* err)
{
    oshot_mock_t*     m = OSHOT_MOCK__SELF(ctx);
    const oshot_err_t r = oshot_mock__ui_only(m, err, "notify");
    if (r != OSHOT_OK)
        return r;
    free(m->notification);
    m->notification = (char*)malloc(title.len + body.len + 3);
    if (!m->notification)
        abort();
    snprintf(m->notification,
             title.len + body.len + 3,
             "%.*s: %.*s",
             (int)title.len,
             title.p ? title.p : "",
             (int)body.len,
             body.p ? body.p : "");
    return OSHOT_OK;
}

static inline oshot_err_t oshot_mock__h_open_url(oshot_ctx_t* ctx, oshot_str_t url, oshot_str_t* err)
{
    oshot_mock_t*     m = OSHOT_MOCK__SELF(ctx);
    const oshot_err_t r = oshot_mock__ui_only(m, err, "open_url");
    if (r != OSHOT_OK)
        return r;
    if (!m->in_capture)
        return oshot_mock__fail(m, err, OSHOT_ERR_UNSUPPORTED, "open_url(): no overlay is open");

    /* scheme ":" ... */
    size_t i = 0;
    while (i < url.len && url.p && (isalnum((unsigned char)url.p[i]) || strchr("+-.", url.p[i])))
        ++i;
    if (i == 0 || i >= url.len || url.p[i] != ':' || !isalpha((unsigned char)url.p[0]))
        return oshot_mock__fail(m, err, OSHOT_ERR_INVALID, "open_url(): the URL must start with a scheme");

    free(m->url);
    m->url = oshot_mock__strndup(url.p, url.len);
    return OSHOT_OK;
}

static inline oshot_err_t oshot_mock__h_run_process(oshot_ctx_t*       ctx,
                                                    const oshot_str_t* argv,
                                                    size_t             argc,
                                                    oshot_str_t        cwd,
                                                    void (*on_exit)(void* user, const oshot_process_result_t* result),
                                                    void*        user,
                                                    int64_t*     id,
                                                    oshot_str_t* err)
{
    (void)cwd;
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!argv || argc == 0 || !argv[0].p || argv[0].len == 0)
        return oshot_mock__fail(m, err, OSHOT_ERR_INVALID, "run_process(): no program given");

    size_t len = 0;
    for (size_t i = 0; i < argc; ++i)
        len += (argv[i].p ? argv[i].len : 0) + 1;
    char* command = (char*)malloc(len + 1);
    if (!command)
        abort();
    size_t at = 0;
    for (size_t i = 0; i < argc; ++i)
    {
        if (i)
            command[at++] = ' ';
        if (argv[i].p && argv[i].len)
            memcpy(command + at, argv[i].p, argv[i].len);
        at += argv[i].p ? argv[i].len : 0;
    }
    command[at] = '\0';

    oshot_mock__lock(&m->lock);
    free(m->last_command);
    m->last_command = command;
    OSHOT_MOCK__PUSH(m->procs, m->n_procs, m->cap_procs, oshot_mock__process_t);
    oshot_mock__process_t* p = &m->procs[m->n_procs - 1];
    p->id                    = ++m->next_proc_id;
    p->on_exit               = on_exit;
    p->user                  = user;
    if (id)
        *id = p->id;
    oshot_mock__unlock(&m->lock);
    return OSHOT_OK;
}

static inline oshot_err_t oshot_mock__h_kill_process(oshot_ctx_t* ctx, int64_t id, oshot_str_t* err)
{
    oshot_mock_t* m     = OSHOT_MOCK__SELF(ctx);
    bool          found = false;
    oshot_mock__lock(&m->lock);
    for (size_t i = 0; i < m->n_procs; ++i)
        if (m->procs[i].id == id && !m->procs[i].killed)
            found = m->procs[i].killed = true;
    oshot_mock__unlock(&m->lock);
    return found ? OSHOT_OK : oshot_mock__fail(m, err, OSHOT_ERR_NOT_FOUND, "kill_process(): no such running process");
}

static inline oshot_err_t oshot_mock__h_request_final_image(oshot_ctx_t* ctx,
                                                            void (*on_ready)(void* user, oshot_capture_t image),
                                                            void*        user,
                                                            oshot_str_t* err)
{
    oshot_mock_t*     m = OSHOT_MOCK__SELF(ctx);
    const oshot_err_t r = oshot_mock__ui_only(m, err, "request_final_image");
    if (r != OSHOT_OK)
        return r;
    if (!on_ready)
        return oshot_mock__fail(m, err, OSHOT_ERR_INVALID, "request_final_image(): on_ready is NULL");
    if (!m->in_capture)
        return oshot_mock__fail(m, err, OSHOT_ERR_UNSUPPORTED, "request_final_image(): no overlay is open");
    OSHOT_MOCK__PUSH(m->image_requests, m->n_image_requests, m->cap_image_requests, oshot_mock__image_request_t);
    m->image_requests[m->n_image_requests - 1].on_ready = on_ready;
    m->image_requests[m->n_image_requests - 1].user     = user;
    return OSHOT_OK;
}

static inline oshot_err_t oshot_mock__h_register_action(oshot_ctx_t*           ctx,
                                                        oshot_place_t          where,
                                                        oshot_str_t            label,
                                                        const oshot_capture_t* icon,
                                                        void (*fn)(void* user, int32_t action),
                                                        void*        user,
                                                        int32_t*     id,
                                                        oshot_str_t* err)
{
    oshot_mock_t*     m = OSHOT_MOCK__SELF(ctx);
    const oshot_err_t r = oshot_mock__ui_only(m, err, "register_action");
    if (r != OSHOT_OK)
        return r;
    if (!fn || !label.p || label.len == 0)
        return oshot_mock__fail(m, err, OSHOT_ERR_INVALID, "register_action(): needs a label and a function");
    if (where != OSHOT_PLACE_TOOLBAR && where != OSHOT_PLACE_TEXT_TOOLS)
        return oshot_mock__fail(m, err, OSHOT_ERR_INVALID, "register_action(): unknown place");
    if (icon && (!icon->rgba || icon->w <= 0 || icon->h <= 0 || icon->w > OSHOT_ACTION_ICON_MAX ||
                 icon->h > OSHOT_ACTION_ICON_MAX))
        return oshot_mock__fail(m, err, OSHOT_ERR_INVALID, "register_action(): the icon is empty or too big");

    OSHOT_MOCK__PUSH(m->actions, m->n_actions, m->cap_actions, oshot_mock__action_t);
    oshot_mock__action_t* a = &m->actions[m->n_actions - 1];
    a->id                   = ++m->next_action_id;
    a->where                = where;
    a->label                = oshot_mock__strndup(label.p, label.len);
    a->fn                   = fn;
    a->user                 = user;
    if (id)
        *id = a->id;
    return OSHOT_OK;
}

static inline oshot_err_t oshot_mock__h_unregister_action(oshot_ctx_t* ctx, int32_t action, oshot_str_t* err)
{
    oshot_mock_t*     m = OSHOT_MOCK__SELF(ctx);
    const oshot_err_t r = oshot_mock__ui_only(m, err, "unregister_action");
    if (r != OSHOT_OK)
        return r;
    for (size_t i = 0; i < m->n_actions; ++i)
    {
        if (m->actions[i].id == action)
        {
            free(m->actions[i].label);
            memmove((void*)&m->actions[i], (void*)&m->actions[i + 1], (m->n_actions - i - 1) * sizeof(*m->actions));
            --m->n_actions;
            return OSHOT_OK;
        }
    }
    return oshot_mock__fail(m, err, OSHOT_ERR_NOT_FOUND, "unregister_action(): no such action");
}

static inline bool oshot_mock__action_exists(oshot_mock_t* m, int32_t action)
{
    /* Actions are only changed on the UI thread, which this may not be, but that's good enough for a test */
    for (size_t i = 0; i < m->n_actions; ++i)
        if (m->actions[i].id == action)
            return true;
    return action == 0;
}

static inline oshot_err_t oshot_mock__h_report_progress(oshot_ctx_t* ctx,
                                                        int32_t      action,
                                                        float        fraction,
                                                        oshot_str_t  text,
                                                        oshot_str_t* err)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__action_exists(m, action))
        return oshot_mock__fail(m, err, OSHOT_ERR_NOT_FOUND, "report_progress(): no such action");
    oshot_mock__lock(&m->lock);
    free(m->progress);
    m->progress          = oshot_mock__strndup(text.p ? text.p : "", text.p ? text.len : 0);
    m->progress_fraction = fraction;
    oshot_mock__unlock(&m->lock);
    return OSHOT_OK;
}

static inline oshot_err_t oshot_mock__h_report_result(oshot_ctx_t* ctx,
                                                      int32_t      action,
                                                      bool         ok,
                                                      oshot_str_t  message,
                                                      oshot_str_t  copyable,
                                                      oshot_str_t* err)
{
    oshot_mock_t* m = OSHOT_MOCK__SELF(ctx);
    if (!oshot_mock__action_exists(m, action))
        return oshot_mock__fail(m, err, OSHOT_ERR_NOT_FOUND, "report_result(): no such action");
    oshot_mock__lock(&m->lock);
    free(m->progress);
    m->progress = NULL;
    free(m->result);
    free(m->result_copyable);
    m->result          = oshot_mock__strndup(message.p ? message.p : "", message.p ? message.len : 0);
    m->result_copyable = oshot_mock__strndup(copyable.p ? copyable.p : "", copyable.p ? copyable.len : 0);
    m->result_ok       = ok;
    oshot_mock__unlock(&m->lock);
    return OSHOT_OK;
}

static inline void oshot_mock__h_request_redraw(oshot_ctx_t* ctx)
{
    (void)ctx;  // the mock draws only when the test calls oshot_mock_render()
}

#undef OSHOT_MOCK__SELF

static inline void oshot_mock__drop_pending(oshot_mock_t* m)
{
    oshot_mock__lock(&m->lock);
    m->n_tasks = 0;
    m->n_procs = 0;
    oshot_mock__unlock(&m->lock);
    m->n_image_requests = 0;
    for (size_t i = 0; i < m->n_actions; ++i)
        free(m->actions[i].label);
    m->n_actions = 0;
}

static inline bool oshot_mock__valid_id(const char* id)
{
    if (!id)
        return false;
    const size_t len = strlen(id);
    if (len == 0 || len > 64 || !strchr(id, '.'))
        return false;
    for (size_t i = 0; i < len; ++i)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || strchr("-_.", id[i])))
            return false;
    return true;
}

static inline bool oshot_mock__check_settings(oshot_mock_t* m, const oshot_plugin_t* p)
{
    if (p->n_settings && !p->settings)
    {
        oshot_mock__complain(m, "n_settings is set but settings is NULL");
        return false;
    }
    for (size_t i = 0; i < p->n_settings; ++i)
    {
        const oshot_setting_t* s = &p->settings[i];
        oshot_value_kind_t     want;
        switch (s->kind)
        {
            case OSHOT_SETTING_BOOL:   want = OSHOT_VAL_BOOL; break;
            case OSHOT_SETTING_INT:    want = OSHOT_VAL_INT64; break;
            case OSHOT_SETTING_FLOAT:  want = OSHOT_VAL_DOUBLE; break;
            case OSHOT_SETTING_STRING:
            case OSHOT_SETTING_CHOICE: want = OSHOT_VAL_STRING; break;
            default:                   oshot_mock__complain(m, "a setting has an unknown kind"); return false;
        }
        char msg[200];
        if (!s->key || !*s->key || !s->label || !*s->label)
            snprintf(msg, sizeof(msg), "setting %u has no key or no label", (unsigned)i);
        else if (s->fallback.kind != want)
            snprintf(msg, sizeof(msg), "setting '%s': its fallback isn't of the kind the setting needs", s->key);
        else if (s->kind == OSHOT_SETTING_CHOICE && (!s->choices || s->n_choices == 0))
            snprintf(msg, sizeof(msg), "setting '%s': a CHOICE needs choices", s->key);
        else
            continue;
        oshot_mock__complain(m, msg);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------
 * Setting up
 * ---------------------------------------------------------------- */

/**
 * Creates a mock oshot. The calling thread becomes oshot's UI thread.
 * @return The mock, freed with oshot_mock_free()
 **/
static inline oshot_mock_t* oshot_mock_new(void)
{
    oshot_mock_t* m = (oshot_mock_t*)calloc(1, sizeof(oshot_mock_t));
    if (!m)
        abort();
#if defined(_WIN32)
    InitializeSRWLock(&m->lock);
#else
    pthread_mutex_init(&m->lock, NULL);
#endif
    m->ui_thread = oshot_mock__this_thread();
    m->data_dir  = oshot_mock__strdup(".");

    oshot_host_t* h        = &m->host;
    h->struct_size         = (uint32_t)sizeof(oshot_host_t);
    h->abi_version         = OSHOT_API_VERSION;
    h->oshot_version       = "mock";
    h->str_new             = oshot_mock__str_new;
    h->str_free            = oshot_mock__str_free;
    h->value_array_free    = oshot_mock__value_array_free;
    h->log                 = oshot_mock__h_log;
    h->get_data_dir        = oshot_mock__h_get_data_dir;
    h->run_on_ui_thread    = oshot_mock__h_run_on_ui_thread;
    h->get_string          = oshot_mock__h_get_string;
    h->get_bool            = oshot_mock__h_get_bool;
    h->get_int64           = oshot_mock__h_get_int64;
    h->get_double          = oshot_mock__h_get_double;
    h->get_array           = oshot_mock__h_get_array;
    h->get_value           = oshot_mock__h_get_value;
    h->set_string          = oshot_mock__h_set_string;
    h->set_bool            = oshot_mock__h_set_bool;
    h->set_int64           = oshot_mock__h_set_int64;
    h->set_double          = oshot_mock__h_set_double;
    h->set_value           = oshot_mock__h_set_value;
    h->get_text            = oshot_mock__h_get_text;
    h->set_text            = oshot_mock__h_set_text;
    h->get_capture         = oshot_mock__h_get_capture;
    h->capture_free        = oshot_mock__h_capture_free;
    h->ui_text             = oshot_mock__h_ui_text;
    h->ui_button           = oshot_mock__h_ui_button;
    h->ui_checkbox         = oshot_mock__h_ui_checkbox;
    h->ui_input_text       = oshot_mock__h_ui_input_text;
    h->ui_combo            = oshot_mock__h_ui_combo;
    h->ui_slider_int       = oshot_mock__h_ui_slider_int;
    h->ui_slider_float     = oshot_mock__h_ui_slider_float;
    h->ui_color            = oshot_mock__h_ui_color;
    h->ui_progress         = oshot_mock__h_ui_progress;
    h->ui_separator        = oshot_mock__h_ui_separator;
    h->ui_same_line        = oshot_mock__h_ui_same_line;
    h->ui_tooltip          = oshot_mock__h_ui_tooltip;
    h->ui_begin_section    = oshot_mock__h_ui_begin_section;
    h->ui_end_section      = oshot_mock__h_ui_end_section;
    h->mem_alloc           = oshot_mock__h_mem_alloc;
    h->mem_free            = oshot_mock__h_mem_free;
    h->copy_text           = oshot_mock__h_copy_text;
    h->copy_image          = oshot_mock__h_copy_image;
    h->notify              = oshot_mock__h_notify;
    h->open_url            = oshot_mock__h_open_url;
    h->run_process         = oshot_mock__h_run_process;
    h->kill_process        = oshot_mock__h_kill_process;
    h->request_final_image = oshot_mock__h_request_final_image;
    h->register_action     = oshot_mock__h_register_action;
    h->unregister_action   = oshot_mock__h_unregister_action;
    h->report_progress     = oshot_mock__h_report_progress;
    h->report_result       = oshot_mock__h_report_result;
    h->request_redraw      = oshot_mock__h_request_redraw;
    return m;
}

/**
 * Pretends to be an older oshot whose host table ends earlier, to test the
 * plugin's OSHOT_HOST_HAS() checks. Call it before oshot_mock_load().
 * @param m The mock
 * @param struct_size offsetof(oshot_host_t, <first missing field>)
 **/
static inline void oshot_mock_set_host_size(oshot_mock_t* m, uint32_t struct_size)
{
    m->host.struct_size = struct_size;
}

/**
 * Sets what get_data_dir() returns.
 * @param m The mock
 * @param dir The folder, or NULL for "." (the default)
 **/
static inline void oshot_mock_set_data_dir(oshot_mock_t* m, const char* dir)
{
    oshot_mock__replace(&m->data_dir, dir ? dir : ".");
}

/**
 * Sets the screenshot inside the selection, which get_capture() copies and
 * request_final_image() delivers.
 * @param m The mock
 * @param w Its width in pixels
 * @param h Its height in pixels
 * @param rgba w * h * 4 bytes of RGBA, copied, or NULL for no image
 **/
static inline void oshot_mock_set_capture(oshot_mock_t* m, int32_t w, int32_t h, const uint8_t* rgba)
{
    free(m->cap_rgba);
    m->cap_rgba = NULL;
    m->cap_w = m->cap_h = 0;
    if (!rgba || w <= 0 || h <= 0)
        return;
    const size_t size = (size_t)w * (size_t)h * 4;
    m->cap_rgba       = (uint8_t*)malloc(size);
    if (!m->cap_rgba)
        abort();
    memcpy(m->cap_rgba, rgba, size);
    m->cap_w = w;
    m->cap_h = h;
}

/**
 * Sets one of oshot's text outputs, as if oshot had shown it.
 * @param m The mock
 * @param buffer_id OSHOT_TEXT_OCR or OSHOT_TEXT_BARCODE
 * @param text The text, copied
 **/
static inline void oshot_mock_set_text(oshot_mock_t* m, const char* buffer_id, const char* text)
{
    char** buf = oshot_mock__buffer(m, buffer_id);
    if (buf)
        oshot_mock__replace(buf, text);
}

/**
 * Sets a value in the plugin's config.toml or in oshot's cache, as if the
 * user had saved it. oshot_mock_set_string(), _bool(), _int64() and
 * _double() do the same for one type.
 * @param m The mock
 * @param store OSHOT_STORE_CONFIG or OSHOT_STORE_CACHE
 * @param key The key, with dots for nested tables
 * @param value The value, copied
 **/
static inline void oshot_mock_set_value(oshot_mock_t* m, oshot_store_t store, const char* key, oshot_value_t value)
{
    oshot_mock__entry(m, store, key)->value = oshot_mock__copy_value(value);
}

/**
 * oshot_mock_set_value() for a string.
 * @param m The mock
 * @param store OSHOT_STORE_CONFIG or OSHOT_STORE_CACHE
 * @param key The key
 * @param value The string, copied
 **/
static inline void oshot_mock_set_string(oshot_mock_t* m, oshot_store_t store, const char* key, const char* value)
{
    oshot_value_t v;
    v.kind = OSHOT_VAL_STRING;
    v.s    = oshot_str_borrow(value);
    oshot_mock_set_value(m, store, key, v);
}

/**
 * oshot_mock_set_value() for a bool.
 * @param m The mock
 * @param store OSHOT_STORE_CONFIG or OSHOT_STORE_CACHE
 * @param key The key
 * @param value The value
 **/
static inline void oshot_mock_set_bool(oshot_mock_t* m, oshot_store_t store, const char* key, bool value)
{
    oshot_value_t v;
    v.kind = OSHOT_VAL_BOOL;
    v.b    = value;
    oshot_mock_set_value(m, store, key, v);
}

/**
 * oshot_mock_set_value() for an integer.
 * @param m The mock
 * @param store OSHOT_STORE_CONFIG or OSHOT_STORE_CACHE
 * @param key The key
 * @param value The value
 **/
static inline void oshot_mock_set_int64(oshot_mock_t* m, oshot_store_t store, const char* key, int64_t value)
{
    oshot_value_t v;
    v.kind = OSHOT_VAL_INT64;
    v.i    = value;
    oshot_mock_set_value(m, store, key, v);
}

/**
 * oshot_mock_set_value() for a float.
 * @param m The mock
 * @param store OSHOT_STORE_CONFIG or OSHOT_STORE_CACHE
 * @param key The key
 * @param value The value
 **/
static inline void oshot_mock_set_double(oshot_mock_t* m, oshot_store_t store, const char* key, double value)
{
    oshot_value_t v;
    v.kind = OSHOT_VAL_DOUBLE;
    v.d    = value;
    oshot_mock_set_value(m, store, key, v);
}

/**
 * Sets an array value, for get_array().
 * @param m The mock
 * @param store OSHOT_STORE_CONFIG or OSHOT_STORE_CACHE
 * @param key The key
 * @param values n values, copied
 * @param n How many
 **/
static inline void oshot_mock_set_array(oshot_mock_t*        m,
                                        oshot_store_t        store,
                                        const char*          key,
                                        const oshot_value_t* values,
                                        size_t               n)
{
    oshot_mock__entry_t* e = oshot_mock__entry(m, store, key);
    e->array               = (oshot_value_t*)calloc(n ? n : 1, sizeof(oshot_value_t));
    if (!e->array)
        abort();
    for (size_t i = 0; i < n; ++i)
        e->array[i] = oshot_mock__copy_value(values[i]);
    e->n_array = n;
}

/**
 * Reads a value back, e.g. one the plugin saved.
 * @param m The mock
 * @param store OSHOT_STORE_CONFIG or OSHOT_STORE_CACHE
 * @param key The key
 * @param out Set to the value. A string is borrowed until the value changes
 * @return false if the key is missing (or holds an array)
 **/
static inline bool oshot_mock_get_value(oshot_mock_t* m, oshot_store_t store, const char* key, oshot_value_t* out)
{
    const oshot_mock__entry_t* e = oshot_mock__find(m, store, key);
    if (!e || e->array)
        return false;
    *out = e->value;
    return true;
}

/**
 * Sets how every later run_process() ends. oshot_mock_run_ui_tasks() passes
 * it to on_exit. By default a process exits with 0 and no output.
 * @param m The mock
 * @param exit_code Its exit code
 * @param out What it wrote to stdout, or NULL
 * @param err What it wrote to stderr, or NULL
 **/
static inline void oshot_mock_set_process_result(oshot_mock_t* m, int32_t exit_code, const char* out, const char* err)
{
    oshot_mock__lock(&m->lock);
    m->proc_exit_code = exit_code;
    oshot_mock__replace(&m->proc_stdout, out);
    oshot_mock__replace(&m->proc_stderr, err);
    oshot_mock__unlock(&m->lock);
}

/* ------------------------------------------------------------------
 * The plugin's lifecycle
 * ---------------------------------------------------------------- */

/**
 * Checks the descriptor as oshot does before loading it (ID, ABI, settings),
 * then calls init().
 * @param m The mock, with no plugin loaded yet
 * @param plugin What the plugin's oshot_plugin_entry() returns
 * @return false, with the reason on stderr, if oshot would refuse the plugin
 *         or init() returned false
 **/
static inline bool oshot_mock_load(oshot_mock_t* m, const oshot_plugin_t* plugin)
{
    if (m->loaded)
    {
        oshot_mock__complain(m, "oshot_mock_load(): a plugin is already loaded in this mock");
        return false;
    }
    m->plugin = plugin;
    if (!plugin)
    {
        oshot_mock__complain(m, "oshot_plugin_entry() returned NULL");
        return false;
    }

    const char* problem = NULL;
    if (plugin->struct_size < offsetof(oshot_plugin_t, settings))
        problem = "struct_size is too small: set it to sizeof(oshot_plugin_t)";
    else if (plugin->abi_version != OSHOT_API_VERSION)
        problem = "abi_version isn't OSHOT_API_VERSION";
    else if (plugin->imgui_version != OSHOT_NO_IMGUI && plugin->imgui_version != OSHOT_IMGUI_VERSION)
        problem = "imgui_version is neither OSHOT_NO_IMGUI nor OSHOT_IMGUI_VERSION";
    else if (!oshot_mock__valid_id(plugin->id))
        problem = "the ID isn't a valid reverse-domain ID (lowercase letters, digits, '-', '_', a '.', at most 64)";
    else if (!plugin->name || !*plugin->name)
        problem = "the plugin has no name";
    if (problem)
    {
        oshot_mock__complain(m, problem);
        return false;
    }
    if (OSHOT_MOCK__HAS(plugin, n_settings) && !oshot_mock__check_settings(m, plugin))
        return false;

    m->state = NULL;
    if (plugin->init && !plugin->init(&m->host, (oshot_ctx_t*)(void*)m, &m->state))
    {
        oshot_mock__complain(m, "init() returned false");
        return false;
    }
    m->loaded = true;
    return true;
}

/**
 * The host table the plugin got in init(), to call host functions from a test.
 * @param m The mock
 **/
static inline const oshot_host_t* oshot_mock_host(oshot_mock_t* m)
{
    return &m->host;
}

/**
 * The context the plugin got in init(), to pass to host functions.
 * @param m The mock
 **/
static inline oshot_ctx_t* oshot_mock_ctx(oshot_mock_t* m)
{
    return (oshot_ctx_t*)(void*)m;
}

/**
 * The plugin's state as init() set it, for tests that look inside it.
 * @param m The mock
 **/
static inline void* oshot_mock_state(oshot_mock_t* m)
{
    return m->state;
}

/**
 * Ends the capture if one is open, then calls destroy(). Tasks and processes
 * still pending are dropped.
 * @param m The mock
 **/
static inline void oshot_mock_unload(oshot_mock_t* m)
{
    if (!m->loaded)
        return;
    if (m->in_capture && OSHOT_MOCK__HAS(m->plugin, on_capture_end) && m->plugin->on_capture_end)
        m->plugin->on_capture_end(m->state);
    m->in_capture = false;
    /* The plugin's threads may still report on its actions until destroy() stops them */
    if (m->plugin->destroy)
        m->plugin->destroy(m->state);
    oshot_mock__drop_pending(m);
    m->loaded = false;
    m->state  = NULL;
}

/**
 * Unloads the plugin and frees the mock.
 * @param m The mock, or NULL
 **/
static inline void oshot_mock_free(oshot_mock_t* m)
{
    if (!m)
        return;
    oshot_mock_unload(m);
    for (size_t i = 0; i < m->n_entries; ++i)
    {
        oshot_mock__clear_entry(&m->entries[i]);
        free(m->entries[i].key);
    }
    for (size_t i = 0; i < m->n_logs; ++i)
        free(m->logs[i].msg);
    for (size_t i = 0; i < m->n_inputs; ++i)
    {
        free(m->inputs[i].label);
        free(m->inputs[i].text);
    }
    free(m->entries);
    free(m->logs);
    free(m->tasks);
    free(m->procs);
    free(m->image_requests);
    free(m->actions);
    free(m->inputs);
    free(m->drawn);
    free(m->data_dir);
    free(m->ocr_text);
    free(m->barcode_text);
    free(m->transformed);
    free(m->cap_rgba);
    free(m->proc_stdout);
    free(m->proc_stderr);
    free(m->last_command);
    free(m->clipboard_text);
    free(m->clipboard_rgba);
    free(m->notification);
    free(m->url);
    free(m->progress);
    free(m->result);
    free(m->result_copyable);
#if !defined(_WIN32)
    pthread_mutex_destroy(&m->lock);
#endif
    free(m);
}

/**
 * Counts the host strings, mem_alloc() blocks and images from the host that
 * weren't freed yet, over every mock.
 * @return 0 after oshot_mock_free() if the plugin freed everything it got
 **/
static inline size_t oshot_mock_live_allocations(void)
{
    oshot_mock__lock(&oshot_mock__alloc_lock);
    const size_t n = oshot_mock__alloc_count;
    oshot_mock__unlock(&oshot_mock__alloc_lock);
    return n;
}

/* ------------------------------------------------------------------
 * Events, in the order oshot sends them
 * ---------------------------------------------------------------- */

/**
 * A capture starts and the overlay opens. Clears the text outputs and the
 * selection, then calls on_capture_start().
 * @param m The mock
 **/
static inline void oshot_mock_capture_start(oshot_mock_t* m)
{
    if (m->in_capture)
        oshot_mock__complain(m, "oshot_mock_capture_start(): a capture is already open");
    m->in_capture = true;
    oshot_mock__replace(&m->ocr_text, NULL);
    oshot_mock__replace(&m->barcode_text, NULL);
    m->sel_x = m->sel_y = m->sel_w = m->sel_h = 0;
    if (m->loaded && OSHOT_MOCK__HAS(m->plugin, on_capture_start) && m->plugin->on_capture_start)
        m->plugin->on_capture_start(m->state);
}

/**
 * The overlay closes. Final images still requested arrive with no image, as
 * in oshot, then on_capture_end() is called.
 * @param m The mock
 **/
static inline void oshot_mock_capture_end(oshot_mock_t* m)
{
    if (!m->in_capture)
        return;
    m->in_capture = false;
    for (size_t i = 0; i < m->n_image_requests; ++i)
    {
        oshot_capture_t none;
        memset(&none, 0, sizeof(none));
        m->image_requests[i].on_ready(m->image_requests[i].user, none);
    }
    m->n_image_requests = 0;
    if (m->loaded && OSHOT_MOCK__HAS(m->plugin, on_capture_end) && m->plugin->on_capture_end)
        m->plugin->on_capture_end(m->state);
}

/**
 * The person finished drawing the selection. Calls on_selection_changed().
 * @param m The mock
 * @param x Left edge, in the whole screenshot's pixels
 * @param y Top edge
 * @param w Width
 * @param h Height
 **/
static inline void oshot_mock_select(oshot_mock_t* m, float x, float y, float w, float h)
{
    m->sel_x = x, m->sel_y = y, m->sel_w = w, m->sel_h = h;
    if (m->loaded && OSHOT_MOCK__HAS(m->plugin, on_selection_changed) && m->plugin->on_selection_changed)
        m->plugin->on_selection_changed(m->state, x, y, w, h);
}

/**
 * Runs text through the plugin's transform_text(), as oshot does after a scan.
 * @param m The mock
 * @param buffer_id OSHOT_TEXT_OCR or OSHOT_TEXT_BARCODE
 * @param in The text
 * @return The plugin's text, or `in` if it passed it through. Valid until the
 *         next call
 **/
static inline const char* oshot_mock_transform(oshot_mock_t* m, const char* buffer_id, const char* in)
{
    free(m->transformed);
    m->transformed = NULL;
    oshot_str_t out;
    out.p   = NULL;
    out.len = 0;
    if (m->loaded && OSHOT_MOCK__HAS(m->plugin, transform_text) && m->plugin->transform_text &&
        m->plugin->transform_text(m->state, buffer_id, oshot_str_borrow(in ? in : ""), &out))
    {
        if (!out.p && out.len)
            oshot_mock__complain(m, "transform_text() returned true with a NULL string");
        m->transformed = oshot_mock__strndup(out.p ? out.p : "", out.p ? out.len : 0);
        oshot_mock__str_free(&out);
    }
    else
    {
        if (out.p)
            oshot_mock__complain(m, "transform_text() returned false but set *out (which oshot leaks)");
        m->transformed = oshot_mock__strdup(in);
    }
    return m->transformed;
}

/**
 * OCR read some text. Like oshot, the mock runs transform_text(), shows the
 * result in OSHOT_TEXT_OCR and calls on_ocr_done().
 * @param m The mock
 * @param text The text, empty (or NULL) when OCR read nothing
 * @param words The words, or NULL
 * @param n_words How many
 * @return OSHOT_TEXT_OCR as on_ocr_done() left it
 **/
static inline const char* oshot_mock_ocr(oshot_mock_t*           m,
                                         const char*             text,
                                         const oshot_ocr_word_t* words,
                                         size_t                  n_words)
{
    const bool empty = !text || !*text;
    oshot_mock__replace(&m->ocr_text, empty ? "" : oshot_mock_transform(m, OSHOT_TEXT_OCR, text));

    oshot_ocr_result_t r;
    memset(&r, 0, sizeof(r));
    r.text       = oshot_str_borrow(m->ocr_text);
    r.confidence = -1;
    r.psm        = empty ? -1 : 3; /* PSM_AUTO */
    r.words      = n_words ? words : NULL;
    r.n_words    = n_words;
    if (n_words)
    {
        int64_t sum = 0;
        for (size_t i = 0; i < n_words; ++i)
            sum += words[i].confidence;
        r.confidence = (int32_t)(sum / (int64_t)n_words);
    }
    if (m->loaded && m->plugin->on_ocr_done)
    {
        /* on_ocr_done may call set_text(), which frees what r.text points to, so it gets a copy */
        char* shown = oshot_mock__strdup(m->ocr_text);
        r.text      = oshot_str_borrow(shown);
        m->plugin->on_ocr_done(m->state, &r);
        free(shown);
    }
    return m->ocr_text;
}

/**
 * ZBar decoded some codes. Like oshot, the mock joins their data (each
 * followed by an empty line), runs transform_text(), shows the result in
 * OSHOT_TEXT_BARCODE and calls on_barcodes_done().
 * @param m The mock
 * @param codes The codes, or NULL
 * @param n How many, 0 when ZBar found none
 * @return OSHOT_TEXT_BARCODE afterwards
 **/
static inline const char* oshot_mock_barcodes(oshot_mock_t* m, const oshot_barcode_t* codes, size_t n)
{
    size_t len = 0;
    for (size_t i = 0; i < n; ++i)
        len += codes[i].data.len + 2;
    char* joined = (char*)malloc(len + 1);
    if (!joined)
        abort();
    size_t at = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (codes[i].data.len)
            memcpy(joined + at, codes[i].data.p, codes[i].data.len);
        at += codes[i].data.len;
        joined[at++] = '\n';
        joined[at++] = '\n';
    }
    joined[at] = '\0';

    oshot_mock__replace(&m->barcode_text, n ? oshot_mock_transform(m, OSHOT_TEXT_BARCODE, joined) : "");
    free(joined);
    if (m->loaded && OSHOT_MOCK__HAS(m->plugin, on_barcodes_done) && m->plugin->on_barcodes_done)
        m->plugin->on_barcodes_done(m->state, n ? codes : NULL, n);
    return m->barcode_text;
}

/**
 * Runs what oshot runs at the start of a frame. That's the run_on_ui_thread()
 * tasks, the on_exit of processes run_process() started (with the result from
 * oshot_mock_set_process_result(), or killed) and the final images
 * request_final_image() asked for (the capture image). oshot_mock_render()
 * calls it too.
 * @param m The mock
 **/
static inline void oshot_mock_run_ui_tasks(oshot_mock_t* m)
{
    if (!m->loaded)
        return;

    oshot_mock__lock(&m->lock);
    const size_t        n_tasks = m->n_tasks;
    oshot_mock__task_t* tasks   = (oshot_mock__task_t*)malloc((n_tasks ? n_tasks : 1) * sizeof(*tasks));
    if (!tasks)
        abort();
    if (n_tasks)
        memcpy((void*)tasks, (void*)m->tasks, n_tasks * sizeof(*tasks));
    m->n_tasks = 0;

    const size_t           n_procs = m->n_procs;
    oshot_mock__process_t* procs   = (oshot_mock__process_t*)malloc((n_procs ? n_procs : 1) * sizeof(*procs));
    if (!procs)
        abort();
    if (n_procs)
        memcpy((void*)procs, (void*)m->procs, n_procs * sizeof(*procs));
    m->n_procs = 0;

    oshot_process_result_t res;
    memset(&res, 0, sizeof(res));
    char* out       = oshot_mock__strdup(m->proc_stdout);
    char* err       = oshot_mock__strdup(m->proc_stderr);
    res.exit_code   = m->proc_exit_code;
    res.stdout_data = oshot_str_borrow(out);
    res.stderr_data = oshot_str_borrow(err);
    oshot_mock__unlock(&m->lock);

    for (size_t i = 0; i < n_tasks; ++i)
        tasks[i].fn(tasks[i].user);
    for (size_t i = 0; i < n_procs; ++i)
    {
        if (!procs[i].on_exit)
            continue;
        oshot_process_result_t r = res;
        r.id                     = procs[i].id;
        if (procs[i].killed)
        {
            r.killed      = true;
            r.exit_code   = -1;
            r.stdout_data = oshot_str_borrow("");
            r.stderr_data = oshot_str_borrow("");
        }
        procs[i].on_exit(procs[i].user, &r);
    }
    free(tasks);
    free(procs);
    free(out);
    free(err);

    const size_t                 n_req = m->n_image_requests;
    oshot_mock__image_request_t* req =
        (oshot_mock__image_request_t*)malloc((n_req ? n_req : 1) * sizeof(oshot_mock__image_request_t));
    if (!req)
        abort();
    if (n_req)
        memcpy((void*)req, (void*)m->image_requests, n_req * sizeof(*req));
    m->n_image_requests = 0;
    for (size_t i = 0; i < n_req; ++i)
        req[i].on_ready(req[i].user, oshot_mock__copy_image(m->cap_w, m->cap_h, m->cap_rgba));
    free(req);
}

/**
 * Makes the next frame press the button, toggle the checkbox or open the
 * section with this label. Several can be queued for one frame.
 * @param m The mock
 * @param label The label, with or without its "##suffix"
 **/
static inline void oshot_mock_click(oshot_mock_t* m, const char* label)
{
    OSHOT_MOCK__PUSH(m->inputs, m->n_inputs, m->cap_inputs, oshot_mock__input_t);
    m->inputs[m->n_inputs - 1].label = oshot_mock__strdup(label);
}

/**
 * Makes the next frame type into the text field with this label, replacing
 * its value.
 * @param m The mock
 * @param label The label, with or without its "##suffix"
 * @param text The new value
 **/
static inline void oshot_mock_type(oshot_mock_t* m, const char* label, const char* text)
{
    OSHOT_MOCK__PUSH(m->inputs, m->n_inputs, m->cap_inputs, oshot_mock__input_t);
    m->inputs[m->n_inputs - 1].label = oshot_mock__strdup(label);
    m->inputs[m->n_inputs - 1].text  = oshot_mock__strdup(text);
}

static inline void oshot_mock__begin_frame(oshot_mock_t* m)
{
    oshot_mock_run_ui_tasks(m);
    m->drawn_len = 0;
    if (m->drawn)
        m->drawn[0] = '\0';
    m->open_sections = 0;
    m->in_ui         = true;
}

static inline void oshot_mock__end_frame(oshot_mock_t* m, const char* callback)
{
    m->in_ui = false;
    if (m->open_sections > 0)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s() left %d section(s) open: call ui_end_section()", callback, m->open_sections);
        oshot_mock__complain(m, msg);
    }
    for (size_t i = 0; i < m->n_inputs; ++i)
    {
        char msg[200];
        snprintf(msg, sizeof(msg), "nothing labeled \"%s\" was drawn to click or type into", m->inputs[i].label);
        oshot_mock__complain(m, msg);
        free(m->inputs[i].label);
        free(m->inputs[i].text);
    }
    m->n_inputs = 0;
}

/**
 * Draws one frame of the text tools window. Runs the UI tasks, then render().
 * @param m The mock
 **/
static inline void oshot_mock_render(oshot_mock_t* m)
{
    oshot_mock__begin_frame(m);
    if (m->loaded && m->plugin->render)
        m->plugin->render(m->state);
    oshot_mock__end_frame(m, "render");
}

/**
 * Draws one frame of the plugin's custom Preferences page.
 * @param m The mock
 * @return What render_preferences() returned (true for unsaved changes)
 **/
static inline bool oshot_mock_render_preferences(oshot_mock_t* m)
{
    bool dirty = false;
    oshot_mock__begin_frame(m);
    if (m->loaded && m->plugin->render_preferences)
        dirty = m->plugin->render_preferences(m->state);
    oshot_mock__end_frame(m, "render_preferences");
    return dirty;
}

/**
 * The person saves Preferences, so on_save_preferences() and then
 * on_settings_saved() are called. To change declarative settings, set them
 * with oshot_mock_set_*() first.
 * @param m The mock
 **/
static inline void oshot_mock_save_preferences(oshot_mock_t* m)
{
    if (!m->loaded)
        return;
    if (m->plugin->on_save_preferences)
        m->plugin->on_save_preferences(m->state);
    if (OSHOT_MOCK__HAS(m->plugin, on_settings_saved) && m->plugin->on_settings_saved)
        m->plugin->on_settings_saved(m->state);
}

/**
 * The person discards their changes in Preferences.
 * @param m The mock
 **/
static inline void oshot_mock_discard_preferences(oshot_mock_t* m)
{
    if (m->loaded && m->plugin->on_discard_preferences)
        m->plugin->on_discard_preferences(m->state);
}

/**
 * Clicks a toolbar button or menu entry the plugin registered, which runs its
 * function right away.
 * @param m The mock
 * @param label The action's label
 * @return false if the plugin has no action with that label
 **/
static inline bool oshot_mock_run_action(oshot_mock_t* m, const char* label)
{
    for (size_t i = 0; i < m->n_actions; ++i)
    {
        if (strcmp(m->actions[i].label, label) == 0)
        {
            const oshot_mock__action_t a = m->actions[i];
            a.fn(a.user, a.id);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------
 * What the plugin did
 * ---------------------------------------------------------------- */

/**
 * A text output as oshot shows it.
 * @param m The mock
 * @param buffer_id OSHOT_TEXT_OCR or OSHOT_TEXT_BARCODE
 * @return The text, "" if empty
 **/
static inline const char* oshot_mock_text(oshot_mock_t* m, const char* buffer_id)
{
    char** buf = oshot_mock__buffer(m, buffer_id);
    return buf && *buf ? *buf : "";
}

/**
 * Whether the last frame drew something containing `text`. See
 * oshot_mock_drawn() for what a frame holds.
 * @param m The mock
 * @param text What to look for, e.g. "button Copy" or "Selection: 4x3"
 **/
static inline bool oshot_mock_drew(oshot_mock_t* m, const char* text)
{
    return m->drawn && text && strstr(m->drawn, text) != NULL;
}

/**
 * The whole last frame, one "<kind> <label or text>" line per widget. The kind
 * is text, button, checkbox, input, combo, slider, color, progress, separator,
 * tooltip or section, and labels keep their "##suffix".
 * @param m The mock
 **/
static inline const char* oshot_mock_drawn(oshot_mock_t* m)
{
    return m->drawn ? m->drawn : "";
}

/**
 * Counts the messages the plugin logged, and the plugin's mistakes the mock
 * logged.
 * @param m The mock
 * @param level The lowest level counted
 **/
static inline size_t oshot_mock_log_count(oshot_mock_t* m, oshot_log_level_t level)
{
    size_t n = 0;
    oshot_mock__lock(&m->lock);
    for (size_t i = 0; i < m->n_logs; ++i)
        n += m->logs[i].level >= level;
    oshot_mock__unlock(&m->lock);
    return n;
}

/**
 * The last message logged at `level` or above.
 * @param m The mock
 * @param level The lowest level looked at
 * @return The message, valid until the mock is freed, or NULL
 **/
static inline const char* oshot_mock_last_log(oshot_mock_t* m, oshot_log_level_t level)
{
    const char* r = NULL;
    oshot_mock__lock(&m->lock);
    for (size_t i = m->n_logs; i-- > 0;)
    {
        if (m->logs[i].level >= level)
        {
            r = m->logs[i].msg;
            break;
        }
    }
    oshot_mock__unlock(&m->lock);
    return r;
}

/**
 * The last text copy_text() put on the clipboard, or NULL.
 * @param m The mock
 **/
static inline const char* oshot_mock_clipboard_text(oshot_mock_t* m)
{
    return m->clipboard_text;
}

/**
 * The last image copy_image() put on the clipboard (rgba NULL for none),
 * borrowed.
 * @param m The mock
 **/
static inline oshot_capture_t oshot_mock_clipboard_image(oshot_mock_t* m)
{
    oshot_capture_t r;
    r.w    = m->clipboard_w;
    r.h    = m->clipboard_h;
    r.rgba = m->clipboard_rgba;
    return r;
}

/**
 * The last notification as "title: body", or NULL.
 * @param m The mock
 **/
static inline const char* oshot_mock_notification(oshot_mock_t* m)
{
    return m->notification;
}

/**
 * The last URL open_url() opened, or NULL.
 * @param m The mock
 **/
static inline const char* oshot_mock_opened_url(oshot_mock_t* m)
{
    return m->url;
}

/**
 * The last command run_process() started, its arguments joined with spaces,
 * or NULL.
 * @param m The mock
 **/
static inline const char* oshot_mock_last_command(oshot_mock_t* m)
{
    return m->last_command;
}

/**
 * The status report_progress() shows, until report_result() replaces it.
 * @param m The mock
 * @param fraction Set to its fraction, unless NULL
 * @return The status, or NULL
 **/
static inline const char* oshot_mock_progress(oshot_mock_t* m, float* fraction)
{
    if (fraction)
        *fraction = m->progress_fraction;
    return m->progress;
}

/**
 * The last report_result().
 * @param m The mock
 * @param ok Set to whether it worked, unless NULL
 * @param copyable Set to its copyable text ("" for none), unless NULL
 * @return Its message, or NULL
 **/
static inline const char* oshot_mock_result(oshot_mock_t* m, bool* ok, const char** copyable)
{
    if (ok)
        *ok = m->result_ok;
    if (copyable)
        *copyable = m->result_copyable;
    return m->result;
}

/**
 * Counts the actions the plugin has registered at one place.
 * @param m The mock
 * @param where OSHOT_PLACE_TOOLBAR or OSHOT_PLACE_TEXT_TOOLS
 **/
static inline size_t oshot_mock_action_count(oshot_mock_t* m, oshot_place_t where)
{
    size_t n = 0;
    for (size_t i = 0; i < m->n_actions; ++i)
        n += m->actions[i].where == where;
    return n;
}

#ifdef __cplusplus
}
#endif
