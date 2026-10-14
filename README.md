<p align="center">
  <img src="assets/oshot.png" width="10%" />
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

Supports Windows, Linux, macOS and the BSDs (built from source).

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

Get the file for your system from the [latest release](https://github.com/Toni500github/oshot/releases/latest) page.
Each package also comes as a `-plugins` variant, which adds plugin support and the `oshotpm` plugin manager.
**If you don't use plugins, pick the file without `-plugins`.**

### Windows

Download `oshot-windows-setup-<version>.exe` and run it.

- Installs for your user only by default (no admin prompt), with a Start Menu entry and an uninstaller.
- Optionally starts oshot in the system tray when you sign in.
- English OCR data is included. Downloaded models go to `%APPDATA%\oshot\models`.

Press **Alt+Shift+S** anywhere to capture while oshot is in the tray. You can change it in Preferences.

Prefer a portable copy? Download `oshot-windows-<version>.zip`, extract it anywhere and run `oshot.exe`.

>[!NOTE]
>The installer is not code-signed yet, so SmartScreen may warn on first run: click **More info → Run anyway**.

---
### macOS

Download the `.dmg` that matches your Mac:

| Package | Mac | Requires |
| ------- | --- | -------- |
| `oshot-macos-arm64-<version>.dmg` | Apple Silicon (M1 and newer) | macOS 26.4 (Tahoe) or later |
| `oshot-macos-x86_64-<version>.dmg` | Intel | macOS 15 (Sequoia) or later |

Older macOS versions aren't supported yet: the bundled libraries come from Homebrew, which builds them for the macOS of our build machines.

1. Open the `.dmg` and drag **oshot** onto **Applications**.
2. The app isn't notarized yet, so macOS blocks the first launch. Run this once in Terminal:
   ```bash
   xattr -dr com.apple.quarantine /Applications/oshot.app
   ```
   Or open oshot once, then go to **System Settings → Privacy & Security** and click **Open Anyway**.
3. Grant **Screen Recording** permission when asked (System Settings → Privacy & Security → Screen Recording), then reopen oshot.

Press **Cmd+Shift+2** anywhere to capture while oshot is in the menu bar. You can change it in Preferences.

English OCR data is included. Downloaded models go to `~/.config/oshot/models`.
In the `-plugins` builds, the plugin manager is at `/Applications/oshot.app/Contents/MacOS/oshotpm`.

---
### Linux

| Package | Description |
| ------- | ----------- |
| `oshot-x86_64-<version>.AppImage` | Recommended. Self-contained, runs on most distributions |
| `oshot-debian-package-<version>.deb` | Debian, Ubuntu and derivatives |
| `oshot-linux-<version>.zip` | Generic binary, needs the runtime libraries installed (see [Dependencies](#dependencies)) |

**AppImage**

```bash
chmod +x oshot-x86_64-*.AppImage
./oshot-x86_64-*.AppImage
```

The `-plugins` AppImage comes as a zip: it also contains `oshotpm/`, with the plugin manager and its libraries.

**Debian / Ubuntu**

```bash
sudo apt install ./oshot-debian-package-*.deb
```

Use `apt` rather than `dpkg -i`, so the dependencies get installed too.

**Generic zip**

```bash
unzip oshot-linux-*.zip
sudo ./oshot/install.sh               # installs into /usr/local
sudo ./oshot/install.sh --uninstall   # removes it again
```

#### Arch Linux (AUR)

```bash
yay -S oshot-bin
```

#### Wayland

On GNOME and most other desktops, screenshots go through the desktop's screenshot portal (the first capture may ask for permission). KDE uses Spectacle. Where no screenshot portal is available (e.g. some Sway/Hyprland setups), install `grim`.
For copying to the clipboard, install `wl-clipboard`.

#### Capture shortcut

On Linux, oshot doesn't register a global shortcut itself. Bind `oshot --gui` to a key in your desktop's keyboard settings (e.g. **Settings → Keyboard → Custom Shortcuts** on GNOME), or in your window manager's config, for example:

```ini
# Hyprland
bind = SUPER SHIFT, S, exec, oshot --gui
# Sway / i3
bindsym $mod+Shift+s exec oshot --gui
```

## Usage

```
oshot              # captures a screenshot and opens the overlay, then stays in the tray
oshot --gui        # only capture and open the overlay (for keyboard shortcuts)
oshot --tray       # start in the tray without capturing
oshot -f <path>    # open an image file instead of capturing
```

## Build from Source

Requires a C++20 compiler.

### Dependencies

**All platforms:** `glfw3`, `tesseract`, `leptonica`, `zbar`, `OpenGL`, `libpng`

**Linux / BSD extras:** `libx11`, `libxrandr`, `gio-2.0`, `gtk+-3.0`, `dbus-1`, `libappindicator3` / `ayatana-appindicator3`, `wayland`, `wayland-protocols`

**macOS frameworks:** `Cocoa`, `Metal`, `QuartzCore`, `CoreGraphics`, `IOKit`

**Windows:** `d3d11`, `dxgi`, `shcore`, `ws2_32`

#### Ubuntu / Debian

```bash
sudo apt-get install -y \
  build-essential pkg-config cmake \
  libx11-dev libxrandr-dev libgl-dev \
  libglfw3-dev \
  libtesseract-dev libleptonica-dev \
  libzbar-dev \
  libgtk-3-dev libdbus-1-dev libayatana-appindicator3-dev \
  libwayland-dev libwayland-bin wayland-protocols
```

#### Arch Linux

```bash
sudo pacman -S --needed \
  base-devel cmake \
  libx11 libxrandr \
  glfw \
  tesseract leptonica \
  zbar \
  gtk3 libappindicator-gtk3 \
  wayland wayland-protocols
```

#### FreeBSD

```bash
pkg install git cmake pkgconf gmake \
  glfw tesseract zbar gtk3 libappindicator dbus \
  libX11 libXrandr mesa-libs wayland wayland-protocols
```

Other BSDs need the same libraries from their ports. FreeBSD is built in CI; the others aren't tested, so please report build errors.

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
make                       # `gmake` on the BSDs
./build/release/oshot
sudo make install          # into /usr (/usr/local on the BSDs), or PREFIX=<dir>
sudo make uninstall
```

To build and install the Flatpak (without plugin support) instead, with `org.flatpak.Builder` installed from Flathub:

```bash
flatpak run org.flatpak.Builder --user --install --force-clean \
  --state-dir=build/flatpak-state build/flatpak flatpak/io.github.toni500github.oshot.yml
flatpak run io.github.toni500github.oshot
```

`make DISABLE_PLUGINS=1` builds without plugin support. Packagers can use `DESTDIR` with `make install`, or CMake directly; `-DOSHOT_INSTALL_LICENSEDIR=` changes or skips (empty) where the license goes.

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
