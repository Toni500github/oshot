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
| [`scan-results`](scan-results) | C11 | CMake | `io.github.toni500github.scan-results` |
| [`text-transformers`](text-transformers) | C11 | CMake | `io.github.toni500github.join-lines`, `io.github.toni500github.extract-links` |
| [`export-png`](export-png) | C11 | CMake | `io.github.toni500github.export-png` |
| [`tesseract-cli`](tesseract-cli) | C11 | CMake | `io.github.toni500github.tesseract-cli` |
| [`word-boxes`](word-boxes) | C11 | CMake | `io.github.toni500github.word-boxes` |

[`cimgui`](cimgui) is a different plugin: it draws a histogram of the extracted text's line lengths with Dear ImGui
through cimgui, which the widgets can't draw. It shows how to use cimgui and what it costs: the plugin only loads in
an oshot built with the same Dear ImGui, so it needs a rebuild for every oshot minor release.

[`scan-results`](scan-results) shows the rest of the API: it lists the QR codes and barcodes oshot decoded with their
positions, with buttons that open or copy their data or copy just the code's image (`get_capture()` cropped to its
box, then `copy_image`), and the OCR words read with low confidence (`on_barcodes_done`, the words in `on_ocr_done`).
It clears them when the capture ends (`on_capture_start`/`on_capture_end`) or the selection changes
(`on_selection_changed`), sends a notification, and runs `uname -a` or a long `sleep` it can kill (`run_process`,
`kill_process`), reporting what the host returns as `oshot_err_t`.

[`text-transformers`](text-transformers) is one repository with two plugins, each a `transform_text`: Join lines
rejoins words hyphenated at the end of a line and the lines of each paragraph in OCR text, and Extract links keeps
only the links and email addresses (or passes the text through when it has none). oshot runs them in the order set
in Preferences > Plugins, and the order matters: with Extract links first, a link split over two lines is cut in
half before Join lines can rejoin it. Each manifest entry has its own `output-dir`, since an output folder must hold
exactly one library.

[`export-png`](export-png) is an action, a toolbar button with its own icon and the same entry in the text tools'
Plugins menu (`register_action`). It asks for the selection with its annotations (`request_final_image`), writes it
as a PNG into `<plugin folder>/exports` from a thread of its own with `report_progress`, and ends with
`report_result` offering the file's path to copy. It writes only inside its own folder, so it declares no
capability.

[`tesseract-cli`](tesseract-cli) is an OCR engine (`register_ocr_provider`) that runs the `tesseract` program, so it
needs Tesseract installed as a program. It lists the languages `tesseract --list-langs` prints as its models. Its
`recognize` runs on a worker thread oshot starts for each scan: it writes the image to a temporary file in its folder,
runs `tesseract <file> stdout tsv` with `run_process`, waits for it while checking whether oshot cancelled the scan
(then it kills the program), and turns the TSV into words with boxes. A setting picks oshot's prepared image
(`OSHOT_OCR_PREPARED`) or the selection as it is, and registers the engine again when it changes. It declares the
`processes` capability.

[`word-boxes`](word-boxes) draws on the overlay (`render_overlay` with `draw_rect` and `draw_text`): an outline around
each OCR word, colored by its confidence, which it can write above the box. The words are moved by the result's
`origin_x`/`origin_y`, so they stay on the text when the selection moves. "Fit the selection to the text" in the text
tools' Plugins menu puts the selection around the words (`get_selection`, `set_selection`).

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
  fields a newer header adds to use them. `oshot_err_t` is an integer type with constants there, not a Rust enum,
  because oshot may return codes added later. A Rust plugin that also uses cimgui should generate its bindings with
  bindgen from the SDK's headers, and use `oshot/cimgui.h`, not `imgui-rs`/`imgui-sys`: those compile their own copy
  of Dear ImGui, which has no context inside oshot.
- **Zig** translates `oshot_plugin.h` in [`build.zig`](zig/build.zig) and imports it as the `c` module. translate-c
  can't translate the `OSHOT_HOST_HAS`, `OSHOT_OCR_HAS_WORDS` and `OSHOT_OCR_HAS_ORIGIN` macros: the example has its
  own `hostHas()`.

Languages that bring a runtime of their own, like Go, are a poor fit: a Go library can't be unloaded, and there can
be only one Go runtime per process, so two Go plugins can't be loaded together.
