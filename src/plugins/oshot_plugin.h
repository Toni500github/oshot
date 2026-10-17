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
 * oshot plugin API.
 *
 * A plugin is a shared library (.so, .dll, .dylib) that exports one function,
 * oshot_plugin_entry(), returning a static oshot_plugin_t. oshot calls into
 * the plugin through that descriptor, and the plugin calls back into oshot
 * only through the oshot_host_t table it gets in init(). Plugins don't link
 * against oshot. A plugin that draws UI uses Dear ImGui through cimgui
 * (oshot/cimgui.h): on Linux and the BSDs it doesn't need to link anything,
 * on macOS it links with `-undefined dynamic_lookup`, and on Windows it links
 * the libcimgui import library from the SDK (sdk/lib).
 *
 * Threads: callbacks in oshot_plugin_t run on oshot's UI thread. Host
 * functions marked "any thread" may be called from threads the plugin
 * creates; the others only from inside a callback. To get back to the UI
 * thread from a worker, use run_on_ui_thread(). A plugin must stop its own
 * threads in destroy().
 **/

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define OSHOT_API_VERSION 1u

/**
 * IMGUI_VERSION_NUM of the Dear ImGui that oshot and oshot/cimgui.h are built
 * from. cimgui exposes ImGui's structs, so a plugin that uses it only works
 * with the exact same ImGui version.
 **/
#define OSHOT_IMGUI_VERSION 19284u

/* Text buffers that get_text()/set_text() accept */
#define OSHOT_TEXT_OCR     "ocr_output"
#define OSHOT_TEXT_BARCODE "barcode_output"

#if defined(OSHOT_HOST)
#  define OSHOT_PLUGIN_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#  define OSHOT_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define OSHOT_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/**
 * Identifies the calling plugin in every host call. Opaque, valid from
 * init() until destroy() returns.
 **/
typedef struct oshot_ctx oshot_ctx_t;

/* Level of a host->log() message */
typedef enum
{
    OSHOT_LOG_DEBUG,
    OSHOT_LOG_INFO,
    OSHOT_LOG_WARN,
    OSHOT_LOG_ERROR,
} oshot_log_level_t;

/* The type of an oshot_value_t */
typedef enum
{
    OSHOT_VAL_STRING,
    OSHOT_VAL_INT64,
    OSHOT_VAL_BOOL,
    OSHOT_VAL_DOUBLE
} oshot_value_kind_t;

/**
 * Where a value is stored. Both are private to the plugin.
 * CONFIG: the plugin's config.toml, meant for settings. Saved when the person
 *         saves the Preferences window and when the plugin is unloaded.
 * CACHE:  oshot's cache file, for state that doesn't need to survive a
 *         deleted cache (last used option, window sizes, ...).
 **/
typedef enum
{
    OSHOT_STORE_CONFIG,
    OSHOT_STORE_CACHE
} oshot_store_t;

/**
 * A string with an explicit length. Strings the host returns are owned by the
 * caller (free them with host->str_free) and are also NUL-terminated.
 **/
typedef struct
{
    const char* p;
    size_t      len;
} oshot_str_t;

/* A config or cache value of any type: read the union member that `kind` says */
typedef struct
{
    oshot_value_kind_t kind;
    union
    {
        oshot_str_t s;
        int64_t     i;
        bool        b;
        double      d;
    };
} oshot_value_t;

/* RGBA8 image, w * h * 4 bytes, owned by the caller (host->capture_free) */
typedef struct
{
    int32_t  w;
    int32_t  h;
    uint8_t* rgba;
} oshot_capture_t;

/* What on_ocr_done() gets */
typedef struct
{
    oshot_str_t text;       /* borrowed: only valid during on_ocr_done() */
    int32_t     confidence; /* 0-100, or -1 if unavailable */
    int32_t     psm;        /* tesseract::PageSegMode value */
} oshot_ocr_result_t;

/* ------------------------------------------------------------------
 * Host functions, passed to init(). Fields are only ever appended:
 * check host->struct_size before using one added in a later version.
 * ---------------------------------------------------------------- */
