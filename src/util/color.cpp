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

#include "util/color.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "fmt/format.h"
#include "imgui/imgui.h"

constexpr ImVec4 rgba_t::to_imvec4() const
{
    return { float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, float(a) / 255.0f };
}

bool parse_hex_rgba(const std::string_view hex, rgba_t& out)
{
    if (hex.empty() || hex[0] != '#')
        return false;

    if (hex.size() != 7 && hex.size() != 9)
        return false;

    // strtoul() needs a NUL-terminated string
    const std::string s(hex.substr(1));

    // strtoul() also takes a sign, leading spaces and a 0x prefix
    if (!std::ranges::all_of(s, [](const unsigned char c) { return std::isxdigit(c); }))
        return false;

    char*         end    = nullptr;
    unsigned long parsed = std::strtoul(s.c_str(), &end, 16);

    // Ensure the entire string was consumed and no overflow occurred.
    if (end != s.c_str() + s.size() || parsed > 0xFFFFFFFFul)
        return false;

    const auto value = uint32_t(parsed);
    rgba_t     v(hex.size() == 7 ? (value << 8) | 0xFF : value);

    out = v;
    return true;
}

std::string col_to_hexstr(const rgba_t& col)
{
    return fmt::format("#{:02x}{:02x}{:02x}{:02x}", col.r, col.g, col.b, col.a);
}

bool hexstr_to_imvec4(const std::string_view hex, ImVec4& out)
{
    rgba_t c;
    if (!parse_hex_rgba(hex, c))
        return false;
    out = c.to_imvec4();
    return true;
}

bool hexstr_to_col(const std::string_view hex, uint32_t& out)
{
    rgba_t c;
    if (!parse_hex_rgba(hex, c))
        return false;
    out = c.to_rgba();
    return true;
}
