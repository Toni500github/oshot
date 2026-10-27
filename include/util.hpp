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

#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "fmt/base.h"
#include "fmt/color.h"
#include "i18n.hpp"
#include "nvdialog/nvdialog_dialog.h"
#include "platform.hpp"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/spdlog.h"

namespace fs = std::filesystem;

// ------------------------------
// Macros
// ------------------------------
#if OSHOT_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <combaseapi.h>
#  include <knownfolders.h>
#  include <shellapi.h>
#  include <shlobj.h>
#  include <windows.h>
#  define DIR_SEP_STR "\\"
#  define DIR_SEP     '\\'
#else
#  define DIR_SEP_STR "/"
#  define DIR_SEP     '/'
#endif

/**
 * Whether captures run inline on the tray (main) thread instead of the capture worker thread.
 * AppKit only allows UI on the main thread.
 **/
#if OSHOT_WINDOWS || OSHOT_MACOS
#  define OSHOT_TOOL_ON_MAIN_THREAD true
#else
#  define OSHOT_TOOL_ON_MAIN_THREAD false
#endif

/**
 * What get_relative_path() and which() return when nothing is found.
 * Unlikely enough to never be a real path.
 **/
#define UNKNOWN "|<u2n4kn6ow8n>|"

/* stbi_failure_reason(), or a placeholder when stb_image didn't set one */
#define STBI_ERROR (stbi_failure_reason() ? stbi_failure_reason() : "Unknown stbi Error")

/* fmt style for bold text in color `x` */
#define BOLD_COLOR(x) (fmt::emphasis::bold | fmt::fg(x))

/**
 * Return the error of a Result from the current function, if it has one
 * @param expr The Result to check
 **/
#define TRY(expr)                     \
    do                                \
    {                                 \
        auto&& _r = (expr);           \
        if (!_r.ok())                 \
            return Err(_r.error_v()); \
    } while (0)

/**
 * TRY(), but wrap the error in a message
 * @param expr The Result to check
 * @param fmtstr The format string. The error is the last argument, after the ones in `...`
 **/
#define TRY_MSG(expr, fmtstr, ...)                                       \
    do                                                                   \
    {                                                                    \
        auto&& _r = (expr);                                              \
        if (!_r.ok())                                                    \
            return Err(fmtstr __VA_OPT__(, ) __VA_ARGS__, _r.error_v()); \
    } while (0)

/**
 * Run some code if a Result has an error
 * @param expr The Result to check
 * @param on_err The code to run. The Result is available as `_r`
 **/
#define MUST_OK(expr, on_err) \
    do                        \
    {                         \
        auto&& _r = (expr);   \
        if (!_r.ok())         \
        {                     \
            on_err;           \
        }                     \
    } while (0)

// ------------------------------
// Forward declarations
// ------------------------------
struct capture_result_t;
struct ImVec4;
struct ImGuiIO;
struct ImFont;
struct ImFontAtlas;
struct ImFontConfig;
enum class ImageExt;
enum class SavingOp;

// Used by the templates and structs below, documented further down
bool  init_dialogs() noexcept;
void  create_dialog(const char* title, const NvdDialogType type, const std::string& str) noexcept;
void  extern_glfwTerminate();
bool  hide_window();
void  show_window();
void* dialog_parent_window();

// I'm not including <gtk/gtk.h> just for a couple of functions
extern "C" {
using gboolean = int32_t;
gboolean gtk_events_pending();
gboolean gtk_main_iteration();
}

// ------------------------------
// Types
// ------------------------------

// shotout to the better c++ server for these helper structs

/* The value of a successful Result */
template <typename T>
struct Ok
{
    using value_type = T;
    T value;
};

/* A successful Result with no value */
template <>
struct Ok<void>
{
};

template <typename T>
Ok(T) -> Ok<T>;
Ok() -> Ok<void>;

/* The error of a failed Result. A std::string by default */
template <typename E = std::string>
struct Err
{
    using value_type = E;
    E value;