typedef struct
{
    uint32_t    struct_size;   /* sizeof(oshot_host_t) in the running oshot */
    uint32_t    abi_version;   /* OSHOT_API_VERSION of the running oshot */
    const char* oshot_version; /* e.g. "0.5.0" */

    /* Strings and arrays. Any thread. */
    oshot_str_t (*str_new)(const char* str, size_t len);
    void (*str_free)(oshot_str_t* str);
    void (*value_array_free)(oshot_value_t* arr, size_t n); /* frees the strings inside, not arr */

    /* Logging to oshot's log (and its Logs window). Any thread. */
    void (*log)(oshot_ctx_t* ctx, oshot_log_level_t level, oshot_str_t msg);

    /**
     * This plugin's own directory (where its library and config.toml are),
     * e.g. ~/.config/oshot/plugins/<id>. Any thread.
     **/
    oshot_str_t (*get_data_dir)(oshot_ctx_t* ctx);

    /**
     * Runs fn(user) on the UI thread, before the plugin is next drawn. Tasks
     * still queued when the plugin is unloaded are dropped. Any thread.
     **/
    void (*run_on_ui_thread)(oshot_ctx_t* ctx, void (*fn)(void* user), void* user);

    /**
     * Typed values in the plugin's config or cache. Keys may contain dots
     * for nested tables ("ui.font-size"). UI thread only.
     **/
    oshot_str_t (*get_string)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_str_t fallback);
    bool (*get_bool)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, bool fallback);
    int64_t (*get_int64)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, int64_t fallback);
    double (*get_double)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, double fallback);
    /**
     * Copies up to `max` elements of an array into out[0..max). Elements that
     * aren't a string, integer, bool or float are skipped. Returns how many
     * were written; free them with value_array_free(out, n).
     **/
    size_t (*get_array)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_value_t* out, size_t max);
    /**
     * Returns the value in its own type, or fallback (copied) if the key is
     * missing. A string result must be freed with str_free.
     **/
    oshot_value_t (*get_value)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_value_t fallback);

    void (*set_string)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_str_t val);
    void (*set_bool)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, bool val);
    void (*set_int64)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, int64_t val);
    void (*set_double)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, double val);
    void (*set_value)(oshot_ctx_t* ctx, oshot_store_t store, const char* key, const oshot_value_t* val);

    /**
     * oshot's text outputs (OSHOT_TEXT_OCR, OSHOT_TEXT_BARCODE). get_text
     * returns false for an unknown id; set_text returns false and logs an
     * error for one. UI thread only.
     **/
    bool (*get_text)(oshot_ctx_t* ctx, const char* id, oshot_str_t* out);
    bool (*set_text)(oshot_ctx_t* ctx, const char* id, oshot_str_t value);

    /**
     * A copy of the current selection, annotations included. rgba is NULL if
     * there's no selection. It copies the whole image: don't call it every
     * frame. UI thread only.
     **/
    oshot_capture_t (*get_capture)(oshot_ctx_t* ctx);
    void (*capture_free)(oshot_capture_t* cap);
} oshot_host_t;

/* ------------------------------------------------------------------
 * Plugin descriptor, returned by oshot_plugin_entry().
 * ---------------------------------------------------------------- */
typedef struct
{
    uint32_t struct_size;   /* sizeof(oshot_plugin_t) */
    uint32_t abi_version;   /* OSHOT_API_VERSION */
    uint32_t imgui_version; /* OSHOT_IMGUI_VERSION, or 0 if the plugin doesn't use ImGui
                               (then render and render_preferences must be NULL) */

    /**
     * Reverse-domain ID, e.g. "io.github.user.translate": lowercase letters,
     * digits, '-', '_' and at least one '.', at most 64 characters. Must match
     * the `id` in the plugin's oshot-plugin.toml.
     **/
    const char* id;
    const char* name; /* display name */

    /* All optional. */

    /**
     * Called once after loading. Store host and ctx in your state: every host
     * call needs them. Return false to refuse loading (log why first).
     **/
    bool (*init)(const oshot_host_t* host, oshot_ctx_t* ctx, void** state);

    /**
     * Called once before unloading, if init() succeeded: free `state` and stop
     * the plugin's threads.
     **/
    void (*destroy)(void* state);

    /**
     * Draws the plugin's section in the text tools window, every frame while
     * it's expanded.
     **/
    void (*render)(void* state);

    /* After oshot extracted text from the selection. */
    void (*on_ocr_done)(void* state, const oshot_ocr_result_t* result);

    /**
     * Draws the plugin's settings in Preferences > Plugins. Return true while
     * there are unsaved changes; oshot then calls on_save_preferences or
     * on_discard_preferences when the person saves or discards them.
     **/
    bool (*render_preferences)(void* state);
    void (*on_save_preferences)(void* state);
    void (*on_discard_preferences)(void* state);
} oshot_plugin_t;

/**
 * Wrap a NUL-terminated string without copying it. Don't str_free() the result.
 * @param s The string, or NULL for an empty one
 **/
static inline oshot_str_t oshot_str_borrow(const char* s)
{
    oshot_str_t r;
    r.p   = s;
    r.len = s ? strlen(s) : 0;
    return r;
}

/**
 * The one symbol a plugin exports. Must return a pointer to a static
 * descriptor, not one allocated per call. Define it with OSHOT_PLUGIN_EXPORT:
 *     OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void) { return &desc; }
 **/
OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void);

#ifdef __cplusplus
}
#endif
