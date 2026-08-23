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

#ifndef _UTIL_HPP_
#define _UTIL_HPP_

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "fmt/base.h"
#include "fmt/color.h"
#include "nvdialog/nvdialog_core.h"
#include "nvdialog/nvdialog_dialog.h"
#include "nvdialog/nvdialog_error.h"
#include "platform.hpp"
#include "spdlog/spdlog.h"
#include "version.h"

namespace fs = std::filesystem;
enum class SavingOp;

#if OSHOT_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <combaseapi.h>
#  include <knownfolders.h>
#  include <shellapi.h>
#  include <shlobj.h>
#  include <windows.h>
#endif

#if OSHOT_WINDOWS || OSHOT_MACOS
#  define OSHOT_TOOL_ON_MAIN_THREAD true
#else
#  define OSHOT_TOOL_ON_MAIN_THREAD false
#endif

#define UNKNOWN "|<u2n4kn6ow8n>|"

#define STBI_ERROR (stbi_failure_reason() ? stbi_failure_reason() : "Unknown stbi Error")

// if Result is not ok(), return it's error
#define TRY(expr)                     \
    do                                \
    {                                 \
        auto&& _r = (expr);           \
        if (!_r.ok())                 \
            return Err(_r.error_v()); \
    } while (0)

// TRY() with fmt::format()
#define TRY_MSG(expr, fmtstr, ...)                                       \
    do                                                                   \
    {                                                                    \
        auto&& _r = (expr);                                              \
        if (!_r.ok())                                                    \
            return Err(fmtstr __VA_OPT__(, ) __VA_ARGS__, _r.error_v()); \
    } while (0)

// if Result is not ok(), execute on_err code
#define MUST_OK(expr, on_err) \
    do                        \
    {                         \
        auto&& _r = (expr);   \
        if (!_r.ok())         \
            on_err;           \
    } while (0)

// shotout to the better c++ server for these helper structs
template <typename T>
struct Ok
{
    using value_type = T;
    T value;
};

// fire-and-forget result
template <>
struct Ok<void>
{
};

// Deduction guide
template <typename T>
Ok(T) -> Ok<T>;
Ok() -> Ok<void>;

template <typename E = std::string>
struct Err
{
    using value_type = E;
    E value;

    Err() = default;

    // Generic single-value ctor: Err(42), Err(some_error_code), Err(std::string{...})
    template <typename U>
        requires std::constructible_from<E, U&&> && (!std::same_as<std::remove_cvref_t<U>, Err>)
    Err(U&& v) : value(std::forward<U>(v))
    {}

    // fmt-powered ctor: Err("failed: {}", code, ...)
    template <typename... Args>
        requires(sizeof...(Args) >= 1) && std::constructible_from<E, std::string>
    Err(fmt::format_string<Args...> fmt_str, Args&&... args) : value(fmt::format(fmt_str, std::forward<Args>(args)...))
    {}
};
template <typename E>
Err(E) -> Err<E>;
Err(const char*) -> Err<std::string>;

template <typename... Args>
    requires(sizeof...(Args) >= 1)
Err(fmt::format_string<Args...>, Args&&...) -> Err<std::string>;

template <typename T = Ok<void>, typename E = Err<std::string>>
class [[nodiscard("Must check if ok")]] Result
{
public:
    template <typename U>
    Result(Ok<U> const& v) : m_value(std::in_place_index<0>, v.value)
    {}
    template <typename U>
    Result(Ok<U>&& v) : m_value(std::in_place_index<0>, std::move(v.value))
    {}

    template <typename U>
    Result(Err<U> const& e) : m_value(std::in_place_index<1>, e.value)
    {}
    template <typename U>
    Result(Err<U>&& e) : m_value(std::in_place_index<1>, std::move(e.value))
    {}

    bool     ok() const { return std::holds_alternative<T>(m_value); }
             operator bool() const { return ok(); }
    T&       get() { return std::get<T>(m_value); }
    E&       error() { return std::get<E>(m_value); }
    const T& get() const { return std::get<T>(m_value); }
    const E& error() const { return std::get<E>(m_value); }

    template <typename U = T, typename = typename U::value_type>
    typename U::value_type& get_v()
    {
        return std::get<T>(m_value).value;
    }

    template <typename U = T, typename = typename U::value_type>
    const typename U::value_type& get_v() const
    {
        return std::get<T>(m_value).value;
    }

    template <typename U = E, typename = typename U::value_type>
    typename U::value_type& error_v()
    {
        return std::get<E>(m_value).value;
    }

