# oshot

A simple and lightweight tool for extracting text from a screenshot/image (on the fly)

## Optimization

- **Screen capture uses the fastest available hardware path per platform**: DXGI Desktop Duplication on Windows acquires frames directly from the GPU's front buffer via a staging texture mapped for CPU read, avoiding any GDI software rasterization; XGetImage on X11 takes a direct 32bpp packed-pixel fast path (a single `memcpy`-equivalent row scan), falling back to the `XGetPixel` generic path only when the pixel format does not match the expected mask layout. The screen is then kept as a single RGBA buffer in memory for the entire session; all cropping, annotation rendering, and encoding operate on that buffer without re-capturing.

- The fullscreen overlay is a **borderless windowed surface** rather than exclusive fullscreen, avoiding implicit GPU mode switches and the display state corruption they can leave behind on abnormal exit. Can also be changed via configuration file.

- **OCR, barcode scanning, and font loading are all on-demand**: none are initialized at startup; Tesseract and ZBar are only configured when the user triggers an extraction, and the Tesseract engine instance is reused across extractions within a session, re-initializing only when the model or data path changes. Tesseract page segmentation mode is additionally dispatched in **O(1)** via area and aspect ratio heuristics before OCR runs, avoiding full-page layout analysis on small single-word or single-line regions.

- **Annotation geometry is rendered entirely through ImGui draw lists on the GPU**, with CPU-side pixel rasterization only used when baking annotations into the saved image. The rasterizer uses **Bresenham's line algorithm**, O(max(Δx, Δy)), and a **midpoint circle algorithm**, O(radius), rather than naive scanline fills.

- **Pencil stroke point reduction uses a squared-distance threshold**, comparing `dx²+dy² > 4.0` rather than computing `sqrt`, keeping the per-mouse-move check O(1) with no transcendental function call and keeping the point array small regardless of how long the user draws.

- **Grayscale conversion for barcode scanning uses integer-only ITU-R BT.601 weights** `(77r + 150g + 29b) >> 8` rather than floating-point luminance coefficients, keeping the O(w×h) pixel walk entirely in the integer pipeline.

- **Monitor detection under XRandR is O(monitors)**, querying only the list of attached outputs and comparing cursor coordinates against their bounding rectangles, never touching pixel data.

- **The font cache is an O(log n) lookup** keyed on `(path, size)`, ensuring repeated renders of the same annotated text at the same size never trigger atlas rebuilds or filesystem access.

- **Image downscaling for oversized sources uses `stbir_resize_uint8_linear`**, a cache-friendly separable linear filter that processes pixels in a single O(w×h) pass with SIMD-friendly memory access patterns.

- VSync is **user-configurable**, allowing the overlay to drop to uncapped rendering on systems where the compositor introduces latency.

- External dependencies are kept minimal: image loading, resizing, and writing use single-header **stb libraries** compiled only into the translation units that need them, with no transitive system library requirements beyond what the platform already provides.

## Dependencies
### Linux
Package names may vary by distribution and package manager.
If a package is not found, try searching by its base name (e.g., `libglfw3-dev` → `glfw`).

- `libx11-dev`
- `libxcb-dev`
- `libpng-dev`
- `libglfw3-dev`
- `libtesseract` (including necessary language models, e.g `tesseract-ocr-eng`)
- `libzbar-dev`
- `libappindicator3-dev`
- `grim` (Wayland only)
- `wl-clipboard` (Wayland only)

## Building
### Make
```bash
$ git clone https://github.com/Toni500github/oshot/
$ cd oshot/
$ make
# You can move it in a custom directory in your $PATH (preferably in the home)
$ ./build/release/oshot
```
### CMake (ninja)
```bash
$ git clone https://github.com/Toni500github/oshot/
$ cd oshot/
$ mkdir build2 && cd build2
$ cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
$ ninja
# You can move it in a custom directory in your $PATH (preferably in home)
$ ./oshot
```

## Downloading additional language models
Tesseract uses separate language model files (`.traineddata`) for each language.  
You can store these files anywhere you like, as long as the path is configured correctly.

1. Download the required language model(s) from the official Tesseract repository:  
   https://github.com/tesseract-ocr/tessdata

2. Place the downloaded `.traineddata` files in one of the following locations:
   - The `models/` directory next to the `oshot` binary (recommended)
   - Or any other directory of your choice (configure the path in the config file)

3. Configure the language data path in `config.toml`:
   - Windows: `%APPDATA%/oshot/config.toml`
   - Linux: `~/.config/oshot/config.toml`

   Set the `ocr-path` variable to the directory containing the `.traineddata` files.
   Example:
   ```toml
   # Works on windows too
   ocr-path = "~/Downloads/oshot/models"
   ```

## Troubleshooting
### Windows
If when starting oshot, it starts to flick a screen black (or it won't launch), try the following steps:
1. Download [MesaForWindows-x64-20.1.8.7z](https://downloads.fdossena.com/geth.php?r=mesa64-latest)
2. Extract the `opengl32.dll` file into the directory where `oshot.exe` is located
3. Try to launch it again

### Linux
- If oshot gives linking library errors, when trying to run it, then try to use the AppImage release instead.
- If you try to copy the text into the clipboard and doesn't work, try to launch `oshot --tray` and then from the system tray you launch oshot

If still errors, please open an [Issue](https://github.com/Toni500github/oshot/issues) and take a screenshot/paste the text of the error appearing in the console when executing oshot

## Usage
https://github.com/user-attachments/assets/8367490a-f7b0-4320-86e9-8ef8764a56b5
<!--https://github.com/user-attachments/assets/ac505de6-0818-4d67-bb51-064d86f1f970-->

### Useful use-case (old footage)
https://github.com/user-attachments/assets/800f50b3-95a6-47c4-b9bd-5a90c35941b2