    Err() = default;

    // Err(42), Err(some_error_code), Err(std::string{...})
    template <typename U>
        requires std::constructible_from<E, U&&> && (!std::same_as<std::remove_cvref_t<U>, Err>)
    Err(U&& v) : value(std::forward<U>(v))
    {}

    // Err("failed: {}", code, ...), formatted with fmt::format()
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

/**
 * Either a value (Ok) or an error (Err), returned instead of throwing.
 * Check ok() before get(), or propagate the error with TRY().
 **/
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

    // The value inside the Err, e.g. the std::string of an Err<std::string>
    template <typename U = E, typename = typename U::value_type>
    typename U::value_type& error_v()
    {
        return std::get<E>(m_value).value;
    }

    template <typename U = E, typename = typename U::value_type>
    [[nodiscard]] const typename U::value_type& error_v() const
    {
        return std::get<E>(m_value).value;
    }

    [[nodiscard]] bool     ok() const { return std::holds_alternative<T>(m_value); }
                           operator bool() const { return ok(); }
    T&                     get() { return std::get<T>(m_value); }
    E&                     error() { return std::get<E>(m_value); }
    [[nodiscard]] const T& get() const { return std::get<T>(m_value); }
    [[nodiscard]] const E& error() const { return std::get<E>(m_value); }

private:
    std::variant<T, E> m_value;
};

/* Result<> of a function that only succeeds or fails */
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

    // The value inside the Err, e.g. the std::string of an Err<std::string>
    template <typename U = E, typename = typename U::value_type>
    typename U::value_type& error_v()
    {
        return m_err.value;
    }

    template <typename U = E, typename = typename U::value_type>
    [[nodiscard]] const typename U::value_type& error_v() const
    {
        return m_err.value;
    }

    [[nodiscard]] bool     ok() const { return m_ok; }
    E&                     error() { return m_err; }
    [[nodiscard]] const E& error() const { return m_err; }
                           operator bool() const { return ok(); }

private:
    bool m_ok;
    E    m_err;
};

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

/* A number of bytes in a unit, from auto_divide_bytes() and divide_bytes() */
struct byte_units_t
{
    std::string unit;
    double      num_bytes;
};

// NOLINTNEXTLINE(performance-enum-size)
enum class ImageExt
{
    PNG,
    JPEG,
    BMP,
    TGA,
    COUNT
};

/* What to do with the image once the selection is done */
// NOLINTNEXTLINE(performance-enum-size)
enum class SavingOp
{
    kNone,
    Clipboard,
    File
};

/* The base of a byte unit, for auto_divide_bytes() */
// NOLINTNEXTLINE(performance-enum-size)
enum class ByteUnit
{
    Kibibyte = 1024,
    Kilobyte = 1000,
};

/**
 * Runs glfwTerminate() when the process exits, even on a crash or signal.
 * Without it, NVIDIA's driver is left in the video mode it switched to for a
 * full-resolution window, and the screen stays at 1024x768.
 **/
struct GlfwGuard
{
    ~GlfwGuard();
};

/**
 * Changes the working directory, and changes it back when it goes out of scope.
 * Dies if the directory doesn't exist.
 **/
struct CdGuard
{
    fs::path saved;

    CdGuard(const fs::path& p);
    ~CdGuard() { fs::current_path(saved); }
};

// ------------------------------
// Globals and constants
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

// Names of the ImageExt values, as written in the config file
inline constexpr std::array<std::pair<ImageExt, const char*>, static_cast<size_t>(ImageExt::COUNT)> IMAGE_EXTS_STR = {
    { { ImageExt::PNG, "PNG" }, { ImageExt::JPEG, "JPEG" }, { ImageExt::BMP, "BMP" }, { ImageExt::TGA, "TGA" } }
};

// This process owns the systray (formerly g_is_clipboard_server)
extern bool g_is_systray;

// Video mode size of the monitor the overlay is on
extern int g_scr_w, g_scr_h;

