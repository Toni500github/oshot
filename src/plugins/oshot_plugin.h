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
 * against oshot. A plugin draws its UI with the ui_* host functions and
 * declares its settings in `settings`, which oshot draws in Preferences.
 * For anything those can't draw, a plugin can use Dear ImGui through cimgui
 * (oshot/cimgui.h), but then it only loads in an oshot built with the same
 * ImGui (see OSHOT_IMGUI_VERSION). Such a plugin doesn't need to link anything
 * on Linux and the BSDs, links with `-undefined dynamic_lookup` on macOS, and
 * links the libcimgui import library from the SDK (sdk/lib) on Windows.
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
 * with the exact same ImGui version. oshot updates ImGui only in minor
 * releases (x.y.0). Plugins that use only the ui_* functions don't depend on it.
 **/
#define OSHOT_IMGUI_VERSION 19284u

/* Text buffers that get_text()/set_text() accept */
#define OSHOT_TEXT_OCR     "ocr_output"
#define OSHOT_TEXT_BARCODE "barcode_output"

/* Flags of host->ui_text() */
#define OSHOT_UI_DISABLED (1u << 0) /* greyed out */
#define OSHOT_UI_WRAPPED  (1u << 1) /* wrapped at the window's edge */

/* Flags of host->ui_input_text() and of OSHOT_SETTING_STRING settings */
#define OSHOT_UI_MULTILINE (1u << 2)
#define OSHOT_UI_PASSWORD  (1u << 3) /* shown as dots; ignored with MULTILINE */

/**
 * Whether the running oshot's host table has `field`, for functions added
 * after the plugin API's first version:
 *     if (!OSHOT_HOST_HAS(host, ui_button)) ...
 **/
#define OSHOT_HOST_HAS(host, field) \
    (offsetof(oshot_host_t, field) + sizeof(((oshot_host_t*)0)->field) <= (host)->struct_size)

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

/**
 * The type of a declarative setting, and the oshot_value_t kind of its
 * fallback and of the value in config.toml.
 **/
typedef enum
{
    OSHOT_SETTING_BOOL,   /* a checkbox; OSHOT_VAL_BOOL */
    OSHOT_SETTING_STRING, /* a text field; OSHOT_VAL_STRING */
    OSHOT_SETTING_INT,    /* a slider, or a number field without min/max; OSHOT_VAL_INT64 */
    OSHOT_SETTING_FLOAT,  /* the same, OSHOT_VAL_DOUBLE */
    OSHOT_SETTING_CHOICE  /* a combo box; OSHOT_VAL_STRING, one of `choices` */
} oshot_setting_kind_t;

/**
 * A setting oshot draws in Preferences > Plugins and saves to the plugin's
 * config.toml under `key`. The plugin reads it with get_bool, get_string,
 * get_int64 or get_double with the same key and fallback. Every pointer
 * must stay valid while the plugin is loaded (string literals, static arrays).
 * The layout is fixed: oshot reads the array with this struct's size.
 **/
