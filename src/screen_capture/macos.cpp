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

#include <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "backends.hpp"
#include "fmt/format.h"
#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "stb_image.h"
#include "tiny-process-library/process.hpp"
#include "util.hpp"

using namespace spdlog;

// ------------------------------
// macOS
// ------------------------------

// The OS automatically prompts for Screen Recording permission on first use.
// Returns the 1-based screencapture display index (-D flag) for the monitor that
// currently contains the cursor, or 1 (main display) if the query fails.
static int cursor_display_index()
{
    // Get current cursor position via CoreGraphics (no special permissions needed).
    CGEventRef event  = CGEventCreate(nullptr);
    CGPoint    cursor = CGEventGetLocation(event);
    CFRelease(event);

    // Find which display(s) contain that point.
    CGDirectDisplayID hit    = kCGNullDirectDisplay;
    uint32_t          nfound = 0;
    CGGetDisplaysWithPoint(cursor, 1, &hit, &nfound);

    if (nfound == 0 || hit == kCGNullDirectDisplay)
        return 1;  // fallback: main display

    // screencapture -D uses a 1-based index into the active display list.
    CGDirectDisplayID active[32];
    uint32_t          active_count = 0;
    CGGetActiveDisplayList(32, active, &active_count);

    for (uint32_t i = 0; i < active_count; ++i)
    {
        if (active[i] == hit)
        {
            debug("macOS capture: display index {} (CGDirectDisplayID {})", i + 1, unsigned(hit));
            return int(i + 1);
        }
    }

    return 1;  // fallback: main display
}

Result<capture_result_t> capture_full_screen_macos()
{
    capture_result_t result;

    const std::string tmppath = create_temp_png();
    if (tmppath.empty())
        return Err("Failed to create temp png");

    // -x        suppress shutter sound
    // -t png    force PNG format
    // -D <n>    capture only display n (1-based index in active display list,
    //           matching the monitor that currently contains the cursor)
    const std::string       display_idx = fmt::to_string(cursor_display_index());
    TinyProcessLib::Process proc({ "screencapture", "-x", "-t", "png", "-D", display_idx.c_str(), tmppath }, "");

    const int exit_code = proc.get_exit_status();
    if (exit_code != 0)
    {
        unlink(tmppath.c_str());
        return Err(
            "screencapture failed (exit code {}). Please check Screen Recording permission in System Settings -> "
            "Privacy & Security",
            exit_code);
    }

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load(tmppath.c_str(), &w, &h, &comp, STBI_rgb_alpha);
    unlink(tmppath.c_str());  // clean up regardless

    if (!rgba)
        return Err("Failed to decode screenshot PNG: {}", STBI_ERROR);

    result.w = w;
    result.h = h;
    result.data.assign(rgba, rgba + size_t(w) * h * 4);
    stbi_image_free(rgba);

    return Ok(std::move(result));
}
