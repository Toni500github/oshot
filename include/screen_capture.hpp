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

#include <SDL3/SDL_video.h>

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "platform.hpp"
#include "util/result.hpp"

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

    // Every monitor (grim, the portal), to be cropped with crop_to_display()
    bool whole_desktop = false;

    [[nodiscard]] std::span<const uint8_t> view() const { return data; }
    std::span<uint8_t>                     view() { return data; }
};

// Which capture backend to use, from get_session_type()
enum class SessionType : std::uint8_t
{
    Wayland,
    X11,
    Windows,
    MacOS,
    KDE,  // Wayland or X11 under KDE Plasma, see capture_full_screen_kde()
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
 * Detect the platform and, on Linux and the BSDs, the desktop session.
 * KDE is only reported outside a Flatpak, which can't reach KWin's D-Bus interface
 * or run the host's spectacle
 **/
SessionType get_session_type();

#if OSHOT_FREEDESKTOP
bool kde_on_wayland();
#endif

// Whether the session is Wayland, KDE's included
bool is_wayland_session(SessionType session);

/**
 * The display under the cursor, or the primary one if SDL can't tell.
 * Wayland has no global cursor position, so there it's always the primary one.
 * SDL's video subsystem must be initialized
 **/
SDL_DisplayID get_display_at_cursor();

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
 * crop_to_monitor() with SDL's display layout.
 * SDL's video subsystem must be initialized
 * @param full The capture of the whole desktop
 * @param display The display to crop out
 **/
Result<capture_result_t> crop_to_display(const capture_result_t& full, SDL_DisplayID display);

/**
 * Parse a region from --selection
 * @param str "WxH+X+Y" (X11 geometry, slop) or "X,Y WxH" (slurp)
 **/
Result<region_t> parse_region(const char* str);
#endif  // !_SCREEN_CAPTURE_HPP_
