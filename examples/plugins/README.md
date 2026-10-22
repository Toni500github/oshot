# Example plugins

The same plugin in four languages: it adds an "Uppercase the OCR output" button to the text tools window, can
uppercase new text as soon as it's extracted, and has a setting for that plus a prefix in Preferences > Plugins.
Each one implements every callback and uses the common host functions. See
[docs/plugins.md](../../docs/plugins.md) for the API.

| Folder | Language | Build | ID |
| ------ | -------- | ----- | -- |
| [`c`](c) | C11 | CMake | `io.github.toni500github.uppercase` |
| [`cpp`](cpp) | C++20 | CMake | `io.github.toni500github.uppercase-cpp` |
| [`rust`](rust) | Rust 2024 (1.85+) | Cargo + bindgen | `io.github.toni500github.uppercase-rust` |
| [`zig`](zig) | Zig 0.17 | `zig build` | `io.github.toni500github.uppercase-zig` |

Each folder is a repository of its own with an `oshot-plugin.toml`, so they can be installed side by side:

```sh
oshotpm install examples/plugins/rust
```

To build one by hand, point it at the SDK of an oshot `-plugins` build (`build/<type>/sdk` in the build tree) with
the `OSHOT_SDK` environment variable, which oshotpm sets on its own.

How they get to the C API:

- **C** and **C++** include `oshot/oshot_plugin.h` and `oshot/cimgui.h` from the SDK.
- **Rust**: [`build.rs`](rust/build.rs) generates the bindings from the SDK headers with bindgen (which needs
  libclang), so they cover all of cimgui and always match the oshot the plugin is built for.
  [`src/ffi.rs`](rust/src/ffi.rs) includes them and adds a few helpers. Use `oshot/cimgui.h` for Dear ImGui, not
  `imgui-rs`/`imgui-sys`: those compile their own copy of Dear ImGui, which has no context inside oshot. On Windows it
  needs Rust's GNU toolchain (MSYS2's `rust` package), since the SDK's cimgui import library is a MinGW one.
- **Zig** translates the SDK headers in [`build.zig`](zig/build.zig) and imports them as the `c` module.

Languages that bring a runtime of their own, like Go, are a poor fit: a Go library can't be unloaded, and there can
be only one Go runtime per process, so two Go plugins can't be loaded together.
