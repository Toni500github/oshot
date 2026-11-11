/* The mock host used from C, with a small plugin defined here.
 * plugin_mock.cpp runs mock_host_c_test() and checks it returned 0 */

#include <string.h>

#include "oshot/oshot_mock_host.h"

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;
    oshot_str_t         note; /* ui_input_text's value */
    bool                loud;
    int                 ui_task_runs;
} reverse_state_t;

static bool reverse_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    reverse_state_t* s = calloc(1, sizeof(*s));
    if (!s)
        return false;
    s->host = host;
    s->ctx  = ctx;
    s->loud = host->get_bool(ctx, OSHOT_STORE_CONFIG, "loud", false);
    *state  = s;
    return true;
}

static void reverse_destroy(void* state)
{
    reverse_state_t* s = state;
    s->host->str_free(&s->note);
    free(s);
}

static void count_task(void* user)
{
    ((reverse_state_t*)user)->ui_task_runs++;
}

static void reverse_render(void* state)
{
    reverse_state_t* s = state;
    if (s->host->ui_begin_section(s->ctx, oshot_str("Options")))
    {
        s->host->ui_checkbox(s->ctx, oshot_str("Loud"), &s->loud);
        s->host->ui_input_text(s->ctx, oshot_str("Note"), &s->note, 0);
        if (s->host->ui_button(s->ctx, oshot_str("Copy##note")))
            s->host->copy_text(s->ctx, s->note, NULL);
    }
    s->host->ui_end_section(s->ctx);
    s->host->run_on_ui_thread(s->ctx, count_task, s);
}

static bool reverse_transform(void* state, const char* buffer_id, oshot_str_t in, oshot_str_t* out)
{
    reverse_state_t* s = state;
    if (strcmp(buffer_id, OSHOT_TEXT_OCR) != 0)
        return false;
    *out    = s->host->str_new(in.p, in.len);
    char* p = (char*)out->p;
    for (size_t i = 0; i < in.len / 2; ++i)
    {
        const char c      = p[i];
        p[i]              = p[in.len - 1 - i];
        p[in.len - 1 - i] = c;
    }
    return true;
}

static const oshot_setting_t reverse_settings[] = {
    { .kind = OSHOT_SETTING_BOOL, .key = "loud", .label = "Loud", .fallback = { .kind = OSHOT_VAL_BOOL, .b = false } },
};

static const oshot_plugin_t reverse_plugin = {
    .struct_size    = sizeof(oshot_plugin_t),
    .abi_version    = OSHOT_API_VERSION,
    .imgui_version  = OSHOT_NO_IMGUI,
    .id             = "io.github.test.reverse",
    .name           = "Reverse",
    .init           = reverse_init,
    .destroy        = reverse_destroy,
    .render         = reverse_render,
    .settings       = reverse_settings,
    .n_settings     = 1,
    .transform_text = reverse_transform,
};

#define EXPECT(cond)         \
    do                       \
    {                        \
        if (!(cond))         \
            return __LINE__; \
    } while (0)

int mock_host_c_test(void)
{
    const size_t  live_before = oshot_mock_live_allocations();
    oshot_mock_t* m           = oshot_mock_new();
    EXPECT(oshot_mock_load(m, &reverse_plugin));
    oshot_mock_capture_start(m);

    EXPECT(strcmp(oshot_mock_ocr(m, "abc", NULL, 0), "cba") == 0);
    EXPECT(strcmp(oshot_mock_barcodes(m, NULL, 0), "") == 0);

    /* A frame with typing and a click in a section. The copy button has a ##suffix */
    oshot_mock_type(m, "Note", "hello");
    oshot_mock_render(m);
    EXPECT(oshot_mock_drew(m, "checkbox Loud"));
    oshot_mock_click(m, "Copy");
    oshot_mock_render(m);
    EXPECT(oshot_mock_clipboard_text(m) && strcmp(oshot_mock_clipboard_text(m), "hello") == 0);
    EXPECT(((reverse_state_t*)oshot_mock_state(m))->ui_task_runs == 1); /* the first frame's task ran in the second */
    EXPECT(oshot_mock_log_count(m, OSHOT_LOG_WARN) == 0);

    /* A click on nothing is a mistake the mock reports */
    oshot_mock_click(m, "Missing");
    oshot_mock_render(m);
    EXPECT(oshot_mock_log_count(m, OSHOT_LOG_ERROR) == 1);

    oshot_mock_free(m);
    EXPECT(oshot_mock_live_allocations() == live_before);
    return 0;
}
