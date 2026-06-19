#ifndef _UTIL_HPP_
#define _UTIL_HPP_

#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "fmt/base.h"
#include "fmt/color.h"
#include "nvdialog/nvdialog_core.h"
#include "nvdialog/nvdialog_dialog.h"
#include "spdlog/spdlog.h"
#include "version.h"

namespace fs = std::filesystem;
enum class SavingOp;

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
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

#define STBI_ERROR std::string(stbi_failure_reason() ? stbi_failure_reason() : "Unknown Error")

// These macros are just for conviniences, nothing else
// They kinda suck ngl
#define TRY(expr)                     \
    do                                \
    {                                 \
        auto&& _r = (expr);           \
        if (!_r.ok())                 \
            return Err(_r.error_v()); \
    } while (0)

#define TRY_MSG(expr, fmtstr, ...)                                                    \
    do                                                                                \
    {                                                                                 \
        auto&& _r = (expr);                                                           \
        if (!_r.ok())                                                                 \
            return Err(fmt::format(fmtstr __VA_OPT__(, ) __VA_ARGS__, _r.error_v())); \
    } while (0)

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
};
template <typename E>
Err(E) -> Err<E>;

template <typename T = Ok<void>, typename E = Err<std::string>>
class Result
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
class Result<Ok<void>, E>
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
fs::path get_home_cache_dir();
fs::path get_home_pictures_dir();
fs::path get_home_dir();
fs::path get_config_dir();
fs::path get_cache_dir();

Result<capture_result_t> load_image_rgba(const std::string& path);
Result<std::string>      get_config_image_out_fmt();
Result<>                 save_png(SavingOp op, const capture_result_t& img);

void minimize_window();               // Defined on main_tool_*
void maximize_window();               // Defined on main_tool_*
void extern_glfwTerminate();          // Defined on main_tool_*
void extern_glfwSwapInterval(int v);  // Defined on main_tool_*
void fit_to_screen(capture_result_t& img);
void rgba_to_grayscale(const uint8_t* rgba, uint8_t* result, int width, int height);
void build_font_atlas(ImGuiIO& io);
int  get_screen_dpi();

bool parse_hex_rgba(const std::string_view hex, rgba_t& out);

#define BOLD_COLOR(x) (fmt::emphasis::bold | fmt::fg(x))

static void create_dialog(const char* title, const NvdDialogType type, const std::string& str) noexcept
{
    NvdDialogBox* dialog = nvd_dialog_box_new(title, str.c_str(), type);
    nvd_show_dialog(dialog);
    nvd_free_object(dialog);
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
inline bool ask_user_yn(bool, const std::string_view fmt, Args&&... args)
{
    const std::string& str      = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);
    NvdQuestionBox*    question = nvd_dialog_question_new("Confirmation", str.c_str(), NVD_YES_NO);
    if (!question)
        die("Couldn't create question dialog box");
    return nvd_get_reply(question) == NVD_REPLY_OK;
}

// RAII guard: ensures glfwTerminate() runs even on crash/signal.
// Without this, NVIDIA's driver is left in the implicit mode it switched
// to when we created a full-resolution window, permanently showing 1024x768.
inline struct GlfwGuard
{
    ~GlfwGuard() { extern_glfwTerminate(); }
} glfw_guard;

class CdGuard
{
public:
    fs::path saved;
    CdGuard(const fs::path& p) : saved(fs::current_path())
    {
        if (!p.empty())
            fs::current_path(p);
    }
    ~CdGuard() { fs::current_path(saved); }
};

#endif  // !_UTIL_HPP_
