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

#ifndef _UTIL_STRINGS_HPP_
#define _UTIL_STRINGS_HPP_

// String helpers and byte units

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// ------------------------------
// Types
// ------------------------------

/* A number of bytes in a unit, from auto_divide_bytes() and divide_bytes() */
struct byte_units_t
{
    std::string unit;
    double      num_bytes;
};

/* The base of a byte unit, for auto_divide_bytes() */
// NOLINTNEXTLINE(performance-enum-size)
enum class ByteUnit
{
    Kibibyte = 1024,
    Kilobyte = 1000,
};

struct Parser
{
    Parser(const std::string_view src) : src{ src } {}

    bool TryRead(const char c)
    {
        if (IsEof())
            return false;

        if (src[pos] == c)
        {
            ++pos;
            return true;
        }

        return false;
    }

    char ReadChar()
    {
        if (IsEof())
            return 0;

        ++pos;
        return src[pos - 1];
    }

    // The next char without consuming it, 0 at the end
    [[nodiscard]] char Peek() const { return IsEof() ? 0 : src[pos]; }

    // Read up to the next of the `Stops` chars (left unread) or the end.
    // A template so the comparisons inline, string_view::find_first_of() is slower on short strings
    template <char... Stops>
    std::string_view ReadUntil()
    {
        const size_t start = pos;
        while (pos < src.size() && ((src[pos] != Stops) && ...))
            ++pos;
        return src.substr(start, pos - start);
    }

    [[nodiscard]] bool IsEof() const { return pos >= src.length(); }

    void Rewind(const size_t count = 1) { pos -= std::min(pos, count); }

    const std::string_view src;
    size_t                 pos = 0;
};

// ------------------------------
// Functions
// ------------------------------

/**
 * Replace every instance of a substring, in place
 * @param str The string to modify
 * @param from The substring to replace
 * @param to What to replace `from` with
 * @return A copy of the modified `str`
 **/
std::string replace_str(std::string& str, const std::string_view from, const std::string_view to);

/**
 * Split a string at every delimiter. Empty fields are kept, except after a trailing delimiter
 * @param text The string to split
 * @param delim The delimiter
 **/
std::vector<std::string> split(const std::string_view text, const char delim);

/**
 * Make the whole string uppercase (ASCII only)
 * @param str The string to convert
 **/
std::string str_toupper(std::string str);

/**
 * Make the whole string lowercase (ASCII only)
 * @param str The string to convert
 **/
std::string str_tolower(std::string str);

/**
 * Convert bytes to the biggest unit that keeps the number at least 1
 * @param num The number of bytes
 * @param base Kibibyte for KiB/MiB/..., Kilobyte for kB/MB/...
 * @param maxprefix The biggest unit to go up to (e.g. "MiB"), empty for none
 **/
byte_units_t auto_divide_bytes(const double num, const ByteUnit base, const std::string_view maxprefix = "");

/**
 * Convert bytes to a specific unit
 * @param num The number of bytes
 * @param prefix The unit to convert to ("GiB", "MB", ...). An unknown one gives bytes
 **/
byte_units_t divide_bytes(const double num, const std::string_view prefix);

#endif  // _UTIL_STRINGS_HPP_
