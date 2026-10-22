# Example plugins

The same plugin in four languages: it adds an "Uppercase the OCR output" button to the text tools window, can
uppercase new text as soon as it's extracted, and declares a setting for that plus a prefix, which oshot shows in
Preferences > Plugins and saves for it. It draws only with oshot's widgets (`imgui_version = 0`), so it keeps working
when oshot updates Dear ImGui. See [docs/plugins.md](../../docs/plugins.md) for the API.

| Folder | Language | Build | ID |
| ------ | -------- | ----- | -- |
| [`c`](c) | C11 | CMake | `io.github.toni500github.uppercase` |
| [`cpp`](cpp) | C++20 | CMake | `io.github.toni500github.uppercase-cpp` |
| [`rust`](rust) | Rust 2024 (1.85+) | Cargo | `io.github.toni500github.uppercase-rust` |
| [`zig`](zig) | Zig 0.17 | `zig build` | `io.github.toni500github.uppercase-zig` |
| [`cimgui`](cimgui) | C11 | CMake | `io.github.toni500github.line-lengths` |

[`cimgui`](cimgui) is a different plugin: it draws a histogram of the extracted text's line lengths with Dear ImGui
through cimgui, which the widgets can't draw. It shows how to use cimgui and what it costs: the plugin only loads in
an oshot built with the same Dear ImGui, so it needs a rebuild for every oshot minor release.

Each folder is a repository of its own with an `oshot-plugin.toml`, so they can be installed side by side:

```sh
oshotpm install examples/plugins/rust
```

To build one by hand, point it at the SDK of an oshot `-plugins` build (`build/<type>/sdk` in the build tree) with
the `OSHOT_SDK` environment variable, which oshotpm sets on its own. The Rust one doesn't need it.

How they get to the C API:

- **C** and **C++** include `oshot/oshot_plugin.h` from the SDK; the cimgui example also includes `oshot/cimgui.h`
  and links cimgui as [docs/plugins.md](../../docs/plugins.md#building) describes.
- **Rust**: [`src/ffi.rs`](rust/src/ffi.rs) has hand-written bindings to `oshot_plugin.h`, so the build needs neither
  bindgen nor the SDK. oshot only appends to its structs, so they keep working with newer versions of oshot; copy the
  fields a newer header adds to use them. A Rust plugin that also uses cimgui should generate its bindings with
  bindgen from the SDK's headers, and use `oshot/cimgui.h`, not `imgui-rs`/`imgui-sys`: those compile their own copy
  of Dear ImGui, which has no context inside oshot.
- **Zig** translates `oshot_plugin.h` in [`build.zig`](zig/build.zig) and imports it as the `c` module.

Languages that bring a runtime of their own, like Go, are a poor fit: a Go library can't be unloaded, and there can
be only one Go runtime per process, so two Go plugins can't be loaded together.