// `/nix` exists, so the OCR window suggests getting tesseract's models from nixpkgs
extern bool g_is_nix;

// Running inside a Flatpak sandbox
extern bool g_is_flatpak;

// The last log lines, shown in the Logs window
extern std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_imgui_log_sink;

// The output of --version, also shown in the About window.
// In globals.cpp, the only file that includes version.h, which changes with every commit
extern const std::string version_infos;

inline GlfwGuard glfw_guard;

// ------------------------------
// Template functions
// ------------------------------

/**
 * Convert an enum class value to its index, for arrays and bitsets sized by the enum's COUNT
 * @param e The enum value
 **/
template <typename E>
constexpr size_t idx(E e) noexcept
{
    static_assert(std::is_enum_v<E>);
    return static_cast<size_t>(e);
}

/**
 * Convert an index back to an enum class value. The opposite of idx()
 * @param n The index
 **/
template <typename E, typename T>
constexpr E toe(T n) noexcept
{
    static_assert(std::is_integral_v<T>);
    return static_cast<E>(n);
}

/**
 * fmt::format() with a runtime format string, which never throws.
 * Used by the noexcept helpers below
 * @param fmt The format string
 * @param args Arguments in the format
 * @return The formatted string, or `fmt` unformatted if formatting failed
 **/
template <typename... Args>
inline std::string format_noexcept(const std::string_view fmt, Args&&... args) noexcept
{
    try
    {
        return fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
    catch (...)
    {
        return std::string(fmt);
    }
}

/**
 * Log a critical error, show it in a dialog, then exit with code 1
 * @param fmt The format string
 * @param args Arguments in the format
 **/
template <typename... Args>
// NOLINTNEXTLINE(bugprone-exception-escape): only reachable through spdlog's own error handler
[[noreturn]] inline void die(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = format_noexcept(fmt, std::forward<Args>(args)...);

    spdlog::critical("{}", str);
    create_dialog(_("oshot Fatal Error"), NVD_DIALOG_ERROR, str);
    std::exit(1);
}

/**
 * Log an error and show it in a dialog
 * @param fmt The format string
 * @param args Arguments in the format
 **/
template <typename... Args>
// NOLINTNEXTLINE(bugprone-exception-escape): only reachable through spdlog's own error handler
inline void error(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = format_noexcept(fmt, std::forward<Args>(args)...);

    spdlog::error("{}", str);
    create_dialog(_("oshot Error"), NVD_DIALOG_ERROR, str);
}

/**
 * Log a warning and show it in a dialog
 * @param fmt The format string
 * @param args Arguments in the format
 **/
template <typename... Args>
// NOLINTNEXTLINE(bugprone-exception-escape): only reachable through spdlog's own error handler
inline void warn(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = format_noexcept(fmt, std::forward<Args>(args)...);

    spdlog::warn("{}", str);
    create_dialog(_("oshot Warning"), NVD_DIALOG_WARNING, str);
}

/**
 * Log a message and show it in a dialog
 * @param fmt The format string
 * @param args Arguments in the format
 **/
template <typename... Args>
// NOLINTNEXTLINE(bugprone-exception-escape): only reachable through spdlog's own error handler
inline void info(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = format_noexcept(fmt, std::forward<Args>(args)...);

    spdlog::info("{}", str);
    create_dialog(_("oshot Info"), NVD_DIALOG_SIMPLE, str);
}

/**
 * Ask the user a yes or no question.
 * Uses a dialog, or the terminal when dialogs are unavailable (e.g. in oshotpm).
 * @param def The answer when the user just presses Enter in the terminal
 * @param fmt The format string
 * @param args Arguments in the format
 * @return true for yes, false for no. EOF (CTRL-D) counts as no
 **/
template <typename... Args>
inline bool ask_user_yn(bool def, const std::string_view fmt, Args&&... args)
{
    const std::string& str = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);
    if (!init_dialogs())
    {
#if OSHOT_WINDOWS
        // oshot.exe has no console; oshotpm.exe (and oshot run from one) does
        if (!GetConsoleWindow())
        {
            int result = MessageBox(NULL, str.c_str(), _("Confirmation"), MB_YESNO | MB_ICONQUESTION);
            return (result == IDYES);
        }
#endif
        const std::string_view inputs_str = def ? "[Y/n]" : "[y/N]";
        std::string            result;
        fmt::print("{} {}: ", str, inputs_str);

        while (std::getline(std::cin, result) && (result.length() > 1))
        {
            fmt::print(BOLD_COLOR(fmt::rgb(fmt::color::yellow)), "Please answear y or n\n");
            fmt::print("{} {}: ", str, inputs_str);
        }

        // CTRL-D or no input left counts as a "no", so callers can cancel and clean up
        if (std::cin.eof())
        {
            fmt::print("\n");
            return false;
        }

        if (result.empty())
            return def;

        if (def ? std::tolower(result[0]) != 'n' : std::tolower(result[0]) != 'y')
            return def;

        return !def;
    }
    else
    {
        NvdQuestionBox* question = nvd_dialog_question_new(_("Confirmation"), str.c_str(), NVD_YES_NO);
        if (!question)
            die("Couldn't create question dialog box");
        return nvd_get_reply(question) == NVD_REPLY_OK;
    }
}

