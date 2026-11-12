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

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include "backends.hpp"
#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "util.hpp"

using namespace spdlog;

// ------------------------------
// X11
// ------------------------------

// The geometry of the monitor under the cursor from XRandR (X11 or XWayland), nullopt without an X server
std::optional<region_t> get_cursor_monitor_xrandr(Display* display)
{
    const char* disp_env = std::getenv("DISPLAY");
    if (!disp_env || disp_env[0] == '\0')
        return std::nullopt;

    Display* dpy = display ? display : XOpenDisplay(disp_env);
    if (!dpy)
        return std::nullopt;

    Window       root = DefaultRootWindow(dpy);
    Window       root_ret, child_ret;
    int          root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;
    XQueryPointer(dpy, root, &root_ret, &child_ret, &root_x, &root_y, &win_x, &win_y, &mask);

    int             nmon     = 0;
    XRRMonitorInfo* monitors = XRRGetMonitors(dpy, root, True, &nmon);

    region_t mon_size;
    bool     found = false;
    if (monitors && nmon > 0)
    {
        // Default to first monitor in case the cursor position is ambiguous
        mon_size.x = monitors[0].x;
        mon_size.y = monitors[0].y;
        mon_size.w = monitors[0].width;
        mon_size.h = monitors[0].height;
        found      = true;

        debug("Found {} monitor{}", nmon, nmon > 1 ? "s" : "");
        for (int i = 0; i < nmon; ++i)
        {
            debug("Iterating monitor #{}", i);
            const int mx = monitors[i].x, my = monitors[i].y;
            const int mw = monitors[i].width, mh = monitors[i].height;
            if (root_x >= mx && root_x < mx + mw && root_y >= my && root_y < my + mh)
            {
                mon_size.x = mx;
                mon_size.y = my;
                mon_size.w = mw;
                mon_size.h = mh;
                debug("XRandR capturing: monitor {}x{}+{}+{} (cursor at {},{}) ", mw, mh, mx, my, root_x, root_y);
                break;
            }
        }
        XRRFreeMonitors(monitors);
    }

    if (!display)
        XCloseDisplay(dpy);

    if (found)
        return mon_size;
    return std::nullopt;
}

static std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height)
{
    std::vector<uint8_t> out(size_t(width) * height * 4);

    // 32bpp packed pixels, faster than XGetPixel()
    if (image && image->bits_per_pixel == 32 && image->data && image->bytes_per_line >= width * 4)
    {
        if ((image->red_mask == 0x00ff0000ul) && (image->green_mask == 0x0000ff00ul) &&
            (image->blue_mask == 0x000000fful))
        {
            for (int y = 0; y < height; ++y)
            {
                const uint32_t* px =
                    reinterpret_cast<const uint32_t*>(image->data) + size_t(y) * image->bytes_per_line / 4;

                uint8_t* dst = out.data() + size_t(y) * width * 4;
                for (int x = 0; x < width; ++x)
                {
                    rgba_t c = rgba_t::from_argb(px[x]);
                    c.a      = 0xFF;
                    store_rgba(dst + static_cast<ptrdiff_t>(x * 4), c);
                }
            }
            return out;
        }
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            rgba_t c = rgba_t::from_argb(XGetPixel(image, x, y));
            c.a      = 0xFF;
            store_rgba(out.data() + (static_cast<ptrdiff_t>(y * width + x) * 4), c);
        }
    }
    return out;
}

Result<capture_result_t> capture_full_screen_x11()
{
    capture_result_t result;

    Display* display = XOpenDisplay(nullptr);
    if (!display)
    {
        spdlog::warn("Failed to open X display");
        return capture_full_screen_portal();
    }

    Window root = DefaultRootWindow(display);

    region_t capture{ .x = 0 };
    if (std::optional<region_t> op_capture = get_cursor_monitor_xrandr(display))
    {
        capture = *op_capture;
    }
    else
    {
        // Fall back to root window dimensions (old single-monitor behavior)
        spdlog::warn("XRandR returned no monitors, falling back to root window size");
        XWindowAttributes attrs;
        XGetWindowAttributes(display, root, &attrs);
        capture.w = attrs.width;
        capture.h = attrs.height;
    }

    XImage* image = XGetImage(display,
                              root,
                              capture.x,
                              capture.y,
                              static_cast<unsigned int>(capture.w),
                              static_cast<unsigned int>(capture.h),
                              AllPlanes,
                              ZPixmap);
    if (!image)
    {
        spdlog::warn("Failed to capture screen image with X11");
        XCloseDisplay(display);
        return capture_full_screen_portal();
    }

    result.data = ximage_to_rgba(image, capture.w, capture.h);
    result.w    = capture.w;
    result.h    = capture.h;

    XDestroyImage(image);
    XCloseDisplay(display);

    return Ok(std::move(result));
}
