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

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define OSHOT_API_VERSION 1u

#define OCR_OUTPUT  "ocr_output"
#define ZBAR_OUTPUT "barcode_output"

typedef enum
{
    OSHOT_CAP_NONE    = 0,
    OSHOT_CAP_NETWORK = 1 << 0,
    OSHOT_CAP_FS      = 1 << 1,
} OSCapabilities;

typedef enum
{
    OSHOT_LOG_DEBUG,
    OSHOT_LOG_INFO,
    OSHOT_LOG_WARN,
    OSHOT_LOG_ERROR,
} OSLogLevel;

typedef enum
{
    OSHOT_VAL_STRING,
    OSHOT_VAL_INT64,
    OSHOT_VAL_BOOL,
    OSHOT_VAL_DOUBLE
} OSValueKind;

typedef struct
{
    int32_t  w;
    int32_t  h;
    uint8_t* rgba;
} oshot_capture_t;

typedef struct
{
    const char* p;
    size_t      len;
} oshot_str_t;

typedef struct
{
    OSValueKind kind;
    union
    {
        oshot_str_t s;
        int64_t     i;
        bool        b;
        double      d;
    };
} oshot_value_t;

typedef struct
{
    oshot_str_t text;
    int32_t     confidence;  // 0-100, or -1 if unavailable
    int32_t     psm;         // tesseract::PageSegMode value
} oshot_ocr_result_t;

/* ------------------------------------------------------------------
 * Plugin descriptor.
 * One per plugin, returned by the single exported symbol below
 * ---------------------------------------------------------------- */
typedef struct
{
    uint32_t    abi_version;
    uint32_t    capabilities;  // OR of oshot_capabilities_t, informational only
    const char* name;          // display name, doesn't need to be unique
    const char* id;            // reverse-domain, e.g. "com.example.myplugin"

    void* (*init)(void);  // NOLINT(modernize-redundant-void-arg): this header is also C
    void (*destroy)(void* state);
    void (*render)(void* state);

    void (*on_ocr_done)(void* state, const oshot_ocr_result_t* result);

    bool (*render_preferences)(void* state);
    void (*on_save_preferences)(void* state);
    void (*on_discard_preferences)(void* state);
} oshot_plugin_t;

/* ------------------------------------------------------------------
 * ABI / identity
 * ------------------------------------------------------------------ */
uint32_t oshot_get_abi_version(void);

/* Returns an owned oshot_str_t (caller must oshot_str_free it) pointing
 * to this plugin's private data directory, e.g.
 * ~/.config/oshot/plugins/<id>/
 * The host guarantees this directory exists by the time any plugin
 * callback can run. Created at plugin-load time, before init(). */
oshot_str_t oshot_get_plugin_data_dir(void);

/* ------------------------------------------------------------------
 * oshot_str_t lifecycle
 * ------------------------------------------------------------------ */
oshot_str_t oshot_str_new(const char* str, size_t n);
void        oshot_str_free(oshot_str_t* str);

static inline oshot_str_t oshot_str_borrow(const char* s)
{
    return (oshot_str_t){ .p = s, .len = s ? strlen(s) : 0 };
}

// Only frees OSHOT_VAL_STRING members
void oshot_value_array_free(oshot_value_t* arr, size_t n);

/* ------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------ */
void oshot_log(OSLogLevel lvl, oshot_str_t str);
void oshot_debug(oshot_str_t str);  // oshot_log(DEBUG, str);
void oshot_error(oshot_str_t str);  // oshot_log(ERROR, str);
void oshot_warn(oshot_str_t str);   // oshot_log(WARN, str);
void oshot_info(oshot_str_t str);   // oshot_log(INFO, str);

/* ------------------------------------------------------------------
 * Config (optional convenience helper)
 *
 * These functions persist plugin config as TOML, in a file the host
 * manages on your behalf. You are not required to use them, if you
 * prefer JSON, YAML, or anything else, ignore this section entirely
 * and read/write your own file under the path returned by
 * oshot_get_plugin_data_dir(), using whatever library you like.
 * ------------------------------------------------------------------ */
oshot_str_t oshot_config_get_string(const char* key, oshot_str_t fallback);
bool        oshot_config_get_bool(const char* key, bool fallback);
int64_t     oshot_config_get_int64(const char* key, int64_t fallback);
double      oshot_config_get_double(const char* key, double fallback);
size_t      oshot_config_get_array(const char* key, oshot_value_t** out, size_t max);

void oshot_config_set_string(const char* key, oshot_str_t val);
void oshot_config_set_bool(const char* key, bool val);
void oshot_config_set_int64(const char* key, int64_t val);
void oshot_config_set_double(const char* key, double val);

// Only for forwarding an already-typed value (e.g. writing back an
// oshot_value_t pulled from get_array). not the primary plugin-facing API.
oshot_value_t oshot_config_get_value(const char* key, oshot_value_t fallback);
void          oshot_config_set_value(const char* key, const oshot_value_t* val);

/* ------------------------------------------------------------------
 * Cache (plugin namespace only)
 * ------------------------------------------------------------------ */
oshot_str_t oshot_cache_get_string(const char* key, oshot_str_t fallback);
bool        oshot_cache_get_bool(const char* key, bool fallback);
int64_t     oshot_cache_get_int64(const char* key, int64_t fallback);
double      oshot_cache_get_double(const char* key, double fallback);
size_t      oshot_cache_get_array(const char* key, oshot_value_t** out, size_t max);

void oshot_cache_set_string(const char* key, oshot_str_t val);
void oshot_cache_set_bool(const char* key, bool val);
void oshot_cache_set_int64(const char* key, int64_t val);
void oshot_cache_set_double(const char* key, double val);

// Only for forwarding an already-typed value (e.g. writing back an
// oshot_value_t pulled from get_array). not the primary plugin-facing API.
oshot_value_t oshot_cache_get_value(const char* key, oshot_value_t fallback);
void          oshot_cache_set_value(const char* key, const oshot_value_t* val);

/* ------------------------------------------------------------------
 * ImGui-bound text buffers
 * ------------------------------------------------------------------ */
bool oshot_get_text(const char* imgui_id, oshot_str_t* ret);
void oshot_set_text(const char* imgui_id, oshot_str_t value);

/* ------------------------------------------------------------------
 * Capture acquisition
 * ------------------------------------------------------------------ */
oshot_capture_t oshot_get_capture(void);
void            oshot_capture_free(oshot_capture_t* cap);

/* ------------------------------------------------------------------
 * Plugin descriptor entry point
 * Must return a pointer to a static, process-lifetime oshot_plugin_t,
 * not something allocated per call.
 * ------------------------------------------------------------------ */
oshot_plugin_t* oshot_host_get_plugin(void);

#ifdef __cplusplus
}
#endif
