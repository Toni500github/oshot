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

#ifndef _SCREEN_CAPTURE_BACKENDS_HPP_
#define _SCREEN_CAPTURE_BACKENDS_HPP_

// The capture backends, shared by the src/screen_capture/ files. CMake builds only the current platform's ones

#include <optional>
#include <string>

#include "platform.hpp"
#include "screen_capture.hpp"
#include "util/result.hpp"

#if OSHOT_FREEDESKTOP
// Xlib's own typedef, so this header doesn't pull in Xlib and its macros
typedef struct _XDisplay Display;  // NOLINT

/**
 * Capture the monitor under the cursor with XGetImage() + XRandR.
 * Falls back to the xdg-desktop-portal
 **/
Result<capture_result_t> capture_full_screen_x11();

/**
 * Capture the whole desktop through the xdg-desktop-portal,
 * falling back to `grim` (wlroots compositors)
 **/
Result<capture_result_t> capture_full_screen_wayland();

/**
 * Capture under KDE Plasma. An X11 session goes through capture_full_screen_x11(),
 * Wayland tries capture_full_screen_kwin(), then `spectacle` if it's installed,
 * then capture_full_screen_wayland()
 **/
Result<capture_result_t> capture_full_screen_kde();

/**
 * Capture the monitor KWin considers active (by default the one under the cursor)
 * through KWin's org.kde.KWin.ScreenShot2 D-Bus interface, the call `spectacle -m` makes.
 * Before Plasma 6.8 KWin only answers executables whose installed desktop file lists
 * X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2 with an absolute Exec path
 **/
Result<capture_result_t> capture_full_screen_kwin();

/**
 * Capture the monitor under the cursor with KDE's `spectacle`.
 * Falls back to capture_full_screen_wayland()
 **/
Result<capture_result_t> capture_full_screen_spectacle();

/**
 * Capture through the xdg-desktop-portal's Screenshot interface, cropped to the monitor under the cursor
 * when XRandR can tell which one that is
 **/
Result<capture_result_t> capture_full_screen_portal();

/**
 * The geometry of the monitor under the cursor from XRandR (X11 or XWayland)
 * @param display An open display, or nullptr to open $DISPLAY for the query
 * @return nullopt without an X server
 **/
std::optional<region_t> get_cursor_monitor_xrandr(Display* display);
#endif

#if OSHOT_WINDOWS
/* Capture the monitor under the cursor with DXGI desktop duplication, falling back to GDI */
Result<capture_result_t> capture_full_screen_windows();
#endif

#if OSHOT_MACOS
/**
 * Capture the display under the cursor with CoreGraphics.
 * macOS asks for the Screen Recording permission on first use
 **/
Result<capture_result_t> capture_full_screen_macos();
#endif

#if !OSHOT_WINDOWS
/* An empty .png file in /tmp for a tool to write its screenshot to, or "" on failure */
std::string create_temp_png();
#endif

#endif  // _SCREEN_CAPTURE_BACKENDS_HPP_
