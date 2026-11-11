# Example plugins

The same plugin in C and C++: it adds an "Uppercase the OCR output" button to the text tools window, can
uppercase new text as soon as it's extracted, and declares a setting for that plus a prefix, which oshot shows in
Preferences > Plugins and saves for it. It draws only with oshot's widgets (`imgui_version = OSHOT_NO_IMGUI`), so it keeps working
when oshot updates Dear ImGui. See [docs/plugins.md](../../docs/plugins.md) for the API.

| Folder | Language | Build | ID |
| ------ | -------- | ----- | -- |
| [`c`](c) | C11 | CMake | `io.github.toni500github.uppercase` |
| [`cpp`](cpp) | C++20 | CMake | `io.github.toni500github.uppercase-cpp` |
| [`cimgui`](cimgui) | C11 | CMake | `io.github.toni500github.line-lengths` |
| [`scan-results`](scan-results) | C11 | CMake | `io.github.toni500github.scan-results` |
| [`text-transformers`](text-transformers) | C11 | CMake | `io.github.toni500github.join-lines`, `io.github.toni500github.extract-links` |
| [`export-png`](export-png) | C11 | CMake | `io.github.toni500github.export-png` |

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

Each folder is a repository of its own with an `oshot-plugin.toml`, so they can be installed side by side:

```sh
oshotpm install examples/plugins/c
```

To build one by hand, point it at the SDK of an oshot `-plugins` build (`build/<type>/sdk` in the build tree) with
the `OSHOT_SDK` environment variable, which oshotpm sets on its own. Their `CMakeLists.txt` find it with
`find_package(oshot)` and build the library with `oshot_add_plugin()`; the cimgui one passes `CIMGUI`, which links
what calling cimgui needs on each system (see [docs/plugins.md](../../docs/plugins.md#building)).

oshot's tests (`make BUILD_TESTS=1 test`) run the uppercase, text transformer and export examples on the SDK's mock
host, `oshot/oshot_mock_host.h`, in [`tests/plugin_mock.cpp`](../../tests/plugin_mock.cpp).

To start a plugin of your own, use the [template](https://github.com/Toni500github/oshot-plugin-test) rather than
copying one of these: it has unit tests and a release workflow for every target.

Languages that bring a runtime of their own, like Go, are a poor fit: a Go library can't be unloaded, and there can
be only one Go runtime per process, so two Go plugins can't be loaded together.
