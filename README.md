<p align="center">
  <img src="oshot.png" width="10%" />
</p>

<h1 align="center">oshot</h1>

<p align="center">
Fast, lightweight, cross-platform screenshot OCR.<br>
Capture any screen region, extract text, and decode QR/barcodes locally.
</p>

<p align="center">
  <img src="https://img.shields.io/github/languages/top/Toni500github/oshot?logo=cplusplusbuilder&label=%20" />
  <img src="https://img.shields.io/github/actions/workflow/status/Toni500github/oshot/build.yml" />
</p>

# Overview

oshot is a native screenshot utility focused on **text extraction** and **barcode decoding**.

Select any part of your screen, run OCR or QR/barcode scan, and copy the result directly to your clipboard. No uploads, no browser workflow, no bulky image editor.

Useful for:

- copying text from applications that block selection
- extracting text inside VMs or remote desktops
- reading text from videos, slides, or images
- decoding QR codes shown on screen
- replacing heavier screenshot tools when you only need capture + extraction

Supports **Windows**, **Linux**, and **macOS**.

# Demo

https://github.com/user-attachments/assets/8367490a-f7b0-4320-86e9-8ef8764a56b5

https://github.com/user-attachments/assets/800f50b3-95a6-47c4-b9bd-5a90c35941b2

# Features

## Capture

- Region selection with resize handles
- Full-screen select (`Ctrl+A`)
- Capture delay
- Open external image files
- Borderless overlay window

## OCR & Barcode

- OCR powered by Tesseract
- QR/barcode decoding via ZBar
- Copy extracted text to clipboard
- Editable OCR output before copying
- Multiple language models supported
- Configurable OCR segmentation mode

## Annotation

- Arrow, line, rectangle, circle
- Filled shapes
- Numbered counters
- Text tool
- Pencil / freehand drawing
- Color picker with zoom loupe
- Undo support

## Customization

- Preferences window
- Light / dark / auto / classic themes
- Font configuration
- Output filename format
- Behavior and rendering options

# Installation

#### Linux

Download from the latest release page:

- `oshot-appimage-<version>.zip`
- `oshot-linux-<version>.zip`
- `oshot-debian-package-<version>.deb`

https://github.com/Toni500github/oshot/releases/latest

#### Arch Linux (AUR)

```bash
yay -S oshot-bin
````

### Wayland

Install:

* `grim`
* `wl-clipboard`

Required for screen capture and clipboard support.

## Windows

Download:

* `oshot-windows-<version>.zip`

Includes bundled English OCR data.

[https://github.com/Toni500github/oshot/releases/latest](https://github.com/Toni500github/oshot/releases/latest)

Extract and run:

```bash
oshot.exe
```

## macOS

Download:

* `oshot-macos-arm64-<version>.zip` (Apple Silicon)
* `oshot-macos-x86_64-<version>.zip` (Intel)

Includes `.dmg` bundle and bundled English OCR data.

[https://github.com/Toni500github/oshot/releases/latest](https://github.com/Toni500github/oshot/releases/latest)

# Usage

## Quick Start

1. Launch `oshot`
2. Select a screen region
3. Annotate if needed
4. Open the text tools panel
5. Run OCR or QR/barcode scan
6. Copy the result

## Command Line

| Command           | Description                     |
| ----------------- | ------------------------------- |
| `oshot`           | Launch interactive overlay      |
| `oshot --tray`    | Start minimized to tray (Linux) |
| `oshot -f <path>` | Open image file                 |

## Keyboard Shortcuts

| Shortcut       | Action                    |
| -------------- | ------------------------- |
| `Ctrl+C`       | Copy selection as image   |
| `Ctrl+Shift+C` | Alternate image copy mode |
| `Ctrl+S`       | Save selection as PNG     |
| `Ctrl+A`       | Select full screen        |
| `Ctrl+Z`       | Undo                      |
| `Ctrl+G`       | Toggle handles            |
| `Ctrl+E`       | Toggle text editing       |
| `Esc`          | Close                     |

# OCR Languages

oshot ships with English OCR data by default.

To add more languages:

1. Download `.traineddata` files from the Tesseract tessdata repository
   [https://github.com/tesseract-ocr/tessdata](https://github.com/tesseract-ocr/tessdata)

2. Place them in:

* `models/` beside the executable
* any custom directory

3. Configure the OCR path in preferences or `config.toml`

## Config Paths

| Platform | Path                                              |
| -------- | ------------------------------------------------- |
| Windows  | `%APPDATA%/oshot/config.toml`                     |
| Linux    | `~/.config/oshot/config.toml`                     |
| macOS    | `~/Library/Application Support/oshot/config.toml` |

Example:

```toml
ocr-path = "~/Downloads/oshot/models"
```

# Troubleshooting

## Windows

### Flicker on launch / app fails to start

Use Mesa software OpenGL:

[https://downloads.fdossena.com/geth.php?r=mesa64-latest](https://downloads.fdossena.com/geth.php?r=mesa64-latest)

Extract `opengl32.dll` beside `oshot.exe`.

## Linux

### Missing runtime libraries

Use the AppImage build.

### Clipboard issues

```bash
oshot --tray
```

Then launch capture from the tray icon.

### Wayland

Install:

* `grim`
* `wl-clipboard`

## Still having issues?

Open an issue with:

* operating system + version
* exact error message
* steps to reproduce

[https://github.com/Toni500github/oshot/issues](https://github.com/Toni500github/oshot/issues)

# Build from Source

Requires a **C++20** compiler.

## Core Dependencies

* glfw3
* tesseract
* leptonica
* zbar
* OpenGL
* libpng

## Linux Extras

* libx11
* libxcb
* libxrandr
* gio-2.0
* gtk+-3.0
* libappindicator3 / ayatana-appindicator3

## macOS Frameworks

* Cocoa
* Metal
* QuartzCore
* CoreGraphics
* IOKit

## Windows Libraries

* d3d11
* dxgi
* shcore
* ws2_32

## Install Dependencies

### Ubuntu / Debian

```bash
sudo apt-get install -y \
  build-essential pkg-config cmake \
  libx11-dev libxcb1-dev libxrandr-dev \
  libglfw3-dev libpng-dev \
  libtesseract-dev libleptonica-dev \
  libzbar-dev \
  libgtk-3-dev libayatana-appindicator3-dev
```

### Arch Linux

```bash
sudo pacman -S --needed \
  base-devel cmake \
  libx11 libxcb libxrandr \
  glfw libpng \
  tesseract leptonica \
  zbar \
  gtk3 libappindicator-gtk3
```

### macOS (Homebrew)

```bash
brew install cmake glfw tesseract leptonica zbar
```

### Windows (MSYS2 UCRT64)

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

## Build

### Make

```bash
git clone https://github.com/Toni500github/oshot/
cd oshot
make
./build/release/oshot
```

### CMake + Ninja

```bash
git clone https://github.com/Toni500github/oshot/
cd oshot
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
./oshot
```

# Contributing

Contributions are welcome.

* Bug reports
* Focused pull requests
* Performance improvements
* Feature proposals (open an issue first)

[https://github.com/Toni500github/oshot/issues](https://github.com/Toni500github/oshot/issues)
