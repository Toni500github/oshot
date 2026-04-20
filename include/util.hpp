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

#include "fmt/base.h"
#include "fmt/color.h"
#include "spdlog/spdlog.h"
#include "version.h"

namespace fs = std::filesystem;
enum class SavingOp;

#if defined(__linux__)
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#elif defined(_WIN32)
#  include <combaseapi.h>
#  include <knownfolders.h>
#  include <shellapi.h>
#  include <shlobj.h>
#  include <windows.h>
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

template <typename E, typename T>
constexpr E enum_(T n) noexcept
{
    static_assert(std::is_integral_v<T>);
    return static_cast<E>(n);
}

// Forward declaration
struct capture_result_t;
struct ImVec4;

// taken from "fmt/color.h" with the addition of alpha.
// useful in contexts where ImVec4 is not used.
// Packed as 0xRRGGBBAA
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

    constexpr rgba_t(fmt::color hex)
        : r((uint32_t(hex) >> 16) & 0xFF),
          g((uint32_t(hex) >> 8)  & 0xFF),
          b((uint32_t(hex))       & 0xFF),
          a(0xFF) {}

    static constexpr rgba_t from_rgba(uint32_t v) { return { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) }; }
    static constexpr rgba_t from_abgr(uint32_t v) { return { uint8_t(v),       uint8_t(v >> 8),  uint8_t(v >> 16), uint8_t(v >> 24) }; }
    static constexpr rgba_t from_argb(uint32_t v) { return { uint8_t(v >> 16), uint8_t(v >> 8),  uint8_t(v),       uint8_t(v >> 24) }; }
    static constexpr rgba_t from_bgra(uint32_t v) { return { uint8_t(v >> 8),  uint8_t(v >> 16), uint8_t(v >> 24), uint8_t(v) }; }

    constexpr uint32_t to_rgba() const { return uint32_t(r)<<24 | uint32_t(g)<<16 | uint32_t(b)<<8 | a; }
    constexpr uint32_t to_abgr() const { return uint32_t(a)<<24 | uint32_t(b)<<16 | uint32_t(g)<<8 | r; }
    constexpr uint32_t to_argb() const { return uint32_t(a)<<24 | uint32_t(r)<<16 | uint32_t(g)<<8 | b; }
    constexpr uint32_t to_bgra() const { return uint32_t(b)<<24 | uint32_t(g)<<16 | uint32_t(r)<<8 | a; }

    constexpr ImVec4 to_imvec4() const;

    uint8_t r, g, b, a;
};
// clang-format on

inline rgba_t load_rgba(const uint8_t* p)
{
    return rgba_t(p[0], p[1], p[2], p[3]);
}

inline void store_rgba(uint8_t* p, const rgba_t& c)
{
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

extern bool g_is_systray;  // old g_is_clipboard_server;
extern int  g_sock;
extern char g_sock_path[100];
extern int  g_scr_w, g_scr_h;

static inline const std::string version_infos = fmt::format(
    "oshot v{} built from branch '{}' at {} commit '{}' ({}).\n"
    "Date: {}\n"
    "Tag: {}\n",
    VERSION,
    GIT_BRANCH,
    GIT_DIRTY,
    GIT_COMMIT_HASH,
    GIT_COMMIT_MESSAGE,
    GIT_COMMIT_DATE,
    GIT_TAG);

#ifdef __linux__
std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height);
#endif
std::vector<uint8_t> encode_to_png(const capture_result_t& cap);

std::string replace_str(std::string& str, const std::string_view from, const std::string_view to);
std::string select_image();
std::string col_to_hexstr(const rgba_t& col);

bool acquire_tray_lock();
bool is_system_dark_mode();
bool hexstr_to_col(const std::string_view hex, uint32_t& out);
bool hexstr_to_imvec4(const std::string_view hex, ImVec4& out);

#ifndef _WIN32
fs::path get_runtime_dir();
#endif
fs::path get_font_path(const std::string& font);
fs::path get_home_config_dir();
fs::path get_home_dir();
fs::path get_config_dir();

Result<capture_result_t> load_image_rgba(const std::string& path);
Result<std::string>      get_config_image_out_fmt();
Result<>                 save_png(SavingOp op, const capture_result_t& img);

void minimize_window();               // Defined on main_tool_*
void maximize_window();               // Defined on main_tool_*
void extern_glfwTerminate();          // Defined on main_tool_*
void extern_glfwSwapInterval(int v);  // Defined on main_tool_*
void fit_to_screen(capture_result_t& img);
void rgba_to_grayscale(const uint8_t* rgba, uint8_t* result, int width, int height);

int get_screen_dpi();

bool parse_hex_rgba(const std::string_view hex, rgba_t& out);

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
inline void error(fmt::format_string<Args...> fmt, Args&&... args) noexcept
{
#ifdef _WIN32
    MessageBox(
        nullptr, fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...).c_str(), "Error", MB_ICONERROR | MB_OK);
#endif
    spdlog::error(fmt, args...);
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
        die("Exiting due to CTRL-D or EOF");

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
    ~GlfwGuard() { extern_glfwTerminate(); }
} glfw_guard;

#endif  // !_UTIL_HPP_
