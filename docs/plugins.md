# Writing oshot plugins

> Plugins are **experimental**: the API below can still change before it's declared stable, and
> plugins then need to be rebuilt.

Plugins only work in the `-plugins` builds of oshot. A plugin is a shared library (`.so`, `.dll`, or `.dylib` or `.so` on macOS) with a plain
C interface, so it can be written in any language that can export a C function: C, C++, Rust, Zig, and so on. The
example in [`examples/plugins/uppercase`](../examples/plugins/uppercase) is a complete plugin in C.

What a plugin can do:

- draw its own section in the text tools window, and its own settings in Preferences > Plugins, with Dear ImGui
  (through [cimgui](https://github.com/cimgui/cimgui));
- read and replace the OCR and barcode text, and read the selected image;
- react when text has been extracted;
- keep settings in its own `config.toml`, and other state in oshot's cache;
- do anything else a native program can, e.g. use the network from its own threads.

Plugins run inside oshot with the same permissions: oshot can't sandbox them.

## Terms

| Term | Meaning |
| ---- | ------- |
| **plugin** | One library, identified by its **ID**. |
| **ID** | Reverse-domain name like `io.github.user.translate`: lowercase letters, digits, `-`, `_`, at least one `.`, up to 64 characters. The same ID is compiled into the library and written in the manifest. |
| **repository** | Everything one `oshot-plugin.toml` describes: one or more plugins, from a git repository, a folder or an archive. |
| **manifest** | The `oshot-plugin.toml` file at the root of a repository. |
| **SDK** | The headers (`oshot/oshot_plugin.h`, `oshot/cimgui.h`), plus on Windows the cimgui import library, that come with oshot. |
| **host** | oshot, as seen from a plugin. |

## The SDK

oshot ships the SDK next to itself, and `oshotpm` passes its location to plugin builds as the `OSHOT_SDK` environment
variable (a folder with `include/oshot/` and, on Windows, `lib/`):

| Package | SDK |
| ------- | --- |
| Linux zip, `.deb`, `make install` | `<prefix>/include/oshot` (e.g. `/usr/local/include/oshot`) |
| AppImage | inside the AppImage, found by `oshotpm` |
| Windows zip / installer | `sdk\` next to `oshot.exe` |
| macOS `.app` | `oshot.app/Contents/Resources/sdk` |
| Build tree | `build/<type>/sdk` |

Set `OSHOT_SDK` yourself to use another one.

## Building

oshot loads cimgui itself, so a plugin uses it without bundling Dear ImGui:

- **Linux and the BSDs**: compile with `-shared -fPIC` and the SDK's `include` folder, without linking anything.
- **macOS**: also link with `-undefined dynamic_lookup`.
- **Windows**: link `sdk/lib/libcimgui.dll.a`. It's a MinGW import library: build with MinGW-w64 (MSYS2's UCRT64 or
  CLANGARM64 environments, the ones oshot is built with).

The example's [`CMakeLists.txt`](../examples/plugins/uppercase/CMakeLists.txt) does all of this. Build the plugin
with hidden symbols (`-fvisibility=hidden`) and export only the entry point, with `OSHOT_PLUGIN_EXPORT`.

A plugin that uses cimgui only works with the exact Dear ImGui version oshot is built with (`OSHOT_IMGUI_VERSION`),
because cimgui exposes ImGui's structs. oshot refuses plugins built for another version, so rebuild plugins when oshot
updates Dear ImGui.

## The API

Everything is in `oshot/oshot_plugin.h`, which documents each function. In short:

```c
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "oshot/cimgui.h"
#include "oshot/oshot_plugin.h"

static bool my_init(const oshot_host_t* host, oshot_ctx_t* ctx, void** state)
{
    /* keep host and ctx: every host call needs them */
    host->log(ctx, OSHOT_LOG_INFO, oshot_str_borrow("hello"));
    *state = /* your state */;
    return true;
}

static void my_render(void* state) { igText("Hello from a plugin"); }

static const oshot_plugin_t descriptor = {
    .struct_size   = sizeof(oshot_plugin_t),
    .abi_version   = OSHOT_API_VERSION,
    .imgui_version = OSHOT_IMGUI_VERSION, /* 0 if the plugin doesn't use ImGui */
    .id            = "io.github.user.hello",
    .name          = "Hello",
    .init          = my_init,
    .render        = my_render,
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
- `host->get_text()`/`set_text()` work on `OSHOT_TEXT_OCR` and `OSHOT_TEXT_BARCODE`.
- `host->get_capture()` copies the whole selected image: call it when needed, not every frame.

### Threads

All callbacks run on oshot's UI thread. A plugin may start its own threads (for network requests, say). From those:

- `log`, `get_data_dir`, `str_new`/`str_free` and `run_on_ui_thread` can be called;
- everything else is refused and logged: hand the result to `host->run_on_ui_thread(ctx, fn, user)`, which calls
  `fn(user)` on the UI thread before the plugin is drawn next;
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
```

- Each build step runs in the repository's root, with `sh -c` (`cmd.exe` on Windows), and the first failing step stops
  the install. `build-steps-<platform>` (e.g. `build-steps-windows`) replaces `build-steps` on that platform. CMake
  commands like the ones above work with both shells.
- The build gets `OSHOT_SDK`, `OSHOT_PLATFORM`, `OSHOT_ARCH`, `OSHOT_TARGET` (e.g. `linux-x86_64`) and
  `OSHOT_VERSION` in its environment.
- After building, oshotpm loads the library to check its ID, ABI and Dear ImGui version (without calling `init()`),
  then copies it into the plugin's folder.

## Distributing

People install plugins from the Plugins menu in oshot or with `oshotpm install <SOURCE>`, where the source is:

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
