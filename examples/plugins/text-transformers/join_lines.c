/* Example oshot plugin: a text transformer that rejoins what OCR split over lines.
 *
 * - transform_text(): joins a word hyphenated at the end of a line ("exam-\nple"
 *                     becomes "example") and, unless the setting is off, the lines
 *                     of each paragraph into one, keeping the blank lines between
 *                     paragraphs. It only changes OCR text, since QR codes and
 *                     barcodes have no line breaks to fix, and passes the rest through
 *
 * oshot runs every plugin's transformer in the order set in Preferences > Plugins,
 * so put this one before a transformer that looks for links, or a link split over
 * two lines is cut in half.
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "oshot/oshot_plugin.h"

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;
    bool                join_paragraphs;
} state_t;

static const oshot_setting_t settings[] = {
    {
        .kind     = OSHOT_SETTING_BOOL,
        .key      = "join-paragraphs",
        .label    = "Join the lines of each paragraph",
        .help     = "Off: only rejoin words hyphenated at the end of a line",
        .fallback = { .kind = OSHOT_VAL_BOOL, .b = true },
    },
};

static void read_settings(state_t* s)
{
    s->join_paragraphs = s->host->get_bool(s->ctx, OSHOT_STORE_CONFIG, "join-paragraphs", true);
}

/* The end of the line starting at `i`, at its '\n' or the end of the text */
static size_t line_end(const char* p, size_t len, size_t i)
{
    while (i < len && p[i] != '\n')
        ++i;
    return i;
}

static bool blank(const char* p, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; ++i)
        if (!isspace((unsigned char)p[i]))
            return false;
    return true;
}

static bool plugin_transform_text(void* state, const char* buffer_id, oshot_str_t in, oshot_str_t* out)
{
    state_t* s = state;
    if (strcmp(buffer_id, OSHOT_TEXT_OCR) != 0 || in.len == 0)
        return false;

    /* Never longer than the input */
    char* buf = malloc(in.len + 1);
    if (!buf)
        return false;

    const char* p = in.p;
    size_t      n = 0;
    for (size_t i = 0; i < in.len;)
    {
        const size_t end = line_end(p, in.len, i);

        /* Trailing spaces would end up in the middle of a joined line */
        size_t last = end;
        while (last > i && (p[last - 1] == ' ' || p[last - 1] == '\t' || p[last - 1] == '\r'))
            --last;
        memcpy(buf + n, p + i, last - i);
        n += last - i;

        if (end >= in.len)
            break;

        const size_t next       = end + 1;
        const size_t next_end   = line_end(p, in.len, next);
        const bool   this_blank = blank(p, i, end), next_blank = blank(p, next, next_end);

        /* "exam-" + "ple": a letter, a hyphen, and the next line going on in lowercase */
        if (last - i >= 2 && p[last - 1] == '-' && isalpha((unsigned char)p[last - 2]) && next < in.len &&
            islower((unsigned char)p[next]))
            --n;
        else if (s->join_paragraphs && !this_blank && !next_blank)
            buf[n++] = ' ';
        else
            buf[n++] = '\n';
        i = next;
    }

    if (n == in.len && memcmp(buf, in.p, n) == 0)
    {
        free(buf);
        return false;
    }
    *out = s->host->str_new(buf, n);
    free(buf);
    return true;
}

static bool plugin_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    if (!OSHOT_HOST_HAS(host, report_result))
    {
        host->log(ctx, OSHOT_LOG_ERROR, oshot_str_borrow("needs an oshot with text transformers, update oshot"));
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
    free(state);
}

static void plugin_on_settings_saved(void* state)
{
    read_settings(state);
}

static const oshot_plugin_t descriptor = {
    .struct_size       = sizeof(oshot_plugin_t),
    .abi_version       = OSHOT_API_VERSION,
    .imgui_version     = OSHOT_NO_IMGUI,
    .id                = "io.github.toni500github.join-lines",
    .name              = "Join lines",
    .init              = plugin_init,
    .destroy           = plugin_destroy,
    .settings          = settings,
    .n_settings        = sizeof(settings) / sizeof(settings[0]),
    .on_settings_saved = plugin_on_settings_saved,
    .transform_text    = plugin_transform_text,
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void)
{
    return &descriptor;
}