typedef struct
{
    oshot_setting_kind_t kind;
    const char*          key; /* config.toml key; dots for nested tables */
    const char*          label;
    const char*          help;     /* tooltip, or NULL */
    oshot_value_t        fallback; /* the default, of the kind `kind` says */
    double               min, max; /* INT and FLOAT: a slider from min to max, or a plain field if min == max */
    const char* const*   choices;  /* CHOICE: the options, stored as the chosen string */
    size_t               n_choices;
    uint32_t             flags; /* STRING: OSHOT_UI_MULTILINE, OSHOT_UI_PASSWORD */
} oshot_setting_t;

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
     * A copy of the screenshot inside the selection, or of the whole screenshot
     * without one. Annotations aren't included. rgba is NULL if it couldn't be
     * copied. It copies the whole image: don't call it every frame. UI thread only.
     **/
    oshot_capture_t (*get_capture)(oshot_ctx_t* ctx);
    void (*capture_free)(oshot_capture_t* cap);

    /**
     * Widgets, drawn with oshot's style and fonts. Check OSHOT_HOST_HAS(host,
     * ui_text) before using them. UI thread only, and only inside render()
     * and render_preferences(): anywhere else they do nothing and log once.
     *
     * Labels and text are drawn as they are, never read as format strings.
     * A label must be unique within the plugin's section: add "##suffix" to
     * tell apart widgets with the same text ("Copy##url"), the part after
     * "##" isn't shown. Functions returning bool return true when the person
     * pressed or changed the widget in this frame.
     **/
    void (*ui_text)(oshot_ctx_t* ctx, oshot_str_t text, uint32_t flags); /* OSHOT_UI_DISABLED, OSHOT_UI_WRAPPED */
    bool (*ui_button)(oshot_ctx_t* ctx, oshot_str_t label);
    bool (*ui_checkbox)(oshot_ctx_t* ctx, oshot_str_t label, bool* value);
    /**
     * `value` must be a host string (from str_new, get_string, ...) or empty
     * ({ NULL, 0 }). When it changes, oshot frees it and stores a new host
     * string, which the plugin frees with str_free when done with it.
     * flags: OSHOT_UI_MULTILINE, OSHOT_UI_PASSWORD
     **/
    bool (*ui_input_text)(oshot_ctx_t* ctx, oshot_str_t label, oshot_str_t* value, uint32_t flags);
    /* A combo box with items[0..n); *current is the selected index */
    bool (*ui_combo)(oshot_ctx_t* ctx, oshot_str_t label, int32_t* current, const oshot_str_t* items, size_t n);
    bool (*ui_slider_int)(oshot_ctx_t* ctx, oshot_str_t label, int64_t* value, int64_t min, int64_t max);
    bool (*ui_slider_float)(oshot_ctx_t* ctx, oshot_str_t label, double* value, double min, double max);
    bool (*ui_color)(oshot_ctx_t* ctx, oshot_str_t label, float rgba[4]); /* components in 0-1 */
    /* fraction in 0-1; overlay is drawn on the bar, or the percentage if it's empty */
    void (*ui_progress)(oshot_ctx_t* ctx, float fraction, oshot_str_t overlay);
    void (*ui_separator)(oshot_ctx_t* ctx, oshot_str_t label); /* empty label = a plain line */
    void (*ui_same_line)(oshot_ctx_t* ctx);                    /* the next widget goes on the same line */
    void (*ui_tooltip)(oshot_ctx_t* ctx, oshot_str_t text);    /* shown when hovering the previous widget */
    /**
     * A collapsible section: returns true when it's open, then draw its
     * contents. Call ui_end_section() after it either way. Sections left open
     * when the callback returns are closed by oshot, and logged.
     **/
    bool (*ui_begin_section)(oshot_ctx_t* ctx, oshot_str_t label);
    void (*ui_end_section)(oshot_ctx_t* ctx);
} oshot_host_t;

/* ------------------------------------------------------------------
 * Plugin descriptor, returned by oshot_plugin_entry().
 * ---------------------------------------------------------------- */
typedef struct
{
    uint32_t struct_size;   /* sizeof(oshot_plugin_t) */
    uint32_t abi_version;   /* OSHOT_API_VERSION */
    uint32_t imgui_version; /* OSHOT_IMGUI_VERSION if the plugin calls cimgui, or 0 if it doesn't
                               (it may still draw with the ui_* host functions) */

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
     * Draws a custom page in Preferences > Plugins, under the declarative
     * `settings`; most plugins only need those. Return true while there are
     * unsaved changes; oshot then calls on_save_preferences or
     * on_discard_preferences when the person saves or discards them.
     **/
    bool (*render_preferences)(void* state);
    void (*on_save_preferences)(void* state);
    void (*on_discard_preferences)(void* state);

    /* Added after the first version: oshot checks struct_size before reading them. */

    /**
     * Settings oshot draws, saves and discards in Preferences > Plugins: a
     * static array of n_settings, in the order they're shown. NULL for none.
     **/
    const oshot_setting_t* settings;
    size_t                 n_settings;

    /**
     * After the person saved this plugin's preferences (settings and
     * render_preferences) to config.toml: re-read the values that matter.
     **/
    void (*on_settings_saved)(void* state);
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
