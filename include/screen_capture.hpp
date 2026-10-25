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

#ifndef _SCREEN_CAPTURE_HPP_
#define _SCREEN_CAPTURE_HPP_

#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
#include <vector>

#include "util.hpp"

struct region_t
{
    int x{};
    int y{};
    int w{};
    int h{};

    bool operator==(const region_t&) const = default;
};

// An RGBA image, either a screenshot or an opened file
struct capture_result_t
{
    std::vector<uint8_t> data;  // RGBA, w * h * 4 bytes
    int                  w = 0;
    int                  h = 0;

    [[nodiscard]] std::span<const uint8_t> view() const { return data; }
    std::span<uint8_t>                     view() { return data; }
};

// A Wayland output, from wl_get_monitors()
struct monitor_t
{
    // The output's name, e.g. "DP-1"
    std::array<char, 64> name;

    // Logical position and size in the compositor's space
    region_t geo;

    // The compositor sent all of the output's properties
    bool done;

    // struct wl_output*
    void* handle = nullptr;

    // WL_OUTPUT_TRANSFORM_* value
    int32_t transform = 0;
};

// Which capture backend to use, from get_session_type()
enum class SessionType : std::uint8_t
{
    Wayland,
    X11,
    Windows,
    MacOS,
    KDE,  // Wayland or X11 under KDE Plasma, captured with spectacle
    Unknown
};

// ------------------------------
// Capture
// ------------------------------

/**
 * Take a screenshot with the backend for a session
 * @param session The session type, usually from get_session_type()
 **/
Result<capture_result_t> capture_full_screen(SessionType session);

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
 * Capture the monitor under the cursor with KDE's `spectacle`.
 * Falls back to capture_full_screen_wayland()
 **/
Result<capture_result_t> capture_full_screen_spectacle();

/* Capture the monitor under the cursor with DXGI desktop duplication, falling back to GDI */
Result<capture_result_t> capture_full_screen_windows();

/**
 * Capture the display under the cursor with CoreGraphics.
 * macOS asks for the Screen Recording permission on first use
 **/
Result<capture_result_t> capture_full_screen_macos();

/**
 * Detect the platform and, on Linux and the BSDs, the desktop session.
 * KDE is only reported outside a Flatpak, which can't run the host's spectacle
 **/
SessionType get_session_type();

// ------------------------------
// Image operations
// ------------------------------

/**
 * Copy a region out of an image
 * @param src The image
 * @param region The region to copy, which must lie fully inside `src`
 **/
Result<capture_result_t> crop_image(const capture_result_t& src, const region_t& region);

/**
 * Crop one monitor out of a capture of the whole desktop.
 * The capture may be scaled compared to the layout (e.g. HiDPI)
 * @param full The capture of the whole desktop
 * @param monitors The geometry of every monitor, in layout coordinates
 * @param target The monitor to crop out, in layout coordinates
 **/
Result<capture_result_t> crop_to_monitor(const capture_result_t&     full,
                                         const std::deque<region_t>& monitors,
                                         const region_t&             target);

/**
 * Rotate an image by a multiple of 90 degrees clockwise.
 * Used for rotated Wayland outputs, whose content the compositor's capture doesn't rotate
 * @param src The image
 * @param quarter_turns_cw How many quarter turns (1 = 90, 2 = 180, 3 = 270)
 **/
capture_result_t rotate_rgba(const capture_result_t& src, int quarter_turns_cw);

/**
 * Parse a region from --selection
 * @param str "WxH+X+Y" (X11 geometry, slop) or "X,Y WxH" (slurp)
 **/
Result<region_t> parse_region(std::string_view str);

#if OSHOT_FREEDESKTOP
/**
 * List the Wayland outputs with their logical geometry (xdg-output)
 * @return The outputs, empty if there's no Wayland display
 **/
std::deque<monitor_t> wl_get_monitors();
#endif

#endif  // !_SCREEN_CAPTURE_HPP_