    template <typename U = E, typename = typename U::value_type>
    const typename U::value_type& error_v() const
    {
        return std::get<E>(m_value).value;
    }

private:
    std::variant<T, E> m_value;
};

template <typename E>
class [[nodiscard("Must check if ok")]] Result<Ok<void>, E>
{
public:
    Result() : m_ok(true) {}
    Result(Ok<void>) : m_ok(true) {}

    template <typename U>
    Result(const Err<U>& err) : m_ok(false), m_err{ err.value }
    {}
    template <typename U>
    Result(Err<U>&& err) : m_ok(false), m_err{ std::move(err.value) }
    {}

    bool     ok() const { return m_ok; }
    E&       error() { return m_err; }
    const E& error() const { return m_err; }
             operator bool() const { return ok(); }

    template <typename U = E, typename = typename U::value_type>
    typename U::value_type& error_v()
    {
        return m_err.value;
    }

    template <typename U = E, typename = typename U::value_type>
    const typename U::value_type& error_v() const
    {
        return m_err.value;
    }

private:
    bool m_ok;
    E    m_err;
};

struct byte_units_t
{
    std::string unit;
    double      num_bytes;
};

template <typename E>
constexpr size_t idx(E e) noexcept
{
    static_assert(std::is_enum_v<E>);
    return static_cast<size_t>(e);
}

template <typename E, typename T>
constexpr E toe(T n) noexcept
{
    static_assert(std::is_integral_v<T>);
    return static_cast<E>(n);
}

// Forward declaration
struct capture_result_t;
struct ImVec4;
struct ImGuiIO;
enum class ImageExt;
// I'm not including <gtk/gtk.h> just for a couple of functions
extern "C" {
using gboolean = int32_t;
gboolean gtk_events_pending();
gboolean gtk_main_iteration();
}

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

    static constexpr rgba_t from_rgba(uint32_t v) { return { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8),  uint8_t(v) }; }
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

constexpr rgba_t operator""_rgba(unsigned long long v)
{
    return rgba_t(static_cast<uint32_t>(v));
}

inline void store_rgba(uint8_t* p, const rgba_t& c)
{
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

enum class ImageExt
{
    PNG,
    JPEG,
    BMP,
    TGA,
    COUNT
};

enum class SavingOp
{
    kNone,
    Clipboard,
    File
};

inline constexpr std::array<std::pair<ImageExt, const char*>, idx(ImageExt::COUNT)> IMAGE_EXTS_STR = {
    { { ImageExt::PNG, "PNG" }, { ImageExt::JPEG, "JPEG" }, { ImageExt::BMP, "BMP" }, { ImageExt::TGA, "TGA" } }
};
inline std::unordered_map<std::string_view, ImageExt> IMAGE_EXTS_ENUM = {
    { { "PNG", ImageExt::PNG }, { "JPEG", ImageExt::JPEG }, { "BMP", ImageExt::BMP }, { "TGA", ImageExt::TGA } }
};

extern bool g_is_systray;  // old g_is_clipboard_server;
extern int  g_sock;
extern char g_sock_path[100];
extern int  g_scr_w, g_scr_h;

static inline const std::string version_infos = fmt::format(
    "oshot v{} built from branch '{}' at {} commit '{}' ({}).\n"
    "Date: {}\n"
    "Tag: {}\n",
#ifdef DISABLE_PLUGINS
    "NO PLUGINS SUPPORT\n",
#endif
    VERSION,
    GIT_BRANCH,
    GIT_DIRTY,
    GIT_COMMIT_HASH,
    GIT_COMMIT_MESSAGE,
    GIT_COMMIT_DATE,
    GIT_TAG);

std::vector<uint8_t> encode_to_image(const capture_result_t& cap, ImageExt ext);

std::string replace_str(std::string& str, const std::string_view from, const std::string_view to);
std::string select_image();
std::string expand_var(std::string ret);
std::string col_to_hexstr(const rgba_t& col);

bool acquire_tray_lock();
bool is_system_dark_mode();
bool hexstr_to_col(const std::string_view hex, uint32_t& out);
bool hexstr_to_imvec4(const std::string_view hex, ImVec4& out);

std::string str_toupper(std::string str);
std::string str_tolower(std::string str);
#if !OSHOT_WINDOWS
fs::path get_runtime_dir();
#endif
fs::path get_font_path(const std::string& font);
fs::path get_home_config_dir();
fs::path get_home_cache_dir();
fs::path get_home_pictures_dir();
fs::path get_home_dir();
fs::path get_config_dir();
fs::path get_cache_dir();

Result<capture_result_t> load_image_rgba(const std::string& path);
Result<std::string>      get_config_image_out_fmt();
Result<>                 save_image(SavingOp op, const capture_result_t& img, ImageExt ext);

void minimize_window();
void maximize_window();
void extern_glfwTerminate();
void extern_glfwSwapInterval(int v);

byte_units_t auto_divide_bytes(const double num, const std::uint16_t base, const std::string_view maxprefix = "");
byte_units_t divide_bytes(const double num, const std::string_view prefix);
void         fit_to_screen(capture_result_t& img);
void         rgba_to_grayscale(const uint8_t* rgba, uint8_t* result, int width, int height);
void         build_font_atlas(ImGuiIO& io);
int          get_screen_dpi();
bool         parse_hex_rgba(const std::string_view hex, rgba_t& out);

#define BOLD_COLOR(x) (fmt::emphasis::bold | fmt::fg(x))

static void create_dialog(const char* title, const NvdDialogType type, const std::string& str) noexcept
{
    if (nvd_get_error() == NVD_NOT_INITIALIZED)
        return;

    NvdDialogBox* dialog = nvd_dialog_box_new(title, str.c_str(), type);
    nvd_show_dialog(dialog);
    nvd_free_object(dialog);
#if OSHOT_LINUX
    while (gtk_events_pending())
        gtk_main_iteration();
#endif
}

template <typename... Args>
[[noreturn]] inline void die(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);

    spdlog::critical("{}", str);
    create_dialog("oshot Fatal Error", NVD_DIALOG_ERROR, str);
    std::exit(1);
}

