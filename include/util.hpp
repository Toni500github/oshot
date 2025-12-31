#ifndef _UTIL_HPP_
#define _UTIL_HPP_

#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "fmt/chrono.h"
#include "fmt/color.h"

#if ENABLE_NLS
/* here so it doesn't need to be included elsewhere */
#include <libintl.h>
#include <locale.h>
#define _(str) gettext(str)
#else
#define _(s) (char*)s
#endif

#ifdef __linux__
std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height);
#endif

std::vector<uint8_t> ppm_to_rgba(uint8_t* ppm, int width, int height);
std::vector<uint8_t> rgba_to_ppm(const std::vector<uint8_t>& rgba, int width, int height);
std::string          replace_str(std::string str, const std::string_view from, const std::string_view to);

/*
 * Get the user config directory
 * either from $XDG_CONFIG_HOME or from $HOME/.config/
 * @return user's config directory
 */
std::filesystem::path getHomeConfigDir();

/*
 * Get the oshot config directory
 * where we'll have "config.toml"
 * from getHomeConfigDir()
 * @return oshot's config directory
 */
std::filesystem::path getConfigDir();

/* Replace special symbols such as ~ and $ (at the begging) in std::string
 * @param str The string
 * @param dont Don't do it
 * @return The modified string
 */
std::string expandVar(std::string ret, bool dont = false);

#define BOLD_COLOR(x) (fmt::emphasis::bold | fmt::fg(x))
template <typename... Args>
void error(const std::string_view fmt, Args&&... args) noexcept
{
    fmt::print(stderr,
               BOLD_COLOR(fmt::rgb(fmt::color::red)),
               "[{}] ERROR: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
}

template <typename... Args>
void die(const std::string_view fmt, Args&&... args) noexcept
{
    fmt::print(stderr,
               BOLD_COLOR(fmt::rgb(fmt::color::red)),
               "[{}] FATAL: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
    std::exit(1);
}

template <typename... Args>
void debug(const std::string_view fmt, Args&&... args) noexcept
{
#if DEBUG
    fmt::print(BOLD_COLOR((fmt::rgb(fmt::color::hot_pink))),
               "[{}] [DEBUG]: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
#endif
}

template <typename... Args>
void warn(const std::string_view fmt, Args&&... args) noexcept
{
    fmt::print(BOLD_COLOR((fmt::rgb(fmt::color::yellow))),
               "[{}] WARNING: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
}

template <typename... Args>
void info(const std::string_view fmt, Args&&... args) noexcept
{
    fmt::print(BOLD_COLOR((fmt::rgb(fmt::color::cyan))),
               "[{}] INFO: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
}

inline void ctrl_d_handler(const std::istream& cin)
{
    if (cin.eof())
        die(_("Exiting due to CTRL-D or EOF"));
}

/** Ask the user a yes or no question.
 * @param def The default result
 * @param fmt The format string
 * @param args Arguments in the format
 * @returns the result, y = true, n = false, only returns def if the result is def
 */
template <typename... Args>
bool askUserYorN(bool def, const std::string_view fmt, Args&&... args)
{
    const std::string& inputs_str = fmt::format(" [{}]: ", def ? "Y/n" : "y/N");
    std::string        result;
    fmt::print(fmt::runtime(fmt), std::forward<Args>(args)...);
    fmt::print("{}", inputs_str);

    while (std::getline(std::cin, result) && (result.length() > 1))
        fmt::print(BOLD_COLOR(fmt::rgb(fmt::color::yellow)), "Please answear y or n,{}", inputs_str);

    ctrl_d_handler(std::cin);

    if (result.empty())
        return def;

    if (def ? std::tolower(result[0]) != 'n' : std::tolower(result[0]) != 'y')
        return def;

    return !def;
}

#endif  // !_UTIL_HPP_