// ------------------------------
// Dialogs and logging
// ------------------------------

/**
 * Set up nvdialog the first time a dialog or notification needs it.
 * On Linux this starts GTK, which takes tens of milliseconds, so it runs after
 * the screenshot is taken instead of at startup. Thread-safe; call it on the
 * main thread where possible, since that's where GTK gets set up.
 * @return false if dialogs are unavailable (init failed, or disable_dialogs())
 **/
bool init_dialogs() noexcept;

/* Never show dialogs again, e.g. in the oshotpm CLI: messages only go to the log/terminal */
void disable_dialogs() noexcept;

/**
 * Show a dialog and wait until it's closed. Does nothing if dialogs are unavailable
 * @param title The dialog's title
 * @param type The dialog's icon (NVD_DIALOG_SIMPLE, NVD_DIALOG_WARNING or NVD_DIALOG_ERROR)
 * @param str The message
 **/
void create_dialog(const char* title, const NvdDialogType type, const std::string& str) noexcept;

/**
 * Make `logger` spdlog's default logger in liboshot_common too.
 * spdlog is header-only, and on Windows an executable and a DLL each get their
 * own copy of its registry, so the executable has to set it on both sides.
 * So do oshot and liboshot_common in macOS Release builds, where LTO makes the
 * executable's copy local instead of sharing the library's.
 * @param logger The executable's default logger
 **/
void set_shared_default_logger(std::shared_ptr<spdlog::logger> logger);

/**
 * Undo set_shared_default_logger(), at exit before the log file is deleted:
 * the DLL's registry outlives the executable's and would keep the file open,
 * which Windows can't delete.
 **/
void release_shared_default_logger();

// ------------------------------
// Strings
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
 * Expand a leading '~' (alone or before a directory separator) to the home
 * directory, and $NAME or ${NAME} to the environment variable's value.
 * Unset variables are left as they are.
 * @param ret The string to expand
 **/
std::string expand_var(std::string ret);

/**
 * Find a file in one of the directories of a ':'-separated environment variable (PATH, XDG_DATA_DIRS, ...)
 * @param relative_path The path to look for, relative to each directory
 * @param env The environment variable's name, without $
 * @param mode Mode bits the file must have, from sys/stat.h,
 *             such as S_IXUSR for executables or S_IFDIR for directories
 * @return The first full path found, else UNKNOWN
 **/
std::string get_relative_path(const std::string_view relative_path, const char* env, const long long mode);

/**
 * Simulate the behaviour of the command `which`
 * @param command The command to look up in $PATH
 * @return The full path of the executable, else UNKNOWN
 **/
std::string which(const std::string_view command);

