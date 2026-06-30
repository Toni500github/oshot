#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSHOT_API_VERSION 1u

#define OCR_OUTPUT  "ocr_output"
#define ZBAR_OUTPUT "barcode_output"

typedef enum
{
    OSHOT_CAP_NONE    = 0,
    OSHOT_CAP_NETWORK = 1u << 0,
    OSHOT_CAP_FS      = 1u << 1,
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

/* ------------------------------------------------------------------
 * Plugin descriptor.
 * One per plugin, returned by the single exported symbol below
 * ---------------------------------------------------------------- */
typedef struct
{
    const char* name;
    uint32_t    abi_version;
    uint32_t    capabilities; /* OR of oshot_capabilities_t, informational only */

    void* (*init)(void);
    void (*destroy)(void* state);
    void (*render)(void* state);
    void (*on_ocr_done)(void* state);
} oshot_plugin_t;

/* ------------------------------------------------------------------
 * ABI / identity
 * ------------------------------------------------------------------ */
uint32_t oshot_get_abi_version(void);
bool     oshot_get_plugin_data_dir(oshot_str_t* ret);

/* ------------------------------------------------------------------
 * oshot_str_t lifecycle
 * ------------------------------------------------------------------ */
oshot_str_t oshot_str_new(const char* str, size_t n);
void        oshot_str_free(oshot_str_t* str);

/* ------------------------------------------------------------------
 * Logging / host messaging
 * ------------------------------------------------------------------ */
void oshot_display_msg(OSLogLevel lvl, oshot_str_t str);
void oshot_log(OSLogLevel lvl, oshot_str_t str);
void oshot_debug(oshot_str_t str);  // oshot_log(DEBUG, str);

/* ------------------------------------------------------------------
 * Config (plugin namespace only)
 * ------------------------------------------------------------------ */
oshot_str_t oshot_config_get_string(const char* key, oshot_str_t fallback);
bool        oshot_config_get_bool(const char* key, bool fallback);
int64_t     oshot_config_get_int64(const char* key, int64_t fallback);
float       oshot_config_get_float(const char* key, float fallback);
double      oshot_config_get_double(const char* key, double fallback);
size_t      oshot_config_get_array(const char* key, oshot_value_t** out, size_t max);

// Only frees OSHOT_VAL_STRING members
void oshot_value_array_free(oshot_value_t* arr, size_t n);

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
