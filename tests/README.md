# Tests

Tests of the text extraction pipeline (`OcrAPI` and `ZbarAPI` in `src/text_extraction.cpp`), with
[Catch2](https://github.com/catchorg/Catch2) v3.16.1's amalgamated files in `catch2/`.

```bash
make BUILD_TESTS=1 test           # or: cmake -B build -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build
build/release/tests/oshot_tests   # every test
build/release/tests/oshot_tests "[barcode]" -s   # by tag, printing every check
```

`BUILD_TESTS` is read when the build folder is configured, so add it to an existing one with
`cmake -B build/release -DBUILD_TESTS=ON`.

The OCR tests need `eng.traineddata`, plus `ita` and `jpn` for the language tests (skipped without them). They are
looked up in `$TESSDATA_PREFIX`, else in the folder oshot uses by default; on Windows and macOS set `TESSDATA_PREFIX`.

CI (`.github/workflows/tests.yml`) runs them in the Arch Linux container with the `eng`, `ita` and `jpn` models.

## Data

`data/` is a synthetic test set: `ocr/<name>.png` with its text in `ocr/<name>.txt`, `barcode/<name>.png` with one
payload per line in `barcode/<name>.txt` (empty for the `neg_` images, which must decode nothing).

OCR passes when the character error rate (edit distance over the expected length, whitespace runs counted as one
space) is under the test's limit, and the `jitter_<group>_*` images, the same text cropped with different margins,
must all read the same. Barcodes pass when they decode exactly the expected payloads. `bar_code39` and
`bar_ean13_tiny` are known failures, tagged `[!mayfail]`.
