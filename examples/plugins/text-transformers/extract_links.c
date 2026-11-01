/* Example oshot plugin: a text transformer that keeps only the links and email addresses.
 *
 * - transform_text(): replaces the OCR or barcode text with the web links
 *                     ("https://...", "www....") and email addresses in it, one per
 *                     line and without duplicates. A text with none passes through
 *                     unchanged (it returns false), so a scan of plain text still
 *                     shows that text
 *
 * The setting picks links, emails or both. The text tools' "Original" toggle shows
 * the whole text again.
 *
 * Build it with the CMakeLists.txt next to it, or install the folder with
 * `oshotpm install <this folder>`. See docs/plugins.md. Public domain (Unlicense).
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "oshot/oshot_plugin.h"

static const char* const kinds[] = { "links and emails", "links", "emails" };

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;
    bool                links;
    bool                emails;
} state_t;

static const oshot_setting_t settings[] = {
    {
        .kind      = OSHOT_SETTING_CHOICE,
        .key       = "keep",
        .label     = "Keep",
        .fallback  = { .kind = OSHOT_VAL_STRING, .s = { "links and emails", sizeof("links and emails") - 1 } },
        .choices   = kinds,
        .n_choices = sizeof(kinds) / sizeof(kinds[0]),
    },
};

static void read_settings(state_t* s)
{
    oshot_str_t keep = s->host->get_string(s->ctx, OSHOT_STORE_CONFIG, "keep", oshot_str_borrow("links and emails"));
    s->links         = keep.p && strcmp(keep.p, "emails") != 0;
    s->emails        = keep.p && strcmp(keep.p, "links") != 0;
    s->host->str_free(&keep);
}

/* The found items, as one string with a line each */
typedef struct
{
    char*  p;
    size_t len;
    size_t cap;
} buf_t;

static bool buf_has_line(const buf_t* b, const char* s, size_t len)
{
    for (size_t i = 0; i + len <= b->len;)
    {
        size_t end = i;
        while (end < b->len && b->p[end] != '\n')
            ++end;
        if (end - i == len && memcmp(b->p + i, s, len) == 0)
            return true;
        i = end + 1;
    }
    return false;
}

static void buf_add_line(buf_t* b, const char* s, size_t len)
{
    if (len == 0 || buf_has_line(b, s, len))
        return;
    if (b->len + len + 2 > b->cap)
    {
        size_t cap = (b->cap + len + 2) * 2;
        char*  p   = realloc(b->p, cap);
        if (!p)
            return;
        b->p   = p;
        b->cap = cap;
    }
    if (b->len)
        b->p[b->len++] = '\n';
    memcpy(b->p + b->len, s, len);
    b->len += len;
}

static bool starts_with(const char* p, size_t len, const char* prefix)
{
    const size_t n = strlen(prefix);
    if (len < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)p[i]) != prefix[i])
            return false;
    return true;
}

static bool url_char(char c)
{
    return c > ' ' && c != '"' && c != '\'' && c != '<' && c != '>' && (unsigned char)c != 0x7f;
}

static bool email_local_char(char c)
{
    return isalnum((unsigned char)c) || strchr("._%+-", c);
}

static bool email_domain_char(char c)
{
    return isalnum((unsigned char)c) || c == '.' || c == '-';
}

/* Trailing punctuation belongs to the sentence, not to the link: "see https://x.org." */
static size_t trim_end(const char* p, size_t begin, size_t end)
{
    while (end > begin && strchr(".,;:!?)]}", p[end - 1]))
        --end;
    return end;
}

static bool plugin_transform_text(void* state, const char* buffer_id, oshot_str_t in, oshot_str_t* out)
{
    (void)buffer_id; /* OCR and barcodes alike */
    state_t*    s     = state;
    const char* p     = in.p;
    buf_t       found = { NULL, 0, 0 };

    for (size_t i = 0; i < in.len; ++i)
    {
        /* A link starts a word */
        const bool word_start = i == 0 || !url_char(p[i - 1]) || strchr("([{", p[i - 1]);
        if (word_start && (starts_with(p + i, in.len - i, "https://") || starts_with(p + i, in.len - i, "http://") ||
                           starts_with(p + i, in.len - i, "www.")))
        {
            size_t end = i;
            while (end < in.len && url_char(p[end]))
                ++end;
            end = trim_end(p, i, end);
            if (s->links)
                buf_add_line(&found, p + i, end - i);
            /* An address inside a link ("https://x.org/?to=a@b.c") isn't an email of its own */
            i = end;
            continue;
        }

        if (p[i] != '@' || !s->emails)
            continue;
        size_t begin = i;
        while (begin > 0 && email_local_char(p[begin - 1]))
            --begin;
        size_t end = i + 1;
        while (end < in.len && email_domain_char(p[end]))
            ++end;
        end = trim_end(p, i + 1, end);

        /* A domain with a dot and at least two letters after the last one */
        const char* dot = NULL;
        for (size_t j = i + 1; j < end; ++j)
            if (p[j] == '.')
                dot = p + j;
        if (begin < i && dot && (size_t)(p + end - dot) > 2)
            buf_add_line(&found, p + begin, end - begin);
        i = end > i ? end - 1 : i;
    }

    if (found.len == 0)
    {
        free(found.p);
        return false; /* nothing to keep: pass the text through */
    }
    *out = s->host->str_new(found.p, found.len);
    free(found.p);
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
    .id                = "io.github.toni500github.extract-links",
    .name              = "Extract links",
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
