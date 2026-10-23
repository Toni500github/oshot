# Tests

oshot's tests, with [Catch2](https://github.com/catchorg/Catch2) v3.16.1's amalgamated files in `catch2/`:

| File | Tags | What |
|---|---|---|
| `text_extraction.cpp` | `[ocr]`, `[barcode]` | `OcrAPI` and `ZbarAPI` over the images in `data/` |
| `config.cpp` | `[config]` | the config `GenerateConfig()` writes loads back the same, every option included |
| `hotkey.cpp` | `[hotkey]` | `hotkey::parse()` and `to_string()` |
| `screen_capture.cpp` | `[capture]` | `parse_region()`, `crop_image()`, `crop_to_monitor()` (HiDPI, fractional, negative layouts), `rotate_rgba()` |
| `util.cpp` | `[util]` | `split()`, `parse_hex_rgba()` |
| `plugin_index.cpp` | `[plugins]` | `compare_versions()`, `sha256_file()`, `parse_plugin_index()`, `is_installable()`, `pick_version()` |
| `manifest.cpp` | `[plugins]` | `Manifest` (names, IDs, `oshot-plugin.toml`) and `StateManager` (`state.toml`) |

The `[plugins]` ones are only built with plugins (`DISABLE_PLUGINS=OFF`).

```bash
make BUILD_TESTS=1 test           # or: cmake -B build -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build -V
build/release/tests/oshot_tests   # every test, with a line per image
build/release/tests/oshot_tests "[barcode]" -s   # by tag, printing every check
```

`BUILD_TESTS` is read when the build folder is configured, so add it to an existing one with
`cmake -B build/release -DBUILD_TESTS=ON`.

The OCR tests need `eng.traineddata`, plus `ita` and `jpn` for the language tests (skipped without them). They are
looked up in `$TESSDATA_PREFIX`, else in the folder oshot uses by default; on Windows and macOS set `TESSDATA_PREFIX`.

`main.cpp` makes a temporary folder for the run, with the config the tests use, and points `XDG_CONFIG_HOME` and
`XDG_CACHE_HOME` into it. On Windows the config folder doesn't follow them, so the `StateManager` test is skipped
there rather than writing to the real `state.toml`.

CI (`.github/workflows/tests.yml`) runs them in the Arch Linux container with the `eng`, `ita` and `jpn` models.

## OCR and barcode data

`data/` is a synthetic test set: `ocr/<name>.png` with its text in `ocr/<name>.txt`, `barcode/<name>.png` with one
payload per line in `barcode/<name>.txt` (empty for the `neg_` images, which must decode nothing).

OCR passes when the character error rate (edit distance over the expected length, whitespace runs counted as one
space) is under the test's limit, and the `jitter_<group>_*` images, the same text cropped with different margins,
must all read the same. Barcodes pass when they decode exactly the expected payloads. `bar_code39` and
`bar_ean13_tiny` are known failures, tagged `[!mayfail]`.
