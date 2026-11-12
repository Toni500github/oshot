# Writing oshot plugins

> Plugins are **experimental**: the API below can still change before it's declared stable, and
> plugins then need to be rebuilt.

Plugins only work in the `-plugins` builds of oshot. A plugin is a shared library (`.so`, `.dll`, or on macOS `.dylib` or `.so`) with a plain
C interface, so it can be written in any language that can export a C function: C, C++, Rust, Zig, and so on.
[`examples/plugins`](../examples/plugins) has the same complete plugin in C and C++, one that uses cimgui, one that
uses the scan results and host services, two text transformers and an export action. To start a plugin of your own,
use the [template](#starting-from-the-template).

What a plugin can do:

- draw its own section in the text tools window with oshot's widgets (buttons, text fields, sliders, ...);
- declare its settings, which oshot shows in Preferences > Plugins and saves for it;
- draw anything else with Dear ImGui itself, through [cimgui](https://github.com/cimgui/cimgui);
- read and replace the OCR and barcode text, and read the selected image, with or without the annotations;
- rewrite every new OCR and barcode text before it's shown, in a chain the person orders;
- add buttons to the toolbar and entries to the text tools' menu, and show their progress and result in the overlay;
- react when text has been extracted, with each word's position and confidence, and when QR codes or barcodes have
  been decoded, with their positions;
- know when a capture starts and ends, and when the selection changes;
- copy to the clipboard, show notifications, open URLs and run programs the way oshot does on each system;
- keep settings in its own `config.toml`, and other state in oshot's cache;
- do anything else a native program can, e.g. use the network from its own threads.

Plugins run inside oshot with the same permissions: oshot can't sandbox them. Declare what yours does on its own, and why,
in the manifest's [`capabilities`](#the-manifest), so people see it before installing.

## Terms

| Term | Meaning |
| ---- | ------- |
| **plugin** | One library, identified by its **ID**. |
| **ID** | Reverse-domain name like `io.github.user.translate`: lowercase letters, digits, `-`, `_`, at least one `.`, up to 64 characters. The same ID is compiled into the library and written in the manifest. |
| **repository** | Everything one `oshot-plugin.toml` describes: one or more plugins, from a git repository, a folder or an archive. |
| **manifest** | The `oshot-plugin.toml` file at the root of a repository. |
| **SDK** | What oshot comes with for building plugins: the headers (`oshot/oshot_plugin.h`, `oshot/oshot_mock_host.h` for unit tests, `oshot/cimgui.h` for plugins that use cimgui), a CMake package and pkg-config files, plus on Windows the cimgui import library. |
| **host** | oshot, as seen from a plugin. |

## The SDK

oshot ships the SDK next to itself, and `oshotpm` passes its location to plugin builds as the `OSHOT_SDK` environment
variable. It's laid out like an install prefix:

```
include/oshot/oshot_plugin.h      the API
include/oshot/oshot_mock_host.h   a fake oshot for unit tests
include/oshot/cimgui.h            Dear ImGui through cimgui
lib/cmake/oshot/                  find_package(oshot)
lib/pkgconfig/                    oshot-plugin.pc, oshot-cimgui.pc
lib/libcimgui.dll.a               Windows only
```

| Package | SDK |
| ------- | --- |
| Linux zip, `.deb`, `make install` | `<prefix>` (e.g. `/usr/local`, with the headers in `/usr/local/include/oshot`) |
| AppImage | inside the AppImage, found by `oshotpm` |
| Windows zip / installer | `sdk\` next to `oshot.exe` |
| macOS `.app` | `oshot.app/Contents/Resources/sdk` |
| Build tree | `build/<type>/sdk` |

Set `OSHOT_SDK` yourself to use another one. The [template](#starting-from-the-template) downloads the SDK of a given
oshot commit with its `scripts/fetch_sdk.sh`.

## Building

A plugin only needs `oshot/oshot_plugin.h`: compile it as a shared library and link nothing from oshot. Build it with
hidden symbols (`-fvisibility=hidden`) and export only the entry point, with `OSHOT_PLUGIN_EXPORT`.

With CMake, the SDK's package does both:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_plugin C)

set(OSHOT_SDK "$ENV{OSHOT_SDK}" CACHE PATH "oshot's plugin SDK")
find_package(oshot REQUIRED CONFIG HINTS "${OSHOT_SDK}")

# A MODULE library with hidden symbols, written to build/out for the manifest's output-dir
oshot_add_plugin(my_plugin src/plugin.c OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
```

It also defines the `oshot::plugin` target (the headers) and the `OSHOT_API_VERSION` and `OSHOT_IMGUI_VERSION`
variables. Without `OSHOT_SDK` set, `find_package` looks in the usual prefixes, so an installed oshot is found too.

With Make, Meson or anything else, `pkg-config oshot-plugin` gives the include flags:

```sh
export PKG_CONFIG_PATH="$OSHOT_SDK/lib/pkgconfig"
cc -shared -fPIC -fvisibility=hidden $(pkg-config --cflags oshot-plugin) plugin.c -o libmy_plugin.so
```

A plugin that also uses cimgui (see [Dear ImGui through cimgui](#dear-imgui-through-cimgui)) gets it from oshot,
without bundling Dear ImGui:

- **Linux and the BSDs**: nothing more to link.
- **macOS**: link with `-undefined dynamic_lookup`.
- **Windows**: link `sdk/lib/libcimgui.dll.a`. It's a MinGW import library: build with MinGW-w64 (MSYS2's UCRT64 or
  CLANGARM64 environments, the ones oshot is built with, or llvm-mingw).

`oshot_add_plugin(<target> CIMGUI ...)` (or linking `oshot::cimgui`) and `pkg-config --libs oshot-cimgui` do this for
each system. The cimgui example's [`CMakeLists.txt`](../examples/plugins/cimgui/CMakeLists.txt) uses it.

## Testing

`oshot/oshot_mock_host.h` is a fake oshot for unit tests: a header-only C11 (or C++) implementation of every host
function, kept in memory. A test compiles the plugin's sources into a program, passes its descriptor to the mock, sends
the events oshot would send and reads back what the plugin did:

```c
#include "oshot/oshot_mock_host.h"

oshot_mock_t* m = oshot_mock_new();
oshot_mock_set_bool(m, OSHOT_STORE_CONFIG, "auto-uppercase", true);  // config.toml, as saved in Preferences
oshot_mock_load(m, oshot_plugin_entry());                             // checks the descriptor, calls init()
oshot_mock_capture_start(m);
oshot_mock_ocr(m, "hello", NULL, 0);          // transform_text(), then on_ocr_done()
assert(strcmp(oshot_mock_text(m, OSHOT_TEXT_OCR), "HELLO") == 0);

oshot_mock_render(m);                         // one frame of render()
assert(oshot_mock_drew(m, "button Uppercase the OCR output"));
oshot_mock_click(m, "Uppercase the OCR output");  // pressed in the next frame
oshot_mock_render(m);
oshot_mock_free(m);
assert(oshot_mock_live_allocations() == 0);   // every host string was freed
```

The capture image, the OCR and barcode text, config and cache values and `run_process()` results are set by the test;
the clipboard, notifications, opened URLs, started commands, actions and reports are read back. Widgets draw nothing:
the mock records each frame's labels and text, and presses buttons, toggles checkboxes or types into fields by label.
`run_on_ui_thread()` tasks, process exits and final images arrive with the next `oshot_mock_render()` (or
`oshot_mock_run_ui_tasks()`), as they do with the next frame in oshot. The thread that creates the mock is the UI
thread: UI-thread-only functions called from another one fail as they would in oshot. The mock prints the plugin's
warnings and errors, and the mistakes it notices (a widget outside `render()`, a section left open, a label used twice,
a click on nothing), so a test can check `oshot_mock_log_count(m, OSHOT_LOG_WARN) == 0`.
`oshot_mock_set_host_size()` pretends to be an older oshot, to test `OSHOT_HOST_HAS` checks. The header documents
each function.

## The API

Everything is in `oshot/oshot_plugin.h`, which documents each function. In short:

```c
#include "oshot/oshot_plugin.h"

typedef struct
{
    const oshot_host_t* host;
    oshot_ctx_t*        ctx;
} state_t;

static state_t g_state;

static bool my_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    /* keep host and ctx: every host call needs them */
    g_state = (state_t){ host, ctx };
    if (host->get_bool(ctx, OSHOT_STORE_CONFIG, "greet-on-start", true)) /* the setting below */
        host->log(ctx, OSHOT_LOG_INFO, oshot_str("hello"));
    *state = &g_state;
    return true;
}

static void my_render(void* state)
{
    state_t* s = state;
    if (s->host->ui_button(s->ctx, oshot_str("Say hello")))
        s->host->log(s->ctx, OSHOT_LOG_INFO, oshot_str("hello again"));
}

static const oshot_setting_t settings[] = {
    {
        .kind     = OSHOT_SETTING_BOOL,
        .key      = "greet-on-start",
        .label    = "Say hello when oshot starts",
        .fallback = { .kind = OSHOT_VAL_BOOL, .b = true },
    },
};

static const oshot_plugin_t descriptor = {
    .struct_size   = sizeof(oshot_plugin_t),
    .abi_version   = OSHOT_API_VERSION,
    .imgui_version = OSHOT_NO_IMGUI, /* OSHOT_IMGUI_VERSION only if it calls cimgui */
    .id            = "io.github.user.hello",
    .name          = "Hello",
    .init          = my_init,
    .render        = my_render,
    .settings      = settings,
    .n_settings    = sizeof(settings) / sizeof(settings[0]),
};

OSHOT_PLUGIN_EXPORT const oshot_plugin_t* oshot_plugin_entry(void) { return &descriptor; }
```

- The plugin exports one function, `oshot_plugin_entry()`, which returns a static descriptor. All its callbacks are
  optional.
- oshot calls back into the plugin through the descriptor. The plugin calls oshot only through the `oshot_host_t`
  table it gets in `init()`. It doesn't link against oshot.
- Strings are `oshot_str_t` (pointer and length). Strings oshot returns also end with `'\0'`; free them with
  `host->str_free`. Memory oshot allocates is freed through oshot's functions.
- Values are stored in `OSHOT_STORE_CONFIG` (the plugin's `config.toml`, saved when the person saves Preferences and
  when oshot exits) or `OSHOT_STORE_CACHE` (oshot's cache file).
- `host->get_text()`/`set_text()` work on `OSHOT_TEXT_OCR` and `OSHOT_TEXT_BARCODE`. To rewrite each new text, use
  a [text transformer](#text-transformers) instead of `set_text()`.
- `host->get_capture()` copies the whole selected image, without annotations: call it when needed, not every frame.
  `request_final_image()` gives the annotated one (see [Actions](#actions)).
- Host functions that can fail return an `oshot_err_t` (see [Errors](#errors)).

### Scan results

`on_ocr_done()` gets the text and every word Tesseract read (`words`, `n_words`):
its text, box, confidence (0-100), and the indexes of its text line and block, in reading order. Check
`OSHOT_OCR_HAS_WORDS(host)` before reading them: an older oshot passes a shorter `oshot_ocr_result_t`. It also runs
when OCR read nothing or failed, with empty text, no words and a confidence and `psm` of -1, so the plugin can clear
what it showed (plugins built before `on_barcodes_done` existed only get it with text).

`on_barcodes_done(state, codes, n)` runs after every QR code and barcode scan, with one `oshot_barcode_t` per code
found (two codes with the same data are two entries): ZBar's name for its type (`"QR-Code"`, `"EAN-13"`, ...), its
data and its box. `n` is 0 when the scan found nothing, so the plugin can clear what it showed.

Both arrays are borrowed: copy what you need, they're gone when the callback returns. The boxes are in the pixels of
the image that was scanned, which is the image `get_capture()` returns for the same selection, with (0, 0) at its
top-left corner. They don't depend on the screen's scaling (Wayland output scales, Retina).

### Text transformers

`transform_text(state, buffer_id, in, &out)` rewrites the text oshot extracted before it's shown: join paragraphs,
fix hyphenation, keep only the links, translate a few words with a local table. After every OCR (`buffer_id` is
`OSHOT_TEXT_OCR`) and QR/barcode scan (`OSHOT_TEXT_BARCODE`), oshot passes the text through each plugin's transformer
in turn, each one getting what the one before returned:

```c
static bool my_transform(void* state, const char* buffer_id, oshot_str_t in, oshot_str_t* out)
{
    state_t* s = state;
    if (strcmp(buffer_id, OSHOT_TEXT_OCR) != 0)
        return false;                         /* pass the barcode text through */
    char* upper = uppercase_copy(in.p, in.len);
    *out = s->host->str_new(upper, in.len);   /* oshot frees it */
    free(upper);
    return true;
}
```

- Return false to pass `in` through unchanged. Returning true with `*out` set replaces it; allocate `out` with
  `host->str_new()`, oshot frees it. `{ NULL, 0 }` means an empty text.
- The person orders the transformers in Preferences > Plugins, and can switch each one off without disabling its
  plugin. oshot keeps that in `plugins/state.toml` under `[text-transformers]`. Transformers not ordered yet (a plugin
  installed since) run after the others, sorted by ID.
- It runs on the UI thread and the overlay waits for it, so keep it fast: oshot logs a transformer that takes over
  200 ms, once per plugin. Slow work, like a translation over the network, belongs in an [action](#actions).
- `on_ocr_done()` runs after the chain and gets the transformed text. Its `words` stay as Tesseract read them, so
  their text no longer matches the transformed text.
- `get_text()` returns the transformed text. A `set_text()` from `on_ocr_done()` still works and replaces what the
  chain made, but with two plugins the last one wins, which is why the chain exists.
- The text tools window keeps the text from before the chain: an "Original" checkbox above the output shows it when a
  transformer changed something.

[`examples/plugins/text-transformers`](../examples/plugins/text-transformers) has two: one rejoins hyphenated words
and paragraphs, the other keeps only the links and email addresses. Run them in that order, or a link split over two
lines gets cut in half.

### Actions

An action is a button in the toolbar next to the selection, or an entry in the Plugins menu of the text tools
window, that runs a function of the plugin. It's how an upload or export plugin offers itself:

```c
static void on_click(void* user, int32_t action)
{
    state_t*    s   = user;
    oshot_str_t err = { NULL, 0 };
    if (s->host->request_final_image(s->ctx, on_image, s, &err) != OSHOT_OK)
    {
        s->host->report_result(s->ctx, action, false, err, oshot_str_borrow(NULL), NULL);
        s->host->str_free(&err);
    }
}

/* in init() */
host->register_action(ctx, OSHOT_PLACE_TOOLBAR, oshot_str("Upload"), &icon, on_click, s, &s->action, NULL);
```

- `register_action(ctx, where, label, icon, fn, user, &id, err)` adds one, usually in `init()`, and
  `unregister_action(ctx, id, err)` removes it. `fn(user, action)` runs on the UI thread at the start of the frame
  after the click. Actions go away with the plugin.
- `where` is `OSHOT_PLACE_TOOLBAR` or `OSHOT_PLACE_TEXT_TOOLS`. A toolbar button shows `icon`, an RGBA image (as
  `oshot_capture_t`, up to `OSHOT_ACTION_ICON_MAX` pixels on each side) that oshot copies and draws at the toolbar's
  button size, or the label's first letter when `icon` is NULL. The label is the button's tooltip and the menu entry's
  text. The tray has no actions yet.
- `request_final_image(ctx, on_ready, user, err)` asks for the selection as Save and Copy write it, annotations
  included. That image only exists once oshot has drawn and read back a frame, so it arrives later:
  `on_ready(user, image)` runs on the UI thread right after that frame. The image belongs to the plugin, which can
  hand it to a thread and free it there with `capture_free()`. If the overlay closes first, `on_ready` gets an image
  whose `rgba` is NULL. Outside an overlay (in `on_capture_end()`), and while the person picks a monitor on Wayland, it
  returns `OSHOT_ERR_UNSUPPORTED`.
- `report_progress(ctx, action, fraction, text, err)` and `report_result(ctx, action, ok, message, copyable, err)`
  work from any thread. oshot shows them in a box in the overlay's bottom-right corner: a progress bar until the
  result comes, then the message, in red if `ok` is false. A non-empty `copyable` (a link, a path) gets a Copy button;
  oshot doesn't copy it by itself. A result without one goes away after 10 seconds, one with it when the person closes
  it. Pass action 0 for work that isn't tied to an action; it's shown under the plugin's name.
- If no overlay is open when `report_result()` is called (the person closed it while an upload ran), oshot sends the
  result as a desktop notification, with `copyable` after the message. Progress without an overlay isn't shown.

[`examples/plugins/export-png`](../examples/plugins/export-png) saves the annotated selection as a PNG in its folder
from a thread, with progress, and offers the path to copy.

### Captures and the selection

In the tray, oshot keeps plugins loaded from one capture to the next, so a plugin needs to know where one ends:

- `on_capture_start()`: a screenshot or image is loaded, before its first frame. Also when the person opens another
  image in the overlay, after `on_capture_end()` for the previous one.
- `on_capture_end()`: the overlay is closed. Drop what belongs to the capture, or it shows up in the next one. Every
  `on_capture_start()` gets one, also when oshot exits during a capture (then before `destroy()`). `get_capture()`
  and `open_url()` don't work in it anymore.
- `on_selection_changed(state, x, y, w, h)`: once the person has finished drawing, moving or resizing the selection,
  not on every frame of the drag; also after Ctrl+A. It's in the pixels of the whole screenshot.

### Host services

oshot already does these right on every system, so plugins call them instead of their own versions:

| Function | What it does | Thread |
| -------- | ------------ | ------ |
| `copy_text`, `copy_image` | Copy to the clipboard like oshot does (xclip or wl-copy on Linux, which keep serving it after oshot exits). The image is encoded in the format set in Preferences. | UI |
| `notify(title, body)` | A desktop notification. | UI |
| `report_progress`, `report_result` | Progress and results of an action, shown in the overlay (see [Actions](#actions)). | any |
| `request_redraw()` | Draw the overlay's next frame now. With no input, oshot draws only a couple of frames a second, so call it every frame from `render()` while your UI animates, or from a thread when its data changes. | any |
| `open_url(url)` | Open a URL (`https://`, `mailto:`, `file:///`, ...) with the default application. It must start with a scheme. | UI |
| `run_process(argv, argc, cwd, on_exit, user, &id)` | Start a program, see below. | any |
| `kill_process(id)` | Stop a process `run_process()` started. | any |
| `mem_alloc`, `mem_free` | Memory oshot and the plugin can free on each other's side: on Windows they may use different C runtimes. | any |

`run_process()` never goes through a shell: `argv[0]` is the program, looked up in `PATH` if it has no directory,
and every argument reaches it as it is, so there's nothing to quote. Its working directory is `cwd`, or the plugin's
folder if `cwd` is empty, its stdin is empty, and it inherits oshot's environment. When it exits, `on_exit(user,
result)` runs on the UI thread with the exit code and what it wrote to stdout and stderr (up to
`OSHOT_PROCESS_OUTPUT_MAX` bytes each, `truncated` if there was more). It runs at the start of the next overlay frame,
so in the tray, a process that ends between captures reports at the start of the next one. `kill_process()` stops a
process (SIGTERM on Unix, also to the processes it started); its `on_exit` still runs, with `killed` set. oshot kills
the plugin's processes that are still running when it unloads the plugin, and drops their `on_exit` calls.

There's no HTTP client, file API or event bus: plugins use their language's libraries for those.

### Errors

Every host function that can fail returns an `oshot_err_t` and takes an `oshot_str_t* err` last:

```c
oshot_str_t err = { NULL, 0 };
if (host->copy_text(ctx, text, &err) != OSHOT_OK)
{
    host->log(ctx, OSHOT_LOG_WARN, err); /* or show it */
    host->str_free(&err);
}
```

| Code | Meaning |
| ---- | ------- |
| `OSHOT_OK` | It worked. `err` isn't touched. |
| `OSHOT_ERR_INVALID` | A bad argument: NULL, empty or malformed. |
| `OSHOT_ERR_THREAD` | A UI-thread function called from another thread. |
| `OSHOT_ERR_UNSUPPORTED` | Not available on this system, or not right now (no notifications, no overlay open). |
| `OSHOT_ERR_NOT_FOUND` | The program, folder or process doesn't exist. |
| `OSHOT_ERR_FAILED` | Failed for another reason, which `err` says. |

On failure oshot stores a message in `*err`, which the plugin frees with `str_free`. If the plugin passes `NULL`,
oshot logs the message under the plugin's ID instead, so a failure never goes unnoticed. More codes may be added:
treat anything but `OSHOT_OK` as a failure.

### Widgets

`render()` draws the plugin's section in the text tools window, every frame while it's expanded, with the `ui_*`
functions of the host table: `ui_text`, `ui_button`, `ui_checkbox`, `ui_input_text`, `ui_combo`, `ui_slider_int`,
`ui_slider_float`, `ui_color`, `ui_progress`, `ui_separator`, `ui_same_line`, `ui_tooltip` and
`ui_begin_section`/`ui_end_section`. They draw with oshot's theme, fonts and scaling, and they're part of the plugin
API, so a plugin that only uses them keeps working when oshot updates Dear ImGui.

- They work only inside `render()` and `render_preferences()`, on the UI thread. Called anywhere else they do nothing,
  and oshot logs it once.
- Labels and text are `oshot_str_t` and drawn as they are, never read as format strings, so text from OCR is safe to
  show. A label must be unique within the plugin: append `##` and something to tell apart widgets with the same text,
  e.g. `"Copy##url"` and `"Copy##text"`; the part after `##` isn't shown. Each plugin gets its own ID scope, so its
  labels can't clash with oshot's or another plugin's.
- Functions that return `bool` return true in the frame the person pressed or changed the widget.
- `ui_input_text` edits a host string (from `str_new`, `get_string`, ... or empty): when the text changes, oshot frees
  the old string and stores a new one, which the plugin frees with `str_free` when it's done with it. There's no fixed
  buffer size.
- Call `ui_end_section()` after every `ui_begin_section()`, whatever it returned; draw the section's contents only when
  it returned true. Sections left open when `render()` returns are closed by oshot and logged.

### Settings

Most plugins' settings are a few checkboxes, text fields, numbers or choices. Describe them in a static array of
`oshot_setting_t` and oshot draws them in Preferences > Plugins, saves them to the plugin's `config.toml` when the
person presses Save, and drops the changes when they discard them:

| Kind | Drawn as | Stored as | Read with |
| ---- | -------- | --------- | --------- |
| `OSHOT_SETTING_BOOL` | checkbox | bool | `get_bool` |
| `OSHOT_SETTING_STRING` | text field (`flags`: `OSHOT_UI_MULTILINE`, `OSHOT_UI_PASSWORD`) | string | `get_string` |
| `OSHOT_SETTING_INT` | slider from `min` to `max`, or a number field if `min == max` | integer | `get_int64` |
| `OSHOT_SETTING_FLOAT` | the same | float | `get_double` |
| `OSHOT_SETTING_CHOICE` | combo box with `choices` | the chosen string | `get_string` |

- `fallback` is the default, and its `kind` must match the setting's (`OSHOT_VAL_STRING` for STRING and CHOICE).
  `help`, if set, is shown as a tooltip.
- The plugin reads the values with the same key and fallback, usually in `init()`, and again in
  `on_settings_saved()`, which oshot calls after saving.
- oshot checks the array when it loads the plugin (and `oshotpm` when it installs it): every setting needs a unique
  key, a label and a fallback of the right kind.
- For anything the settings can't express, `render_preferences()` draws a custom page under them with the same
  widgets. It returns true while it has unsaved changes; oshot then calls `on_save_preferences()` or
  `on_discard_preferences()`.

### Dear ImGui through cimgui

For what the widgets can't draw (plots, tables, custom drawing), a plugin can call Dear ImGui through cimgui
(`oshot/cimgui.h`, with `CIMGUI_DEFINE_ENUMS_AND_STRUCTS` defined before including it), alongside the `ui_*`
functions. [`examples/plugins/cimgui`](../examples/plugins/cimgui) draws a histogram this way. The cost:

- cimgui exposes ImGui's structs, so the plugin only works with the exact Dear ImGui version oshot is built with. It
  must set `imgui_version = OSHOT_IMGUI_VERSION`, and oshot refuses it when the versions differ.
- oshot updates Dear ImGui only in minor releases (x.y.0), never in patch releases, so a cimgui plugin needs a rebuild
  for each oshot minor release. Plugins that use only the widgets and settings keep `imgui_version = OSHOT_NO_IMGUI` and don't.
- It links cimgui as described in [Building](#building).

oshot checks Dear ImGui's stacks around `render()` and `render_preferences()`: a `Begin`/`Push` the plugin didn't end
or pop is closed by oshot and logged, instead of breaking the rest of the window.

### Newer host functions

oshot only ever appends to `oshot_host_t` and `oshot_plugin_t`, so a plugin built with an older header keeps working.
A plugin built with a newer header may run in an older oshot, whose host table ends earlier: check a function added
after the plugin API's first version with `OSHOT_HOST_HAS(host, field)` before calling it, e.g. in `init()`:

```c
if (!OSHOT_HOST_HAS(host, ui_end_section)) /* the widgets came after the first version */
{
    host->log(ctx, OSHOT_LOG_ERROR, oshot_str("this oshot is too old for this plugin"));
    return false;
}
```

oshot does the same with the descriptor: fields added to `oshot_plugin_t` (like `settings`) are only read when the
plugin's `struct_size` includes them.

Structs oshot passes to callbacks have no size of their own. A field appended to one comes with host functions of
the same oshot version, so the plugin checks one of those: `OSHOT_OCR_HAS_WORDS(host)` is
`OSHOT_HOST_HAS(host, mem_alloc)`, and the header says which function to check for each later addition. Structs
oshot passes in arrays (`oshot_ocr_word_t`, `oshot_barcode_t`) and `oshot_setting_t` never change.

A plugin with a `transform_text` or actions checks `OSHOT_HOST_HAS(host, report_result)`: an older oshot never calls
the transformer and has no actions.

### Threads

All callbacks run on oshot's UI thread. A plugin may start its own threads (for network requests, say). From those:

- `log`, `get_data_dir`, `str_new`/`str_free`, `run_on_ui_thread`, `mem_alloc`/`mem_free`, `capture_free`,
  `run_process`, `kill_process`, `report_progress` and `report_result` can be called;
- everything else is refused and logged (the `ui_*` functions only once), and the host services return
  `OSHOT_ERR_THREAD`: hand the result to `host->run_on_ui_thread(ctx, fn, user)`, which calls `fn(user)` on the UI
  thread at the start of the next overlay frame (in the tray, between captures, when the next one starts);
- stop the threads in `destroy()`: tasks still queued then are dropped.

## The manifest

`oshotpm gen-manifest` writes a commented template. A minimal one:

```toml
[repository]
name = "my-plugins"                         # letters, digits, '-', '_', '='
url  = "https://github.com/user/my-plugins"

[dependencies]                              # shown to the person installing, never installed
all = ["cmake", "a C compiler"]

[translate]                                 # the plugin's name
id          = "io.github.user.translate"    # must match the library's ID
description = "Translate the extracted text"
authors     = ["user"]
licenses    = ["MIT"]
platforms   = ["all"]                       # or linux, macos, windows, freebsd, openbsd, netbsd, dragonfly

build-steps = [
    "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release",
    "cmake --build build --config Release",
]
output-dir = "build/out"                    # must contain exactly one library after the build

[translate.capabilities]                    # what it does on its own, and why
network = "Sends the text to translate.example.com to translate it"
```

- `[<plugin>.capabilities]` declares what the plugin does beyond oshot's API, each with the reason it needs to:
  `network` (connects to the internet), `files` (reads or writes files outside its own folder and oshot's cache) and
  `processes` (runs other programs, `run_process()` included). The reason can't be empty. oshot and oshotpm show them
  before installing and in the list of installed plugins. Nothing checks them, since a native library can do
  anything, so they're a promise to the people installing it.
- Each build step runs in the repository's root, with `sh -c` (`cmd.exe` on Windows), and the first failing step stops
  the install. `build-steps-<platform>` (e.g. `build-steps-windows`) replaces `build-steps` on that platform. CMake
  commands like the ones above work with both shells.
- The build gets `OSHOT_SDK`, `OSHOT_PLATFORM`, `OSHOT_ARCH`, `OSHOT_TARGET` (e.g. `linux-x86_64`) and
  `OSHOT_VERSION` in its environment.
- After building, oshotpm loads the library to check its ID, ABI, Dear ImGui version and settings (without calling
  `init()`), then copies it into the plugin's folder.

## Distributing

### The plugin index

The [oshot plugin index](https://github.com/Toni500github/oshot-plugin-index) lists plugins people can find in
oshot (Tools > Browse Plugins) and install by ID with `oshotpm install <ID>`. Each version in it pins a source (tag
and commit) and prebuilt files per target with their SHA-256. oshot installs the prebuilt file for the person's
platform after checking its checksum and the library's ID, ABI and ImGui version, and builds the pinned commit when
there's no prebuilt file. `oshotpm update` and the browser offer newer versions and warn about withdrawn ones.

To list a plugin, open a pull request there with a `plugins/<ID>.toml`: its README explains the format and what's
checked. Its `[capabilities]` are the same as the manifest's, and are what oshot shows for index plugins.

### Starting from the template

The [test plugin](https://github.com/Toni500github/oshot-plugin-test) is a GitHub template repository: a complete C
plugin with unit tests on the mock host and a workflow that builds every target oshot ships for (Linux, Windows and
macOS, x86_64 and aarch64), runs the tests, and for a `v*` tag publishes a release with the libraries and the plugin's
`plugins/<ID>.toml` for the index, checksums included. Its README lists what to change in a new plugin.

### Other ways

People can also install plugins from Tools > Install Plugins... in oshot or with `oshotpm install <SOURCE>`, where the
source is:

- **a git URL**: oshotpm clones it and runs the build steps. This needs git, the build tools and the SDK on the
  person's computer, which is normal on Linux but rare on Windows and macOS. `oshotpm update` fetches and rebuilds it.
- **a folder**: built in place, handy while developing.
- **a prebuilt archive** (`.zip`, `.tar.gz`/`.tgz`, `.tar.xz`/`.txz`): nothing is built, so this is what to offer most
  people. Put the manifest at the archive's root (or in a single top-level folder) and list a library per target:

  ```toml
  [translate.prebuilt]
  linux-x86_64   = "linux-x86_64/libtranslate.so"
  linux-aarch64  = "linux-aarch64/libtranslate.so"
  windows-x86_64 = "windows-x86_64/translate.dll"
  macos-aarch64  = "macos-aarch64/libtranslate.dylib"
  macos-x86_64   = "macos-x86_64/libtranslate.dylib"
  ```

  Targets are `<platform>-<arch>`, with `x86_64` or `aarch64` as the architecture. Archives are updated by installing
  a newer one with `oshotpm install --force`.

Installed plugins live in `<config dir>/oshot/plugins/<ID>/` (on Windows `%APPDATA%\oshot\plugins\<ID>\`) together with
their `config.toml`, and `plugins/state.toml` lists what's installed. Changes apply the next time oshot starts.
