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
#include <vector>

#include "util.hpp"

struct region_t
{
    int x{};
    int y{};
    int width{};
    int height{};
};

struct capture_result_t
{
    std::vector<uint8_t> data;  // RGBA
    int                  w = 0;
    int                  h = 0;

    std::span<const uint8_t> view() const { return data; }
    std::span<uint8_t>       view() { return data; }
};

struct monitor_t
{
    char     name[64];
    region_t geo;
    bool     done;
    void*    handle = nullptr;  // wl_output*
};

enum class SessionType
{
    Wayland,
    X11,
    Windows,
    MacOS,
    KDE,
    Unknown
};

Result<capture_result_t> capture_full_screen_x11();
Result<capture_result_t> capture_full_screen_wayland();
Result<capture_result_t> capture_full_screen_spectacle();
Result<capture_result_t> capture_full_screen_windows();
Result<capture_result_t> capture_full_screen_macos();

SessionType              get_session_type();
Result<capture_result_t> crop_to_monitor(const capture_result_t&     full,
                                         const std::deque<region_t>& monitors,
                                         const region_t&             target);

#endif  // !_SCREEN_CAPTURE_HPP_