template <typename... Args>
inline void error(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);

    spdlog::error("{}", str);
    create_dialog("oshot Error", NVD_DIALOG_ERROR, str);
}

template <typename... Args>
inline void warn(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);

    spdlog::warn("{}", str);
    create_dialog("oshot Warning", NVD_DIALOG_WARNING, str);
}

template <typename... Args>
inline void info(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);

    spdlog::info("{}", str);
    create_dialog("oshot Info", NVD_DIALOG_SIMPLE, str);
}

/** Ask the user a yes or no question.
 * @param def The default result (Removed)
 * @param fmt The format string
 * @param args Arguments in the format
 * @returns the result, y = true, n = false, only returns def if the result is def
 */
template <typename... Args>
inline bool ask_user_yn(bool def, const std::string_view fmt, Args&&... args)
{
    const std::string& str = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);
    if (nvd_get_error() == NVD_NOT_INITIALIZED)
    {
#if OSHOT_WINDOWS
        int result = MessageBox(NULL, str.c_str(), "Confirmation", MB_YESNO | MB_ICONQUESTION);
        return (result == IDYES);
#else
        const std::string_view inputs_str = def ? "[Y/n]" : "[y/N]";
        std::string            result;
        fmt::print("{} {}: ", str, inputs_str);

        while (std::getline(std::cin, result) && (result.length() > 1))
        {
            fmt::print(BOLD_COLOR(fmt::rgb(fmt::color::yellow)), "Please answear y or n\n");
            fmt::print("{} {}: ", str, inputs_str);
        }

        if (std::cin.eof())
            die("Exiting due to CTRL-D or EOF");

        if (result.empty())
            return def;

        if (def ? std::tolower(result[0]) != 'n' : std::tolower(result[0]) != 'y')
            return def;

        return !def;
#endif
    }
    else
    {
        NvdQuestionBox* question = nvd_dialog_question_new("Confirmation", str.c_str(), NVD_YES_NO);
        if (!question)
            die("Couldn't create question dialog box");
        return nvd_get_reply(question) == NVD_REPLY_OK;
    }
}

// RAII guard: ensures glfwTerminate() runs even on crash/signal.
// Without this, NVIDIA's driver is left in the implicit mode it switched
// to when we created a full-resolution window, permanently showing 1024x768.
inline struct GlfwGuard
{
    ~GlfwGuard() { extern_glfwTerminate(); }
} glfw_guard;

struct CdGuard
{
    fs::path saved;
    CdGuard(const fs::path& p) : saved(fs::current_path())
    {
        if (!fs::exists(p))
            die("CdGuard: Path {} doesn't exist", p.string());
        if (!p.empty())
            fs::current_path(p);
    }
    ~CdGuard() { fs::current_path(saved); }
};

#endif  // !_UTIL_HPP_
