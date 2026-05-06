<p align="center">
  <img src="oshot.png" width="10%"/>
</p>
<h1 align="center">oshot</h1>
<p align="center">
A <b>fast, lightweight, cross-platform screenshot OCR tool</b><br>
Capture any area of your screen, instantly extract text, and decode QR/barcodes.
</p>

<p align="center">
  <img src="https://img.shields.io/github/languages/top/Toni500github/oshot?logo=cplusplusbuilder&label=%20" />
  <img src="https://img.shields.io/github/actions/workflow/status/Toni500github/oshot/build.yml" />
</p>

## What is oshot?

oshot is a **lightweight, cross-platform screenshot tool that extracts text and decodes barcodes from your screen instantly**. Select any region of your screen, and oshot copies the recognized text or QR/barcode content directly to your clipboard — no saving files, no opening a separate editor, no cloud uploads, no heavy dependencies.

oshot is built for real-world productivity: copy error messages from locked-down applications, grab text from virtual machines when shared clipboard fails, decode QR codes displayed on your monitor, or extract text from remote desktops and streaming sessions.

oshot works on **Windows**, **Linux**, and **macOS**.

## Demo

<!-- Demo GIFs/videos from the original README -->
https://github.com/user-attachments/assets/8367490a-f7b0-4320-86e9-8ef8764a56b5

https://github.com/user-attachments/assets/800f50b3-95a6-47c4-b9bd-5a90c35941b2

## Why use oshot?

Most screenshot tools are general-purpose: they capture, save, and maybe annotate. oshot is **purpose-built for extraction**. That means:

- **Instant text extraction**: select an area, click "Extract Text", and recognized text appears ready to copy. No saving, no cropping dialog, no drag-to-upload workflow.
- **Built-in QR/barcode decoding**: scan QR codes, barcodes, and other symbologies directly from your screen without needing your phone.
- **No cloud, no telemetry**: all OCR and barcode decoding runs locally on your machine using Tesseract and ZBar.
- **Minimal memory footprint**: the OCR engine, barcode scanner, and fonts load on-demand and are reused across extractions within a session.
- **Runs where clipboard sharing breaks**: virtual machines, remote desktops, locked-down applications. If you can see text on your screen, oshot can extract it.
- **Full annotation toolkit**: draw arrows, rectangles, circles, text, and more right in the overlay before saving.
- **GUI preferences editor**: customize settings and themes through a built-in preferences window, no manual config file editing required.

## Features

### Text & barcode extraction
- **OCR** using Tesseract with on-demand engine initialization and session reuse
- **QR code and barcode decoding** using ZBar, with symbology detection details
- **Text copied to clipboard** with a single click
- **Editable output** — toggle text editing to correct OCR results in-place before copying
- **Multiple language support** via standard Tesseract `.traineddata` models
- **PSM optimization** — choose page segmentation mode (word, line, block, auto) for better accuracy

### Annotation tools
- **Arrow**, **Line**, **Rectangle** (outline & filled), **Circle** (outline & filled)
- **Counter bubble** — numbered annotation markers
- **Text** with customizable font, size, and color
- **Pencil** for freehand drawing
- **Color picker** with magnifying loupe for precise color selection
- **Undo** support for annotations
- GPU-rendered during editing, CPU-rasterized into saved images

### Capture & UI
- **Cross-platform** (Windows, Linux, macOS)
- **Region selection** with draggable handles and move support
- **Select all** (Ctrl+A) for full-screen extraction
- **Borderless windowed overlay** — no exclusive fullscreen mode switching
- **System tray support** on Linux for clipboard workarounds
- **Load external images** for extraction (File → Open Image)
- **Configurable capture delay** for capturing transient UI elements

### Customization
- **GUI preferences window** — edit settings without touching config files
- **Theme system** — choose auto/dark/light/classic base styles, with per-color overrides and style variable tuning (rounding, borders)
- **Custom fonts** — configure multiple fonts for UI and annotation text
- **Customizable output filename format** with chrono-based datetime specifiers
- **Configurable VSync**, annotation rendering in scans, copy shortcuts, and more

## Installation / Downloads

### Linux