// ------------------------------
// Colors
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

// ------------------------------
// Bytes
// ------------------------------

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

// ------------------------------
// Paths
// ------------------------------

/**
 * Get the user's home directory. Dies if it can't be found
 * @return $HOME (%USERPROFILE% on Windows), else the passwd entry's
 **/
fs::path get_home_dir();

/**
 * Get the user config directory
 * @return $XDG_CONFIG_HOME or ~/.config/ (%APPDATA% on Windows)
 **/
fs::path get_home_config_dir();

/**
 * Get the user cache directory
 * @return $XDG_CACHE_HOME or ~/.cache/ (%LOCALAPPDATA% on Windows)
 **/
fs::path get_home_cache_dir();

/**
 * Get the user pictures directory, where the save dialog opens first
 * @return $XDG_PICTURES_DIR, or the one in user-dirs.dirs, else ~/Pictures (the Pictures known folder on Windows)
 **/
fs::path get_home_pictures_dir();

/**
 * Get oshot's config directory, where "config.toml" is
 * @return get_home_config_dir() / "oshot"
 **/
fs::path get_config_dir();

/**
 * Get oshot's cache directory, where "cache.toml" is
 * @return get_home_cache_dir() / "oshot"
 **/
fs::path get_cache_dir();

#if !OSHOT_WINDOWS
/**
 * Get the directory for runtime files, like the tray lock
 * @return $XDG_RUNTIME_DIR, else the temp directory
 **/
fs::path get_runtime_dir();
#endif

/**
 * Get the path of the running executable
 * @return The absolute path, or empty if it can't be found (e.g. on OpenBSD)
 **/
fs::path get_exe_path();

/**
 * Get the directory of the running executable
 * @return get_exe_path()'s parent, or empty
 **/
fs::path get_exe_dir();

/**
 * Get the OCR models shipped next to the executable (Windows zip/installer, macOS .app)
 * @return The directory, or empty if there's none
 **/
fs::path get_bundled_models_dir();

/**
 * Get where the system's tesseract models are, which differs per distro.
 * Only the default of the ocr-path option
 **/
std::string get_default_ocr_path();

/**
 * Find a font file by path or by file name in the system and user font directories
 * @param font An existing path, or a file name like "DejaVuSans.ttf"
 * @return The font's path, or empty if not found
 **/
fs::path get_font_path(const std::string& font);

/**
 * List the TrueType and OpenType fonts in the system and user font directories
 * @return One path per file name, the one get_font_path() finds for that name
 **/
std::vector<fs::path> list_font_files();

/**
 * fs::remove_all() that also deletes read-only files (on Windows), and only logs on failure
 * @param path The file or directory to remove
 * @return false if something couldn't be removed
 **/
bool remove_tree(const fs::path& path) noexcept;

// ------------------------------
// Images
// ------------------------------

/**
 * Load an image as RGBA
 * @param path The image file, or "-" to read it from stdin
 **/
Result<capture_result_t> load_image_rgba(const std::string& path);

/**
 * Encode an image in a file format
 * @param cap The RGBA image
 * @param ext The format to encode to
 * @return The encoded file, empty on failure
 **/
std::vector<uint8_t> encode_to_image(const capture_result_t& cap, ImageExt ext);

/**
 * Encode an image and write it to a file
 * @param path The file to write
 * @param img The RGBA image
 * @param ext The format to encode to
 **/
Result<> write_image_file(const std::string& path, const capture_result_t& img, ImageExt ext);

/**
 * Write an image in a directory, named after the image-out-fmt option
 * @param dir The directory, created if missing
 * @param img The RGBA image
 * @param ext The format to encode to
 * @return The path of the written file
 **/
Result<std::string> save_image_to_dir(const fs::path& dir, const capture_result_t& img, ImageExt ext);

/**
 * Copy an image to the clipboard or save it, then send a notification.
 * Saving goes to the --instant-save directory, else asks where with a save dialog
 * @param op Clipboard or File
 * @param img The RGBA image
 * @param ext The format to encode to
 **/
