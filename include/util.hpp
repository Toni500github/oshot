#ifndef _UTIL_HPP_
#define _UTIL_HPP_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "spdlog/spdlog.h"
#include "fmt/base.h"
#include "fmt/color.h"

namespace fs = std::filesystem;
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

#if defined(_WIN32) || defined(__APPLE__)
#  define OSHOT_TOOL_ON_MAIN_THREAD true
#else
#  define OSHOT_TOOL_ON_MAIN_THREAD false
#endif

// shotout to the better c++ server for these helper structs
template <typename T = bool>
struct Ok
{
    using value_type = T;
    T value;
};
template <typename T>
Ok(T) -> Ok<T>;

template <typename E = std::string>
struct Err
{
    using value_type = E;
    E value;
};
template <typename E>
Err(E) -> Err<E>;

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

    template <typename U = T, typename = typename U::value_type>
    typename U::value_type& get_v()
    {
        return std::get<T>(value).value;
    }

    template <typename U = T, typename = typename U::value_type>
    const typename U::value_type& get_v() const
    {
        return std::get<T>(value).value;
    }

    template <typename U = E, typename = typename U::value_type>
    typename U::value_type& error_v()
    {
        return std::get<E>(value).value;
    }

    template <typename U = E, typename = typename U::value_type>
    const typename U::value_type& error_v() const
    {
        return std::get<E>(value).value;
    }
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

template <typename E>
constexpr size_t idx(E e) noexcept
{
    static_assert(std::is_enum_v<E>);
    return static_cast<size_t>(e);
}

// Forward declaration
struct capture_result_t;

extern bool  g_is_systray;  // old g_is_clipboard_server;
extern int   g_sock;
extern char  g_sock_path[100];
extern int   g_scr_w, g_scr_h;
extern FILE* g_fp_log;

#ifdef __linux__
std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height);
#endif

std::string replace_str(std::string& str, const std::string_view from, const std::string_view to);
std::string select_image();

#ifndef _WIN32
fs::path get_runtime_dir();
#endif
bool acquire_tray_lock();

fs::path get_font_path(const std::string& font);
fs::path get_home_config_dir();
fs::path get_home_dir();
fs::path get_config_dir();

Result<capture_result_t> load_image_rgba(const std::string& path);
Result<>                 save_png(SavingOp op, const capture_result_t& img);

void minimize_window();  // Defined on main_tool_*
void maximize_window();  // Defined on main_tool_*
void extern_glfw_terminate(); // Defined on main_tool_*
void fit_to_screen(capture_result_t& img);
void rgba_to_grayscale(const uint8_t* rgba, uint8_t* result, int width, int height);

int get_screen_dpi();

#define BOLD_COLOR(x) (fmt::emphasis::bold | fmt::fg(x))

template <typename... Args>
[[noreturn]] inline void die(fmt::format_string<Args...> fmt, Args&&... args) noexcept
{
#ifdef _WIN32
    MessageBox(nullptr,
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...).c_str(),
               "Fatal Error",
               MB_ICONERROR | MB_OK);
#endif
    spdlog::critical(fmt, std::forward<Args>(args)...);

    std::exit(1);
}

template <typename... Args>
inline void warn(fmt::format_string<Args...> fmt, Args&&... args) noexcept
{
#ifdef _WIN32
    MessageBox(nullptr,
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...).c_str(),
               "Warning",
               MB_ICONWARNING | MB_OK);
#endif
    spdlog::warn(fmt, args...);
}

template <typename... Args>
inline void info(fmt::format_string<Args...> fmt, Args&&... args) noexcept
{
#ifdef _WIN32
    MessageBox(nullptr,
               fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...).c_str(),
               "Info",
               MB_ICONINFORMATION | MB_OK);
#endif
    spdlog::info(fmt, std::forward<Args>(args)...);
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

// RAII guard: ensures glfwTerminate() runs even on crash/signal.
// Without this, NVIDIA's driver is left in the implicit mode it switched
// to when we created a full-resolution window, permanently showing 1024x768.
inline struct GlfwGuard
{
    ~GlfwGuard() { extern_glfw_terminate(); }
} glfw_guard;


#endif  // !_UTIL_HPP_
