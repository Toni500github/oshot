<p align="center">
  <img src="oshot.png" width="10%" />
</p>

<h1 align="center">oshot</h1>

<p align="center">
Fast, lightweight, cross-platform screenshot text extractor.<br>
Capture any screen region, extract text, and decode QR/barcodes locally.
</p>

<p align="center">
  <img src="https://img.shields.io/github/languages/top/Toni500github/oshot?logo=cplusplusbuilder&label=%20" />
  <img src="https://img.shields.io/github/actions/workflow/status/Toni500github/oshot/build.yml" />
</p>

Select a screen region, run OCR or scan a QR/barcode, and copy the result to your clipboard. No uploads, no browser workflow, no bulky image editor. Useful for copying text from apps that block selection, extracting content from VMs or remote desktops, reading text in videos or slides, or decoding QR codes on screen.

Supports Windows, Linux, and macOS.

## Demo

https://github.com/user-attachments/assets/8367490a-f7b0-4320-86e9-8ef8764a56b5

https://github.com/user-attachments/assets/800f50b3-95a6-47c4-b9bd-5a90c35941b2

## Features

- Region selection with resize handles, full-screen select (`Ctrl+A`), and capture delay
- OCR via Tesseract with multiple language models and configurable segmentation mode
- In-app OCR model downloader (specify a GitHub link and model name)
- QR/barcode decoding via ZBar
- Editable OCR output before copying
- Annotation tools: arrow, line, rectangle, circle, filled shapes, numbered counters, text, freehand pencil
- Color picker with zoom loupe, undo support
- **Really Customizable** with Preferences window with light/dark/auto/classic or custom themes
- Font configuration, output filename format, behavior and rendering options
- Borderless overlay window, always on top

## Keyboard Shortcuts

| Shortcut | Action |
| -------------- | ----------------------- |
| `Ctrl+C/S` | Copy/Save selection as image |
| `Ctrl+A` | Select full image |
| `Ctrl+Z` | Undo |
| `Ctrl+G` | Toggle handles |
| `Ctrl+E` | Toggle text editing |
| `Esc` | Close |

## Installation
### Linux

>[!NOTE]
For wayland platforms, `grim` and `wl-clipboard` are required for screen capture and clipboard support.
 
Download the package that fits your setup from the [latest release](https://github.com/Toni500github/oshot/releases/latest) page:

| Package | Description |
| ------- | ----------- |
| `oshot-appimage-<version>.zip` | Recommended. Self-contained, no system dependencies required |
| `oshot-debian-package-<version>.deb` | For Debian/Ubuntu and derivatives |
| `oshot-linux-<version>.zip` | Generic binary, requires dependencies to be installed |

#### Arch Linux (AUR)

```bash
yay -S oshot-bin
```

---
### Windows

Download `oshot-windows-<version>.zip` from the [latest release](https://github.com/Toni500github/oshot/releases/latest) page.\
Includes bundled .dlls and English OCR data. Extract and run `oshot.exe`.

---
### macOS

Download from the [latest release](https://github.com/Toni500github/oshot/releases/latest) page:

| Package | Description |
| ------- | ----------- |
| `oshot-macos-arm64-<version>.zip` | Apple Silicon |
| `oshot-macos-x86_64-<version>.zip` | Intel |

Both include a `.dmg` bundle and bundled English OCR data.

## Usage

```
oshot              # captures a screenshot and launches interactive overlay
oshot --tray       # start minimized to tray
oshot -f <path>    # open image file
```

## Build from Source

Requires a C++20 compiler.

### Dependencies

**All platforms:** `glfw3`, `tesseract`, `leptonica`, `zbar`, `OpenGL`, `libpng`

**Linux extras:** `libx11`, `libxcb`, `libxrandr`, `gio-2.0`, `gtk+-3.0`, `libappindicator3` / `ayatana-appindicator3`

**macOS frameworks:** `Cocoa`, `Metal`, `QuartzCore`, `CoreGraphics`, `IOKit`

**Windows:** `d3d11`, `dxgi`, `shcore`, `ws2_32`

#### Ubuntu / Debian

```bash
sudo apt-get install -y \
  build-essential pkg-config cmake \
  libx11-dev libxcb1-dev libxrandr-dev \
  libglfw3-dev libpng-dev \
  libtesseract-dev libleptonica-dev \
  libzbar-dev \
  libgtk-3-dev libayatana-appindicator3-dev
```

#### Arch Linux

```bash
sudo pacman -S --needed \
  base-devel cmake \
  libx11 libxcb libxrandr \
  glfw libpng \
  tesseract leptonica \
  zbar \
  gtk3 libappindicator-gtk3
```

#### macOS (Homebrew)

```bash
brew install cmake glfw tesseract leptonica zbar
```

#### Windows (MSYS2 UCRT64)

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

### Build

```bash
git clone https://github.com/Toni500github/oshot/
cd oshot
make
./build/release/oshot
```

Or with CMake + Ninja:

```bash
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
./oshot
```

## Troubleshooting

### Windows: flicker on launch / app fails to start

Download [MesaForWindows.7z](https://downloads.fdossena.com/geth.php?r=mesa64-latest), extract `opengl32.dll` beside `oshot.exe`, and relaunch.

### Linux: missing/mismatch runtime libraries

Use the AppImage build.

### Linux: clipboard issues

Run `oshot --tray` and launch captures from the tray icon instead.

### NixOS: Tesseract can't find language data

`TESSDATA_PREFIX` must be set explicitly. If tesseract is in `environment.systemPackages`:

```nix
environment.sessionVariables = {
    "TESSDATA_PREFIX" = "${pkgs.tesseract}/share/tessdata";
};
```

If it is in `users.users.<user>.packages`, use home-manager instead:

```nix
systemd.user.sessionVariables = {
    "TESSDATA_PREFIX" = "${pkgs.tesseract}/share/tessdata";
};
```
>[!Note]
>The home-manager solution requires systemd. If `echo $TESSDATA_PREFIX` returns a valid path but OCR still fails, check `ocr-path` in your config file.

### Still broken?

Open an [issue](https://github.com/Toni500github/oshot/issues) with your OS version, the exact error message, and steps to reproduce.

## Contributing
Bug reports, focused pull requests, and feature proposals (open an issue first) are welcome: https://github.com/Toni500github/oshot/issues
