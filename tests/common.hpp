#ifndef _TESTS_COMMON_HPP_
#define _TESTS_COMMON_HPP_

#include <fcntl.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "fmt/os.h"
#include "text_extraction.hpp"

// The OCR engine every test shares, so a model loads once; main() ends it before exiting
OcrAPI& test_ocr();

// A folder main() creates for this run and removes at the end. XDG_CONFIG_HOME and
// XDG_CACHE_HOME point inside it, so get_config_dir() does too outside Windows
const std::filesystem::path& test_temp_dir();

inline std::string read_file(const std::filesystem::path& path)
{
    fmt::file   f(path.string(), fmt::file::RDONLY);
    std::string out(size_t(f.size()), '\0');
    size_t      len = 0;
    while (len < out.size())
    {
        const size_t n = f.read(out.data() + len, out.size() - len);
        if (n == 0)
            break;
        len += n;
    }
    // Windows reads in text mode, without the \r of each \r\n
    out.resize(len);
    return out;
}

// Write a file byte for byte, creating its folder
inline void write_file(const std::filesystem::path& path, const std::string_view content)
{
#ifdef _WIN32
    // Not in text mode, which turns \n into \r\n
    constexpr int binary = _O_BINARY;
#else
    constexpr int binary = 0;
#endif
    std::filesystem::create_directories(path.parent_path());
    fmt::file f(path.string(), fmt::file::WRONLY | fmt::file::CREATE | fmt::file::TRUNC | binary);
    f.write(content.data(), content.size());
}

#endif  // !_TESTS_COMMON_HPP_