Result<> save_image(SavingOp op, const capture_result_t& img, ImageExt ext);

/**
 * Get the file name for a new image, from the image-out-fmt option and the current local time
 * @return The file name, with the extension of image-out-type
 **/
Result<std::string> get_config_image_out_fmt();

/**
 * Shrink an image in place to fit the screen (g_scr_w x g_scr_h), keeping its aspect ratio.
 * Smaller images are left alone
 * @param img The RGBA image
 **/
void fit_to_screen(capture_result_t& img);

/**
 * Convert an RGBA image to 8-bit grayscale (ITU-R BT.601 luminance)
 * @param rgba The RGBA pixels
 * @param result Where to write `width * height` gray pixels
 * @param width The image width
 * @param height The image height
 **/
void rgba_to_grayscale(const uint8_t* rgba, uint8_t* result, int width, int height);

// ------------------------------
// Window and GLFW
// ------------------------------

/**
 * Register the window functions of the graphics backend (main_tool_*), so oshot_common
 * can call them without linking GLFW
 * @param minimize_fn Minimizes the overlay, e.g. when a link opens in the browser
 * @param hide_fn Hides the overlay while a modal dialog is open, see hide_window()
 * @param show_fn Shows the overlay again after hide_fn
 * @param dialog_parent_fn Returns the overlay's native window, see dialog_parent_window(). May be nullptr
 * @param _extern_glfwTerminate glfwTerminate()
 * @param _extern_glfwSwapInterval glfwSwapInterval()
 **/
void register_window_callbacks(void (*minimize_fn)(),
                               bool (*hide_fn)(),
                               void (*show_fn)(),
                               void* (*dialog_parent_fn)(),
                               void (*_extern_glfwTerminate)(),
                               void (*_extern_glfwSwapInterval)(int));

/* Minimize the overlay window, if a backend registered one */
void minimize_window();

/**
 * Hide the overlay window while a modal dialog (file picker, message box) is open.
 * It's always on top, so the dialog would open behind it, and its frame loop is
 * blocked until the dialog closes. Does nothing off the overlay's thread
 * @return true if the overlay was on screen and is now hidden: call show_window() after the dialog
 **/
bool hide_window();

/* Show the overlay window again after hide_window() */
void show_window();

/**
 * The overlay's native window (HWND on Windows), to own a dialog so it stays above the overlay
 * @return nullptr off the overlay's thread, without an overlay, or where owning isn't supported
 **/
void* dialog_parent_window();

/* Call glfwTerminate(), if a backend registered it */
void extern_glfwTerminate();

/**
 * Call glfwSwapInterval(), if a backend registered it
 * @param v 1 to enable vsync, 0 to disable it
 **/
void extern_glfwSwapInterval(int v);

/**
 * Rebuild ImGui's font atlas from the fonts option, then ImGui's own font with builtin-font,
 * then the system UI font and the fonts covering the UI language's script. The first font found
 * is the default one and the others fill in its missing glyphs. Falls back to ImGui's own font
 * when none is found
 * @param io The ImGui IO whose atlas to rebuild
 **/
void build_font_atlas(ImGuiIO& io);

/**
 * ImFontAtlas::AddFontFromFileTTF() that returns nullptr for a file it can't load,
 * where ImGui would assert in a debug build
 * @param atlas The atlas to add the font to
 * @param path The font file
 * @param size The font's reference size in pixels
 * @param cfg Options for the font, e.g. MergeMode, or nullptr for the defaults
 * @return The font, or nullptr
 **/
ImFont* add_font_file(ImFontAtlas* atlas, const fs::path& path, float size, const ImFontConfig* cfg = nullptr);

// ------------------------------
// Misc
// ------------------------------

/**
 * Try to become the single instance that owns the systray.
 * The lock is held until the process exits
 * @return false if another instance already owns it
 **/
bool acquire_tray_lock();

#endif  // !_UTIL_HPP_
