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
inline constexpr std::string_view oshotpm_help = (R"(Usage: oshotpm [GLOBAL OPTIONS]... <COMMAND> [OPTIONS]...
Manage plugins for oshot.

Terms:
    REPO:
        - With install: a Git repository, either URL or local path, both containing plugins and a 'oshot-plugin.toml' manifest.
        - With enable OR disable: the name of a repository already installed (as listed with 'oshotpm list').

Examples:
    Install a plugin repository from GitHub:
        oshotpm install https://github.com/Toni500github/oshot-test-plugins
    Disable a plugin from an installed repository:
        oshotpm disable toni-test/plugins
    Uninstall an entire plugin repository:
        oshotpm uninstall tonis-plugins

Commands:
    help <COMMAND>                     Show help for a specific command. As right now just 'install' and 'list'
    install [OPTIONS] <REPO(s)>...     Install one or more plugin repository from a Git repo or local path.
    uninstall <REPO(s)>...             Uninstall one or more installed plugin repository.
    enable <REPO/PLUGIN>...            Enable one or more plugins from an installed repository.
    disable <REPO/PLUGIN>...           Disable one or more plugins from an installed repository.
    list [-v]                          Show all plugins installed via state.toml.
    update                             Update and upgrade all repositories
    gen-manifest                       Generate a template 'oshotpm.toml' file.

Global options:
    -h, --help          Show this help message.
    -V, --version       Show version and build information.
    -D, --dialogs       Enable GUI dialogs
)");

inline constexpr std::string_view oshotpm_help_install = (R"(Usage: oshotpm install [OPTIONS] <REPO>...

Install one or more plugin repositories. If a given argument exists on disk,
it is treated as a local directory. Otherwise, it is treated as a Git
repository URL and will be cloned.

All plugins found within the repository will be installed.

Options:
    -f, --force        Force installation, even if already installed.
    -h, --help         Show help for this command.
)");

inline constexpr std::string_view oshotpm_help_list = (R"(Usage: oshotpm list [options]
List all installed plugins.

Options:
    -v, --verbose      Show detailed plugin information.
    -h, --help         Show help for this command.
)");

// oshot

inline constexpr std::string_view oshot_help = (R"(Usage: oshot [OPTIONS]...
Lightweight Screenshot tool to extract text (and more) on the fly.

GENERAL OPTIONS:
    -h, --help                  Print this help menu.
    -V, --version               Print version and other infos about the build.
    -f, --source <PATH>         Path to the image to use as background (use '-' for reading from stdin).
    -C, --config <PATH>         Path to the config file to use (default: ~/.config/oshot/config.toml).
    -O, --override <OPTION>     Override a config option (e.g "delay=200", "default.ocr-model='jpn'").
    -d, --delay <MILLIS>        Delay the app before acquiring the screenshot by milliseconds.
                                Won't affect if using the -f flag

    --instant-copy/save         Instant copy/save a selection once it's selected
    --gen-config [<PATH>]       Generate default config file. If PATH is omitted, saves to default location.
                                Prompts before overwriting.

    -g, --gui                   Only launch the GUI.
    -t, --tray                  Only launch system tray.
)");

// default oshot config
inline constexpr std::string_view AUTOCONFIG = R"#([default]
# Default Path to where we'll use all the '.traineddata' models.
# The TESSDATA_PREFIX environment variable supersedes this.
ocr-path = "{}"

# Default OCR model.
ocr-model = "{}"

# GitHub repository from where we are going to
# download an OCR '.traineddata' model.
# The models must be on the root directory of the repository
ocr-repo-downlaod = "{}"

# Delay the app before acquiring a screenshot (in milliseconds)
# Doesn't affect if opening external image (i.e. -f flag)
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

# Allow the extracted output to be editable.
allow-text-edit = {}

# Display the text tools window (OCR, Bar/QR code scan) at startup.
show-text-tools = {}

# Prefer using config variables over environment variable.
config-over-env = {}

# Consider annotations when scanning (true)
# or only when saving the selection (false).
annotations-in-text-tools = {}

# Copy image shortcut to use.
# true: CTRL+C
# false: CTRL+SHIFT+C
ctrl-c-copy-img = {}

# Fonts to use for the application. Can be an absolute path, or just a name.
# You can combine multiple fonts for multiple language support.
# for example, using "Roboto-Regular.ttf" and "RobotoCJK-Regular.ttc" for Chinese, Japanese, and Korean support alongside English support.
# If empty, or non-existent (or commented out), oshot will use the default font for ImGUI.
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
# Case Sensitive.
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

inline constexpr std::string_view AUTO_MANIFEST = R"([repository]
# The repository name.
# It must contain only alpha-numeric characters and symbols such as '-' or '_'
name = "repo_name"

# The repository git clone / homepage url.
url = "https://github.com/user/repo"

# Platform-dependent packages required to build all plugins in this repository.
# NOTE: This will ONLY tell the user which packages are needed;
#       it will not actually install them.
#
# Use the "all" key for dependencies common to every platform.
# Current platforms: all, linux, macos, windows
[dependencies]
all     = ["pkg-config", "cmake"]
linux   = ["wayland-protocols", "xorg-dev"]
macos   = ["gtk+3"]

# From now on, each table that is neither "repository" nor "dependencies" will be treated as a plugin entry.
# The table's name (e.g. "test-plugin" below) is used directly as the plugin's name,
# so it still must conform to alpha-numeric characters and symbols such as '-' or '_'
[test-plugin]

# The plugin ID.
# It must contain only alpha-numeric characters and symbols such as '-' or '_'
# and at least one dot '.'
id = "dev.test-plugin"

# The plugin description.
description = "Test plugin"

# The plugin authors.
authors = ["user1", "friend_user1"]

# The plugin SPDX License Identifiers (not validated)
licenses = ["MIT", "GPL-2.0"]

# Which platforms this specific plugin supports.
# Use ["all"] if it builds on every platform, otherwise list only the ones it supports.
# If the current platform isn't in this list, the plugin is skipped (with a warning) during install/update.
# Current platforms: all, linux, macos, windows
platforms = ["all"]

# The directory where the final plugin output (the built library) shall be found.
# It is checked right after running build-steps below; oshotpm then picks up the
# one library found in this directory and installs it as one of this plugin's library.
# The path is relative to the repository root unless absolute.
output-dir = "build/plugin-dir/"

# A list of commands to be executed for building the plugin.
# All commands are executed in a single shared shell session, in a concatened && sequences,
# so environment variables, `cd`, and other shell state persist across steps.
# Commands are executed in order and stop at the first failure.
build-steps = [
    "make -C ./test-plugin-entry/",
    "mkdir -p ./build/plugin-dir/",
    "mv ./test-plugin-entry/library.so ./build/plugin-dir/library.so"
]
)";

#endif  // !_TEXTS_HPP_
