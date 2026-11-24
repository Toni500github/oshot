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

#ifndef _UTIL_COLOR_HPP_
#define _UTIL_COLOR_HPP_

// rgba_t and hex color strings

#include <cstdint>
#include <string>
#include <string_view>

#include "fmt/color.h"

struct ImVec4;

// ------------------------------
// Types
// ------------------------------

/**
 * An RGBA color, packed as 0xRRGGBBAA.
 * Taken from "fmt/color.h" with the addition of alpha,
 * for the places where ImVec4 isn't used.
 **/
// clang-format off
struct rgba_t
{
    constexpr rgba_t() : r(0), g(0), b(0), a(0) {}

    constexpr rgba_t(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_)
        : r(r_), g(g_), b(b_), a(a_) {}

    explicit constexpr rgba_t(uint32_t hex)
        : r((hex >> 24) & 0xFF),
          g((hex >> 16) & 0xFF),
          b((hex >> 8)  & 0xFF),
          a(hex & 0xFF) {}

    constexpr rgba_t(ImVec4 vec);

    // fmt::color is 0xRRGGBB, so it's opaque
    constexpr rgba_t(fmt::color hex)
        : r((uint32_t(hex) >> 16) & 0xFF),
          g((uint32_t(hex) >> 8)  & 0xFF),
          b((uint32_t(hex))       & 0xFF),
          a(0xFF) {}

    // Unpack from other byte orders. ImGui's IM_COL32 is ABGR
    static constexpr rgba_t from_rgba(uint32_t v) { return { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8),  uint8_t(v) }; }
    static constexpr rgba_t from_abgr(uint32_t v) { return { uint8_t(v),       uint8_t(v >> 8),  uint8_t(v >> 16), uint8_t(v >> 24) }; }
    static constexpr rgba_t from_argb(uint32_t v) { return { uint8_t(v >> 16), uint8_t(v >> 8),  uint8_t(v),       uint8_t(v >> 24) }; }
    static constexpr rgba_t from_bgra(uint32_t v) { return { uint8_t(v >> 8),  uint8_t(v >> 16), uint8_t(v >> 24), uint8_t(v) }; }

    [[nodiscard]] constexpr uint32_t to_rgba() const { return uint32_t(r)<<24 | uint32_t(g)<<16 | uint32_t(b)<<8 | a; }
    [[nodiscard]] constexpr uint32_t to_abgr() const { return uint32_t(a)<<24 | uint32_t(b)<<16 | uint32_t(g)<<8 | r; }
    [[nodiscard]] constexpr uint32_t to_argb() const { return uint32_t(a)<<24 | uint32_t(r)<<16 | uint32_t(g)<<8 | b; }
    [[nodiscard]] constexpr uint32_t to_bgra() const { return uint32_t(b)<<24 | uint32_t(g)<<16 | uint32_t(r)<<8 | a; }

    [[nodiscard]] constexpr ImVec4 to_imvec4() const;
    [[nodiscard]] constexpr operator ImVec4() const;

    uint8_t r, g, b, a;
};
// clang-format on

// ------------------------------
// Constants
// ------------------------------

namespace Colors
{
static constexpr rgba_t WHITE(0xFFFFFFFFu);
static constexpr rgba_t RED(0xFF0000FFu);
static constexpr rgba_t GREEN(0x00FF00FFu);
static constexpr rgba_t BLUE(0x0000FFFFu);
static constexpr rgba_t YELLOW(0xFFFF00FFu);
static constexpr rgba_t CYAN(0x00FFFFFFu);
}  // namespace Colors

// ------------------------------
// Functions
// ------------------------------

/**
 * Read 4 RGBA bytes into an rgba_t
 * @param p The first byte (red)
 **/
inline rgba_t load_rgba(const uint8_t* p)
{
    return { p[0], p[1], p[2], p[3] };
}

/**
 * Write an rgba_t as 4 RGBA bytes
 * @param p Where to write the first byte (red)
 * @param c The color to write
 **/
inline void store_rgba(uint8_t* p, const rgba_t& c)
{
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

/**
 * rgba_t literal, e.g. 0xFF0000FF_rgba for opaque red
 * @param v The color packed as 0xRRGGBBAA
 **/
constexpr rgba_t operator""_rgba(unsigned long long v)
{
    return rgba_t(static_cast<uint32_t>(v));
}

/**
 * Parse a hex color string
 * @param hex The color as "#RRGGBB" or "#RRGGBBAA". Without alpha it's opaque
 * @param out Where to store the color, only written on success
 * @return false if `hex` isn't a valid color
 **/
bool parse_hex_rgba(const std::string_view hex, rgba_t& out);

/**
 * parse_hex_rgba(), packed as 0xRRGGBBAA
 * @param hex The color as "#RRGGBB" or "#RRGGBBAA"
 * @param out Where to store the color, only written on success
 * @return false if `hex` isn't a valid color
 **/
bool hexstr_to_col(const std::string_view hex, uint32_t& out);

/**
 * parse_hex_rgba(), as an ImVec4
 * @param hex The color as "#RRGGBB" or "#RRGGBBAA"
 * @param out Where to store the color, only written on success
 * @return false if `hex` isn't a valid color
 **/
bool hexstr_to_imvec4(const std::string_view hex, ImVec4& out);

/**
 * Convert a color to a hex string. The opposite of parse_hex_rgba()
 * @param col The color to convert
 * @return The color as "#rrggbbaa"
 **/
std::string col_to_hexstr(const rgba_t& col);

#endif  // _UTIL_COLOR_HPP_
