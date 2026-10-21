/*
 * Copyright 2026 Toni500
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _TEXTS_HPP_
#define _TEXTS_HPP_

#include <string_view>

// oshotpm
inline constexpr std::string_view oshotpm_help = (R"(Usage: oshotpm [-h|-V] <COMMAND> [OPTIONS]...
Install and manage oshot plugins.

Commands:
    search [<WORD>...]                List the plugins in the plugin index, or the ones matching all WORDs.
    install [-f] [-y] <SOURCE>...     Install plugins from the plugin index, a git URL, a folder or an archive.
    update [-y]                       Update the plugins installed from the index and from git URLs.
    uninstall <NAME>...               Uninstall a plugin (by ID) or a repository with all its plugins.
    enable <PLUGIN>...                Enable installed plugins.
    disable <PLUGIN>...               Disable installed plugins.
    list [-v]                         List the installed plugins.
    gen-manifest                      Write a template oshot-plugin.toml in the current folder.
    help [<COMMAND>]                  Show the help of a command.

Terms:
    SOURCE      A plugin ID from the plugin index (see `oshotpm search`), a git URL (https://..., git@...,
                or ending in .git), a folder with an oshot-plugin.toml, or a prebuilt archive
                (.zip, .tar.gz/.tgz, .tar.xz/.txz) with one.
    REPOSITORY  Everything one oshot-plugin.toml installs, named after its [repository] name.
    PLUGIN      A plugin ID like io.github.user.plugin, or <REPOSITORY>/<plugin name>.

Examples:
    oshotpm search translate
    oshotpm install io.github.user.translate
    oshotpm install https://github.com/user/oshot-plugins
    oshotpm install ./my-plugin
    oshotpm disable io.github.user.translate
    oshotpm uninstall oshot-plugins

Options:
    -h, --help          Show this help.
    -V, --version       Show version and build information.

Plugins are installed in the "plugins" folder of oshot's config folder, and take effect the next time oshot starts.
The plugin index is https://github.com/Toni500github/oshot-plugin-index ($OSHOT_PLUGIN_INDEX replaces its URL).
)");

inline constexpr std::string_view oshotpm_help_search = (R"(Usage: oshotpm search [<WORD>...]
List the plugins in the plugin index (https://github.com/Toni500github/oshot-plugin-index), or only the ones
whose ID, name, description or tags contain every WORD. Install one with `oshotpm install <ID>`.

Options:
    -v, --verbose      Also show every version, the owners, the license and the homepage.
    -h, --help         Show this help.
)");

inline constexpr std::string_view oshotpm_help_install = (R"(Usage: oshotpm install [OPTIONS] <SOURCE>...
Install the plugins from each SOURCE:
    - a plugin ID from the plugin index: its newest version that works here, prebuilt for this computer if
      available (checked against the index's SHA-256), else built from the reviewed source;
    - a git URL: cloned, then built with the build steps in its oshot-plugin.toml (needs git and the build tools);
    - a local folder with an oshot-plugin.toml: built in place;
    - a prebuilt archive (.zip, .tar.gz/.tgz, .tar.xz/.txz): its libraries are installed, nothing is built.

Options:
    -f, --force        Reinstall a repository that is already installed.
    -y, --yes          Don't ask anything, answer the default.
    -h, --help         Show this help.
)");

inline constexpr std::string_view oshotpm_help_update = (R"(Usage: oshotpm update [OPTIONS]
Install newer versions of the plugins from the plugin index (and warn about withdrawn ones), then fetch every
repository installed from a git URL and rebuild the ones that changed.
Plugins installed from a folder or an archive are updated by installing them again.

Options:
    -y, --yes          Don't ask anything, answer the default.
    -h, --help         Show this help.
)");

inline constexpr std::string_view oshotpm_help_uninstall = (R"(Usage: oshotpm uninstall <NAME>...
Uninstall plugins. NAME is either a plugin ID (io.github.user.plugin), which removes that plugin,
or a repository name, which removes the repository and all its plugins. Their settings are deleted too.

Options:
    -h, --help         Show this help.
)");

inline constexpr std::string_view oshotpm_help_enable = (R"(Usage: oshotpm enable|disable <PLUGIN>...
Enable or disable installed plugins. PLUGIN is a plugin ID (io.github.user.plugin)
or <REPOSITORY>/<plugin name>, as shown by `oshotpm list`.

Options:
    -h, --help         Show this help.
)");

inline constexpr std::string_view oshotpm_help_list = (R"(Usage: oshotpm list [OPTIONS]
List the installed repositories and their plugins.

Options:
    -v, --verbose      Also show authors, licenses, platforms and where each plugin is.
    -h, --help         Show this help.
)");

inline constexpr std::string_view oshotpm_help_gen_manifest = (R"(Usage: oshotpm gen-manifest
Write a commented template oshot-plugin.toml in the current folder.

Options:
    -h, --help         Show this help.
)");

// oshot

inline constexpr std::string_view oshot_help = (R"(Usage: oshot [OPTIONS]...
Lightweight screenshot tool to extract text (and more) on the fly.

GENERAL OPTIONS:
    -h, --help                  Print this help menu.
    -V, --version               Print version and other information about the build.
    -f, --source <PATH>         Path to the image to use as background (use '-' for reading from stdin).
    -C, --config <PATH>         Path to the config file to use (default: ~/.config/oshot/config.toml).
    -O, --override <OPTION>     Override a config option (e.g. "delay=200", "default.ocr-model='jpn'",
                                "fonts=['/path/to/font.ttf']").
    -d, --delay <MILLIS>        Delay the app before acquiring the screenshot by milliseconds.
                                Has no effect with the -f flag.

    --instant-copy/save         Instantly copy/save a selection once it's selected.
    --instant-save=<DIR>        Save it in DIR, named after the "image-out-fmt" option, without asking.

    --extract-text              Print the text (OCR) of the screenshot or -f image, without opening the overlay.
    --extract-barcode           Print the QR codes and barcodes found in it, one per line.
                                With --instant-copy, the result is copied to the clipboard instead of printed.
    --selection <REGION>        Use only this region of the screenshot or -f image, in pixels, as
                                "WxH+X+Y" (slop) or "X,Y WxH" (slurp). Needs --extract-text, --extract-barcode,
                                --instant-copy or --instant-save, and skips the overlay.
                                --instant-save then saves without asking (to DIR, or the last used folder)
                                and prints the path.
    --gen-config [<PATH>]       Generate default config file. If PATH is omitted, saves to default location.
                                Prompts before overwriting.

    -g, --gui                   Only launch the GUI.
    -t, --tray                  Only launch system tray.
)");

// default oshot config
inline constexpr std::string_view AUTOCONFIG = R"#([default]
# Default path to where we'll use all the '.traineddata' models.
# The TESSDATA_PREFIX environment variable supersedes this.
ocr-path = "{}"

# Default OCR model.
ocr-model = "{}"

# GitHub repository from where we are going to
# download an OCR '.traineddata' model.
# The models must be in the root directory of the repository.
ocr-repo-download = "{}"

# Delay the app before acquiring a screenshot (in milliseconds)
# Has no effect when opening an external image (i.e. -f flag)
delay = {}

# Which ImGui color picker widget to use for annotation colors.
# 0 = "Bar - Square"; 1 = "Wheel - Triangle"
color-picker = {}
 
# Alpha (transparency) editing behavior for the annotation color picker.
# 0 = Disabled (alpha not editable)
# 1 = Inline slider
# 2 = Dedicated alpha bar
color-picker-alpha-mode = {}
 
# On some desktop environments (e.g. MATE), the compositor may cause
# the capture window to look grainy or pixelated. Enabling this uses exclusive
# fullscreen mode which bypasses the compositor and fixes it.
# Downside: the window may briefly take over the display on some setups.
real-full-screen = {}

# Controls vertical sync (VSync). When enabled, the capture window renders in sync
# with your monitor's refresh rate, thus being smoother visually but uses slightly more CPU/GPU.
# Disable if the overlay feels sluggish or unresponsive.
vsync = {}

# Linux (X11) only. GLX vendor library used to create the overlay's OpenGL window.
# On laptops with a hybrid NVIDIA GPU, the NVIDIA library wakes up the sleeping
# discrete GPU on every launch, delaying the overlay by 1-2 seconds.
# Set to "mesa" to use only the integrated GPU's driver (this works if your
# X screen is rendered by the integrated GPU, and breaks if it is rendered by NVIDIA).
# Empty = leave it to the system.
# The __GLX_VENDOR_LIBRARY_NAME environment variable supersedes this,
# unless config-over-env is true.
glx-vendor = "{}"

# Allow the extracted output to be editable.
allow-text-edit = {}

# Display the text tools window (OCR, Bar/QR code scan) at startup.
show-text-tools = {}

# Prefer using config variables over environment variables.
config-over-env = {}

# Consider annotations when scanning (true)
# or only when saving the selection (false).
annotations-in-text-tools = {}

# Disable dynamic tools position for legacy placement (same line)
legacy-toolbar-pos = {}

# Copy image shortcut to use.
# true: CTRL+C
# false: CTRL+SHIFT+C
ctrl-c-copy-img = {}

# System-wide shortcut that starts a capture, e.g. "Alt+Shift+S" or "Cmd+Shift+2".
# Modifiers: Ctrl, Shift, Alt (Option), Win/Cmd/Super. Keys: A-Z, 0-9, F1-F12,
# Space, PrintScreen, Insert, Delete, Home, End, PageUp, PageDown, Pause, Tab.
# Registered by the tray instance on Windows and macOS. Set to "" to disable.
# On Linux, bind `oshot --gui` in your window manager/desktop settings instead.
hotkey = "{}"

# Language of the interface, as a code like "it", "ru" or "pt_BR".
# "auto" follows the system language, and English is used when there's no translation for it.
# Translations can also be added or tested by putting a <code>.po file in the locale/
# folder next to this config (see docs/translating.md).
language = "{}"

# Fonts to use for the application. Can be an absolute path, or just a name.
# You can combine multiple fonts for multiple language support.
# for example, using "Roboto-Regular.ttf" and "RobotoCJK-Regular.ttc" for Chinese, Japanese, and Korean support alongside English support.
# If empty, or non-existent (or commented out), oshot will use the system UI font
# (fontconfig's sans-serif on Linux/BSD, Segoe UI on Windows, Helvetica on macOS),
# plus a font for the language's script when it lacks one (e.g. Chinese or Arabic).
# These system fonts also come after the fonts listed here, to fill in the characters they lack.
# ImGui's built-in font is used only when no font is found.
fonts = [{}]

# Extension of the output image when saving/copying.
# Case insensitive.
# Accepts: "png", "jpeg", "bmp", "tga"
image-out-ext = "{}"

# Format of the output image filename when saving.
# The image extension is appended automatically.
# Uses {{fmt}} chrono specifiers. NOTE: 
#    the colon inside {{}} is required: {{:%F}} correct, {{%F}} will error.
#
# Default: "oshot_{{:%F_%H-%M}}"
image-out-fmt = "{}"

# Size format of the capture in the selected image
# extension from image-out-ext, shown under the capture size widget.
# Case sensitive.
# Accepts: "off", "auto", "B", "KiB", "MiB", "KB", "MB", more...
image-out-size-ind = "{}"

# Path to a theme file. Absolute or relative to this config's directory.
theme-file = "{}"
)#";

inline constexpr std::string_view AUTOTHEME = (R"([theme]
smooth-animations = true

# All sections and keys are optional. Omit anything you don't want to override.

# ---------------------------------------------------------------
# Rounding (pixels, 0 = sharp corners, max ~12)
# ---------------------------------------------------------------
[theme.style]
window-rounding = 4.0
frame-rounding  = 4.0
grab-rounding   = 1.5
tab-rounding    = 2.0

# Border width in pixels. 0 = none, 1 = thin line.
window-border = 1.0
frame-border  = 0.0

# ---------------------------------------------------------------
# Color overrides
# Format: "#RRGGBBAA"
# Only the entries you list here are overridden;
# everything else falls back to the base theme.
#
# Full list of valid names:
#   https://github.com/ocornut/imgui/blob/master/imgui.cpp
#   (search for "GetStyleColorName")
# ---------------------------------------------------------------
[theme.colors]

# Main text
Text = "#DCDDE1FF"
TextDisabled = "#666870FF"

# Main application backgrounds
WindowBg = "#0D1015FF"
ChildBg = "#0D1015FF"
PopupBg = "#10131AFF"

# Borders
Border = "#242933FF"

# Title bars
TitleBg = "#090B0FFF"
TitleBgActive = "#5274F0FF"

# Input fields and other framed widgets
FrameBg = "#151920FF"
FrameBgHovered = "#202630FF"
FrameBgActive = "#29313EFF"

# Buttons
Button = "#294CC7FF"
ButtonHovered = "#385DE0FF"
ButtonActive = "#203CA6FF"

# Headers
Header = "#1A1F28FF"
HeaderHovered = "#252C38FF"
HeaderActive = "#303946FF"

# Tabs
Tab = "#10141BFF"
TabHovered = "#252E42FF"
TabSelected = "#294CC7FF"

# Sliders
SliderGrab = "#294CC7FF"
SliderGrabActive = "#5274F0FF"

# Scrollbars
ScrollbarBg = "#07090CFF"
ScrollbarGrab = "#2A303AFF"

# Checkboxes and other selection indicators
CheckMark = "#5274F0FF"

# Menu bar
MenuBarBg = "#090B0FFF"

# Graphs / histogram elements
PlotHistogram = "#5274F0FF"
)");

inline constexpr std::string_view AUTO_MANIFEST = R"(# oshot plugin manifest. See docs/plugins.md in the oshot repository.

[repository]
# The repository name, used by `oshotpm uninstall` and the like.
# 1-64 letters, digits, '-', '_' or '='.
name = "my-plugins"

# The homepage. When installing from a git URL, oshotpm updates from that URL instead.
url = "https://github.com/user/my-plugins"

# Packages needed to build the plugins, only shown to the person installing them
# (oshotpm never installs anything). Keys: all, linux, macos, windows, freebsd, openbsd, netbsd, dragonfly.
[dependencies]
all     = ["cmake", "a C compiler"]

# Every other table is a plugin. The table name is the plugin's name, with the
# same rules as the repository name.
[my-plugin]

# Must be the same as the `id` in the plugin's oshot_plugin_t.
# Lowercase letters, digits, '-', '_' and at least one '.', e.g. "io.github.user.my-plugin".
id = "io.github.user.my-plugin"

description = "What the plugin does"
authors     = ["user"]
licenses    = ["MIT"]  # SPDX identifiers, not validated

# Platforms the plugin supports: ["all"], or some of linux, macos, windows, freebsd, openbsd, netbsd, dragonfly.
# Plugins that don't support the current platform are skipped.
platforms = ["all"]

# Building from source (git URL or folder): each step runs in the repository's root folder,
# with `sh -c` (cmd.exe on Windows), and installing stops at the first failing step.
# The environment has OSHOT_SDK (the folder with include/oshot/oshot_plugin.h, and lib/ on Windows),
# OSHOT_PLATFORM, OSHOT_ARCH, OSHOT_TARGET (e.g. "linux-x86_64") and OSHOT_VERSION.
# build-steps-<platform> (e.g. build-steps-windows) replaces build-steps on that platform.
build-steps = [
    "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release",
    "cmake --build build --config Release",
]

# Where the build steps leave the library, relative to the repository root.
# It must contain exactly one library (.so, .dll or .dylib, .so also on macOS): the plugin.
output-dir = "build/out"

# Prebuilt archives: the library for each target, relative to the archive root.
# Without this table, the library is looked up in output-dir.
#[my-plugin.prebuilt]
#linux-x86_64   = "linux-x86_64/libmy-plugin.so"
#linux-aarch64  = "linux-aarch64/libmy-plugin.so"
#windows-x86_64 = "windows-x86_64/my-plugin.dll"
#macos-aarch64  = "macos-aarch64/libmy-plugin.dylib"
#macos-x86_64   = "macos-x86_64/libmy-plugin.dylib"
)";

#endif  // !_TEXTS_HPP_
