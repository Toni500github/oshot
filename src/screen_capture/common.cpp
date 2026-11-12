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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "backends.hpp"
#include "platform.hpp"
#include "screen_capture.hpp"
#include "util.hpp"
#include "util/result.hpp"

#if !OSHOT_WINDOWS
#  include <unistd.h>
#endif

#if !OSHOT_WINDOWS
std::string create_temp_png()
{
    std::string tmppath = "/tmp/oshot_XXXXXX.png";
    const int   fd      = mkstemps(tmppath.data(), 4);
    if (fd < 0)
        return {};
    close(fd);
    return tmppath;
}
#endif

Result<capture_result_t> capture_full_screen(SessionType session)
{
    switch (session)
    {
#if OSHOT_FREEDESKTOP
        case SessionType::X11:     return capture_full_screen_x11();
        case SessionType::Wayland: return capture_full_screen_wayland();
        case SessionType::KDE:     return capture_full_screen_kde();
#elif OSHOT_WINDOWS
        case SessionType::Windows: return capture_full_screen_windows();
#elif OSHOT_MACOS
        case SessionType::MacOS: return capture_full_screen_macos();
#endif
        default: return Err("Unknown platform");
    }
}

Result<capture_result_t> crop_image(const capture_result_t& src, const region_t& region)
{
    if (region.w <= 0 || region.h <= 0 || region.x < 0 || region.y < 0 || region.x + region.w > src.w ||
        region.y + region.h > src.h)
        return Err(
            "Region {}x{}+{}+{} is outside the {}x{} image", region.w, region.h, region.x, region.y, src.w, src.h);

    capture_result_t out{ .data = std::vector<uint8_t>(size_t(region.w) * region.h * 4), .w = region.w, .h = region.h };
    const size_t     row = size_t(region.w) * 4;
    for (int y = 0; y < region.h; ++y)
        std::memcpy(out.data.data() + y * row, src.data.data() + ((size_t(region.y + y) * src.w) + region.x) * 4, row);

    return Ok(std::move(out));
}

Result<region_t> parse_region(const char* str)
{
    region_t r{};
    char     tail = 0;

    // %c catches trailing garbage; a full match leaves it unassigned
    if (std::sscanf(str, "%dx%d+%d+%d%c", &r.w, &r.h, &r.x, &r.y, &tail) != 4 &&
        std::sscanf(str, "%d,%d %dx%d%c", &r.x, &r.y, &r.w, &r.h, &tail) != 4)
        return Err("Invalid region '{}', expected WxH+X+Y or \"X,Y WxH\"", str);

    if (r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0)
        return Err("Invalid region '{}': the size must be positive and the position not negative", str);

    return Ok(r);
}

SessionType get_session_type()
{
#if OSHOT_WINDOWS
    return SessionType::Windows;
#elif OSHOT_MACOS
    return SessionType::MacOS;
#else
    const char* xdg     = std::getenv("XDG_CURRENT_DESKTOP");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* x11     = std::getenv("DISPLAY");
    const char* kde     = std::getenv("KDE_FULL_SESSION");

    // A Flatpak can't reach KWin's D-Bus interface or run the host's spectacle, so KDE goes
    // through the portal (or X11) there
    if (!g_is_flatpak)
    {
        if (xdg && strstr(xdg, "KDE") != nullptr)
            return SessionType::KDE;
        if (kde && kde[0] != '\0')
            return SessionType::KDE;
    }

    xdg = std::getenv("XDG_SESSION_TYPE");

    if (xdg && strcmp(xdg, "wayland") == 0)
        return SessionType::Wayland;
    if (wayland && wayland[0] != '\0')
        return SessionType::Wayland;

    if (x11 && x11[0] != '\0')
        return SessionType::X11;
    if (xdg && strcmp(xdg, "x11") == 0)
        return SessionType::X11;
#endif

    return SessionType::Unknown;
}

Result<capture_result_t> crop_to_monitor(const capture_result_t&     full,
                                         const std::deque<region_t>& monitors,
                                         const region_t&             target)
{
    if (monitors.empty())
        return Err("No monitor list provided");

    int min_x = monitors[0].x, min_y = monitors[0].y;
    int max_x = monitors[0].x + monitors[0].w, max_y = monitors[0].y + monitors[0].h;
    for (const region_t& m : monitors)
    {
        min_x = std::min(min_x, m.x);
        min_y = std::min(min_y, m.y);
        max_x = std::max(max_x, m.x + m.w);
        max_y = std::max(max_y, m.y + m.h);
    }
    const auto layout_w = float(max_x - min_x);
    const auto layout_h = float(max_y - min_y);
    if (layout_w <= 0 || layout_h <= 0)
        return Err("Degenerate monitor layout");

    const float scale_x = float(full.w) / layout_w;
    const float scale_y = float(full.h) / layout_h;

    const int crop_x = std::clamp(int(std::lround(float(target.x - min_x) * scale_x)), 0, full.w);
    const int crop_y = std::clamp(int(std::lround(float(target.y - min_y) * scale_y)), 0, full.h);
    const int crop_w = std::clamp(int(std::lround(float(target.w) * scale_x)), 0, full.w - crop_x);
    const int crop_h = std::clamp(int(std::lround(float(target.h) * scale_y)), 0, full.h - crop_y);

    if (crop_w <= 0 || crop_h <= 0)
        return Err("Computed crop rect is empty");

    capture_result_t out;
    out.w = crop_w;
    out.h = crop_h;
    out.data.resize(size_t(crop_w) * crop_h * 4);

    for (int row = 0; row < crop_h; ++row)
    {
        const uint8_t* src = full.data.data() + (size_t(crop_y + row) * full.w + crop_x) * 4;
        uint8_t*       dst = out.data.data() + size_t(row) * crop_w * 4;
        std::memcpy(dst, src, size_t(crop_w) * 4);
    }

    return Ok(std::move(out));
}

capture_result_t rotate_rgba(const capture_result_t& src, int turns_cw)
{
    turns_cw = ((turns_cw % 4) + 4) % 4;
    if (turns_cw == 0)
        return src;

    capture_result_t out;
    out.w = (turns_cw % 2 == 0) ? src.w : src.h;
    out.h = (turns_cw % 2 == 0) ? src.h : src.w;
    out.data.resize(size_t(out.w) * out.h * 4);

    for (int y = 0; y < src.h; ++y)
    {
        for (int x = 0; x < src.w; ++x)
        {
            int dx = 0, dy = 0;
            switch (turns_cw)
            {
                case 1:  // 90 CW
                    dx = src.h - 1 - y;
                    dy = x;
                    break;
                case 2:  // 180
                    dx = src.w - 1 - x;
                    dy = src.h - 1 - y;
                    break;
                case 3:  // 270 CW
                    dx = y;
                    dy = src.w - 1 - x;
                    break;
                default:;
            }
            const uint8_t* psrc = src.data.data() + (size_t(y) * src.w + x) * 4;
            uint8_t*       pdst = out.data.data() + (size_t(dy) * out.w + dx) * 4;
            std::memcpy(pdst, psrc, 4);
        }
    }
    return out;
}
