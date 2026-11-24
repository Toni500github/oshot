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

#include "util/strings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

std::vector<std::string> split(const std::string_view text, const char delim)
{
    std::vector<std::string> vec;

    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find(delim, start);
        vec.emplace_back(text.substr(start, end == text.npos ? text.npos : end - start));
        if (end == text.npos)
            break;
        start = end + 1;
    }

    return vec;
}

byte_units_t auto_divide_bytes(const double num, const ByteUnit base, const std::string_view maxprefix)
{
    double size = num;

    const auto prefixes =
        (base == ByteUnit::Kibibyte
             ? std::to_array({ "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB" })
             : (base == ByteUnit::Kilobyte ? std::to_array({ "B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" })
                                           : std::array<const char*, 9>({ "B" })));

    size_t      counter = 0;
    const auto& max_it  = !maxprefix.empty() ? std::ranges::find(prefixes, maxprefix) : prefixes.end();

    while (counter + 1 < prefixes.size() && size >= double(base))
    {
        if (max_it != prefixes.end() && prefixes[counter] == maxprefix)
            break;
        size /= double(base);
        ++counter;
    }

    return { .unit = std::string(prefixes[counter]), .num_bytes = size };
}

byte_units_t divide_bytes(const double num, const std::string_view prefix)
{
    if (prefix == "B")
        return { .unit = "B", .num_bytes = num };

    // GiB
    // 012
    const std::uint16_t base = (prefix.size() == 3 && prefix[1] == 'i') ? 1024 : 1000;
    const auto prefixes = base == 1024 ? std::to_array({ "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB" })
                                       : std::to_array({ "B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" });

    const auto& it = std::ranges::find(prefixes, prefix);
    if (it == prefixes.end())
        return { .unit = "B", .num_bytes = num };

    const size_t index = std::distance(prefixes.begin(), it);
    const double value = num / std::pow(static_cast<double>(base), index);

    return { .unit = std::string(prefix), .num_bytes = value };
}

std::string replace_str(std::string& str, const std::string_view from, const std::string_view to)
{
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();  // Handles case where 'to' is a substring of 'from'
    }
    return str;
}

std::string str_toupper(std::string str)
{
    for (char& c : str)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    return str;
}

std::string str_tolower(std::string str)
{
    for (char& c : str)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return str;
}