**Prebuilt packages:**
- **AppImage**: `oshot-appimage-<version>.zip` from the [releases page](https://github.com/Toni500github/oshot/releases/latest)
- **Binary tarball**: `oshot-linux-<version>.zip` from the [releases page](https://github.com/Toni500github/oshot/releases/latest)
- **Debian package**: `oshot-debian-package-<version>.deb` from the [releases page](https://github.com/Toni500github/oshot/releases/latest)
- **AUR** (Arch Linux):
  ```bash
  yay -S oshot-bin
  ```

**Wayland note:** On Wayland, oshot requires `grim` for screen capture and `wl-clipboard` for clipboard access. Install both packages separately.

### Windows
Download `oshot-windows-<version>.zip` from the [releases page](https://github.com/Toni500github/oshot/releases/latest). Extract and run `oshot.exe`. A bundled `eng.traineddata` model is included.

### macOS
Download `oshot-macos-arm64-<version>.zip` (Apple Silicon) or `oshot-macos-x86_64-<version>.zip` (Intel) from the [releases page](https://github.com/Toni500github/oshot/releases/latest). Includes a `.dmg` bundle and the English language model.


## Common real-world use cases

**Copy text from virtual machines**  \
When host/guest clipboard sharing doesn't work (common in minimal VMs, older OSes, or certain hypervisor configurations), select the text on screen and extract it.

**Grab text from locked-down applications**  \
Some apps prevent text selection or don't allow copy-paste. oshot extracts whatever is visible on screen.

**Decode QR codes and barcodes shown on screen**  \
No need to reach for your phone — select the barcode region and the decoded content goes straight to your clipboard.

**Extract text from remote desktops**  \
When working over RDP, VNC, or streaming services where clipboard integration is unreliable.

**Pull text from images, presentations, and video**  \
Paused video frames, presentation slides, or photos — extract visible text in seconds.

**Lightweight alternative to heavy screenshot suites**  \
If you primarily need text extraction and barcode scanning, oshot replaces tools that bundle full image editors and cloud sync clients.

## Usage

1. **Launch oshot** from your application menu or terminal.
2. **Select a region** of the screen by clicking and dragging. Use Ctrl+A to select the entire screen.
3. **Annotate** if desired using the toolbar (arrows, rectangles, circles, text, pencil).
4. **Open the Text tools panel** (toggle button in toolbar) to access OCR and barcode extraction.
5. **Click "Extract Text"** for OCR or **"Extract Text"** under QR/Bar Decode for barcodes.
6. **Click "Copy Text"** to copy results to your clipboard.

### Command-line options

| Command | Description |
|---------|-------------|
| `oshot` | Launch the interactive overlay |
| `oshot --tray` | Start minimized to system tray (Linux workaround for clipboard issues) |
| `oshot -f <path>` | Open a specific image file |

### Keyboard shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+C` | Copy selected region as image (or `Ctrl+Shift+C` if "Use CTRL+C to copy image" is disabled) |
| `Ctrl+S` | Save selected region as PNG |
| `Ctrl+A` | Select entire screen |
| `Ctrl+Z` | Undo last annotation |
| `Ctrl+G` | Toggle selection handles |
| `Ctrl+E` | Toggle output text editing |
| `Esc` | Cancel / close overlay |

### Preferences window

oshot includes a built-in preferences editor accessible from **Edit → Preferences** in the text tools menu. It has two tabs:

- **Defaults** — configure OCR path, default OCR model, capture delay, theme, fonts, VSync, output format, and various behavioral toggles
- **Theme** — customize the application appearance with style variable overrides (rounding, borders) and per-color adjustments

Changes can be saved and applied, or discarded. A confirmation dialog prevents accidental loss of unsaved changes.

## OCR language models (Tesseract)

oshot uses Tesseract for OCR. By default, it bundles English language data. To add more languages:

1. **Download language models** (`.traineddata` files) from the [official Tesseract tessdata repository](https://github.com/tesseract-ocr/tessdata).
2. **Place the `.traineddata` files** in one of these locations:
   - The `models/` directory next to the `oshot` binary (recommended)
   - Any custom directory of your choice
3. **Configure the path** in the Preferences window or directly in `config.toml`:
   - **Windows**: `%APPDATA%/oshot/config.toml`
   - **Linux**: `~/.config/oshot/config.toml`
   - **macOS**: `~/Library/Application Support/oshot/config.toml`

   Set `ocr-path` to the directory containing your `.traineddata` files:
   ```toml
   ocr-path = "~/Downloads/oshot/models"
   ```

You can also change the default OCR model and optimize the page segmentation mode via **Edit → Optimize OCR for...**.

## Troubleshooting

### Windows

**Screen flickering on launch or application won't start:**

1. Download [MesaForWindows-x64-20.1.8.7z](https://downloads.fdossena.com/geth.php?r=mesa64-latest).
2. Extract the `opengl32.dll` file into the directory where `oshot.exe` is located.
3. Launch oshot again.

### Linux

**Linking library errors at runtime:**  
Use the AppImage release instead of the native binary.

**Clipboard does not work:**  
Launch oshot with the system tray flag: `oshot --tray`, then trigger a capture from the system tray icon.

**Wayland-specific notes:**  
On Wayland, oshot relies on `grim` for screen capture and `wl-clipboard` for clipboard access. Install both:
- `grim`
- `wl-clipboard`

### All platforms

If problems persist, please open an [Issue](https://github.com/Toni500github/oshot/issues) and include:
- The exact error message from the terminal
- Your OS and version
- Steps to reproduce

## Build from source

### Dependencies

oshot requires a **C++20** compiler and the following libraries:

#### All platforms
| Dependency | Purpose |
|-----------|---------|
| `glfw3` | Window creation and OpenGL context |
| `tesseract` | OCR engine |
| `leptonica` | Image processing (Tesseract dependency) |
| `zbar` | Barcode and QR code decoding |
| `OpenGL` | Rendering backend |
| `libpng` | PNG read/write support |

#### Linux only
| Dependency | Purpose |
|-----------|---------|
| `libx11` | X11 screen capture and display |
| `libxcb` | X11 protocol C bindings |
| `xrandr` | Monitor detection |
| `gio-2.0` | GLib I/O abstractions |
| `gtk+-3.0` | System tray support |
| `libappindicator3` (or `ayatana-appindicator3`) | System tray indicator |

#### macOS only
| Dependency | Purpose |
|-----------|---------|
| `Cocoa` | Native windowing |
| `Metal` | GPU rendering backend |
| `QuartzCore` | Display and compositing |
| `CoreGraphics` | Screen capture |
| `IOKit` | System information |

#### Windows only
| Dependency | Purpose |
|-----------|---------|
| `d3d11` | DirectX 11 for screen capture |
| `dxgi` | DXGI Desktop Duplication API |
| `shcore` | DPI awareness |
| `ws2_32` | Sockets (single-instance lock) |

#### Installing dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get install -y \
  build-essential pkg-config cmake \
  libx11-dev libxcb1-dev libxrandr-dev \
  libglfw3-dev libpng-dev \
  libtesseract-dev libleptonica-dev \
  libzbar-dev \
  libgtk-3-dev libayatana-appindicator3-dev
```

**Arch Linux:**
```bash
sudo pacman -S --needed \
  base-devel cmake \
  libx11 libxcb libxrandr \
  glfw libpng \
  tesseract leptonica \
  zbar \
  gtk3 libappindicator-gtk3
```

**macOS (Homebrew):**
```bash
brew install cmake glfw tesseract leptonica zbar
```

**Windows (MSYS2 UCRT64):**
```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-glfw \
  mingw-w64-ucrt-x86_64-libpng \
  mingw-w64-ucrt-x86_64-tesseract-ocr \
  mingw-w64-ucrt-x86_64-zbar
```

### Make

```bash
git clone https://github.com/Toni500github/oshot/
cd oshot
make
./build/release/oshot
```

### CMake (Ninja)

```bash
git clone https://github.com/Toni500github/oshot/
cd oshot
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
./oshot
```

You can move the resulting binary to any directory in your `$PATH`.

## Why it's fast

oshot is deliberately optimized to stay out of your way. Capture, recognize, done. Behind the scenes, several design choices keep everything snappy:

**Zero-warmup startup**: OCR, barcode scanning, and font loading are fully on-demand. Tesseract and ZBar are configured only when extraction is triggered, and the Tesseract engine is reused across extractions within the same session.

**Hardware-accelerated screen capture**: on Windows, frames come directly from the GPU's front buffer via DXGI Desktop Duplication, bypassing software rasterization entirely. On X11, pixel acquisition takes a fast-path `memcpy` row scan for 32bpp packed formats, falling back to `XGetPixel` only for non-standard layouts. The captured buffer is held once and reused for the entire session.

**Smart OCR dispatch**: Tesseract's page segmentation mode can be selected based on region aspect ratio and area heuristics, avoiding expensive full-page layout analysis on single-line or single-word selections.

**Integer-only image processing**: grayscale conversion for barcode scanning uses integer ITU-R BT.601 coefficients `(77r + 150g + 29b) >> 8` instead of floating-point luminance, keeping the pixel walk entirely in the integer pipeline.

**Efficient annotation rendering**: drawing primitives use Bresenham's line algorithm O(max(Δx, Δy)) and the midpoint circle algorithm O(radius). Pencil stroke simplification uses a squared-distance threshold (`dx²+dy² > 4.0`) to avoid a `sqrt` per mouse-move event and keep point arrays compact.

**No exclusive fullscreen**: the overlay is a borderless windowed surface, avoiding GPU mode switches and the display corruption they can leave on abnormal exit.

**Minimal, embeddable dependencies**: image loading, resizing, and writing use single-header stb libraries compiled only where needed. Downscaling of oversized images uses `stbir_resize_uint8_linear`, a cache-friendly separable linear filter.

**Font caching**: an O(log n) lookup keyed on `(path, size)` ensures repeated text renders at the same size never re-trigger font atlas rebuilds or filesystem reads.

**Monitor detection** is O(monitors), comparing cursor coordinates against bounding rectangles without touching pixel data.

## Contributing

Contributions are welcome — whether it's bug reports, feature ideas, or pull requests.

- **Issues**: Check the [issue tracker](https://github.com/Toni500github/oshot/issues) for existing discussions or open a new one.
- **Pull Requests**: Keep them focused. Performance-related changes are especially appreciated; include benchmarks or rationale when possible.
- **Feature requests**: Open an issue first to discuss the idea before investing time in implementation.
