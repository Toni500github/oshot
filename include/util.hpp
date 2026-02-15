#ifndef _UTIL_HPP_
#define _UTIL_HPP_

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "fmt/base.h"
#include "fmt/chrono.h"
#include "fmt/color.h"

enum class SavingOp;

#if defined(__linux__)
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#elif defined(_WIN32)
#  include <combaseapi.h>
#  include <knownfolders.h>
#  include <shlobj.h>
#  include <windows.h>
#endif

#if ENABLE_NLS
#  include <libintl.h>
#  include <locale.h>
#  define _(str) gettext(str)
#else
#  define _(s) (char*)s
#endif

// shotout to the better c++ server for these helper structs
template <typename T = bool>
struct Ok
{
    T value;
};

template <typename E = std::string>
struct Err
{
    E value;
};

template <typename T = Ok<>, typename E = Err<>>
class Result
{
    std::variant<T, E> value;

public:
    template <typename U>
    Result(Ok<U> const& v) : value(std::in_place_index<0>, v.value)
    {}
    template <typename U>
    Result(Ok<U>&& v) : value(std::in_place_index<0>, std::move(v.value))
    {}

    template <typename U>
    Result(Err<U> const& e) : value(std::in_place_index<1>, e.value)
    {}
    template <typename U>
    Result(Err<U>&& e) : value(std::in_place_index<1>, std::move(e.value))
    {}

    bool     ok() const { return std::holds_alternative<T>(value); }
    T&       get() { return std::get<T>(value); }
    E&       error() { return std::get<E>(value); }
    const T& get() const { return std::get<T>(value); }
    const E& error() const { return std::get<E>(value); }
};

// custom structs for fmt::format
template <typename T>
struct fmt::formatter<Ok<T>>
{
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const Ok<T>& p, FormatContext& ctx) const
    {
        return fmt::format_to(ctx.out(), "{}", p.value);
    }
};

template <typename E>
struct fmt::formatter<Err<E>>
{
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const Err<E>& p, FormatContext& ctx) const
    {
        return fmt::format_to(ctx.out(), "{}", p.value);
    }
};

struct capture_result_t;

extern int   g_lock_sock;
extern int   g_scr_w, g_scr_h;
extern FILE* g_fp_log;

#ifdef __linux__
std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height);
#endif

std::string replace_str(std::string& str, const std::string_view from, const std::string_view to);
std::string select_image();

bool acquire_tray_lock();

std::filesystem::path get_font_path(const std::string& font);
std::filesystem::path get_lang_font_path(const std::string& lang);
std::filesystem::path get_home_config_dir();
std::filesystem::path get_home_dir();
std::filesystem::path get_config_dir();

Result<capture_result_t> load_image_rgba(const std::string& path);
Result<>                 save_png(SavingOp op, const capture_result_t& img);

void fit_to_screen(capture_result_t& img);
void rgba_to_grayscale(const uint8_t* rgba, uint8_t* result, int width, int height);

int get_screen_dpi();

#define BOLD_COLOR(x) (fmt::emphasis::bold | fmt::fg(x))

template <typename... Args>
inline void error(const std::string_view fmt, Args&&... args) noexcept
{
    fmt::print(g_fp_log,
               BOLD_COLOR(fmt::rgb(fmt::color::red)),
               "[ {:%T} ] ERROR: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
}

template <typename... Args>
[[noreturn]] inline void die(const std::string_view fmt, Args&&... args) noexcept
{
#ifdef _WIN32
    MessageBox(nullptr,
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...).c_str(),
               "Fatal Error",
               MB_ICONERROR | MB_OK);
#endif
    fmt::print(g_fp_log,
               BOLD_COLOR(fmt::rgb(fmt::color::red)),
               "[ {:%T} ] FATAL: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));

    std::exit(1);
}

// helper for getting g_config->File.debug_print
void debug_msg(const std::string_view msg) noexcept;
template <typename... Args>
inline void debug(std::string_view fmtstr, Args&&... args) noexcept
{
    debug_msg(fmt::format(fmt::runtime(fmtstr), std::forward<Args>(args)...));
}

template <typename... Args>
inline void warn(const std::string_view fmt, Args&&... args) noexcept
{
#ifdef _WIN32
    MessageBox(nullptr,
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...).c_str(),
               "Warning",
               MB_ICONWARNING | MB_OK);
#endif
    fmt::print(g_fp_log,
               BOLD_COLOR((fmt::rgb(fmt::color::yellow))),
               "[ {:%T} ] WARNING: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
}

template <typename... Args>
inline void info(const std::string_view fmt, Args&&... args) noexcept
{
#ifdef _WIN32
    MessageBox(nullptr,
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...).c_str(),
               "Info",
               MB_ICONINFORMATION | MB_OK);
#endif
    fmt::print(g_fp_log,
               BOLD_COLOR((fmt::rgb(fmt::color::cyan))),
               "[ {:%T} ] INFO: {}\n",
               std::chrono::system_clock::now(),
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
}

/** Ask the user a yes or no question.
 * @param def The default result
 * @param fmt The format string
 * @param args Arguments in the format
 * @returns the result, y = true, n = false, only returns def if the result is def
 */
template <typename... Args>
inline bool ask_user_yn(bool def, const std::string_view fmt, Args&&... args)
{
#ifdef _WIN32
    int result = MessageBox(NULL,
                            fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...).c_str(),
                            "Confirmation",
                            MB_YESNO | MB_ICONQUESTION);
    return (result == IDYES);
#else
    const std::string_view inputs_str = def ? " [Y/n]:" : " [y/N]:";
    std::string            result;
    fmt::print(fmt::runtime(fmt), std::forward<Args>(args)...);
    fmt::print("{}", inputs_str);

    while (std::getline(std::cin, result) && (result.length() > 1))
        fmt::print(BOLD_COLOR(fmt::rgb(fmt::color::yellow)), "Please answear y or n,{}", inputs_str);

    if (std::cin.eof())
        die(_("Exiting due to CTRL-D or EOF"));

    if (result.empty())
        return def;

    if (def ? std::tolower(result[0]) != 'n' : std::tolower(result[0]) != 'y')
        return def;

    return !def;
#endif
}

#endif  // !_UTIL_HPP_
