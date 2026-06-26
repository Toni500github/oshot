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

typedef struct
{
    const char* p;
    size_t      len;
} oshot_str_t;

oshot_str_t oshot_str_new(const char* str, size_t n);
void        oshot_str_free(oshot_str_t* str);

uint32_t oshot_get_abi_version(void);
bool     oshot_get_plugin_data_dir(oshot_str_t* ret);
bool     oshot_get_text(const char* imgui_id, oshot_str_t* ret);
void     oshot_set_text(const char* imgui_id, oshot_str_t value);

void oshot_display_msg(OSLogLevel lvl, oshot_str_t str);
void oshot_log(OSLogLevel lvl, oshot_str_t str);

typedef struct
{
    int32_t  w;
    int32_t  h;
    uint8_t* rgba;
} oshot_capture_t;
oshot_capture_t oshot_get_capture(bool render_anns);
void            oshot_capture_free(oshot_capture_t* cap);

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

/* Must return a pointer to a static, process-lifetime oshot_plugin_t,
 * not something allocated per call. */
oshot_plugin_t* oshot_host_get_plugin(void);

#ifdef __cplusplus
}
#endif
