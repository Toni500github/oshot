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

#include "util.hpp"

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

#include "cache.hpp"
#include "clipboard.hpp"
#include "config.hpp"
#include "dotenv.hpp"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "i18n.hpp"
#include "nvdialog/nvdialog_notification.h"
#include "platform.hpp"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "tinyfiledialogs.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

// clang-format off
#if OSHOT_WINDOWS
#  ifdef __MINGW64__
#    undef NTDDI_VERSION
#    undef _WIN32_WINNT
#    define NTDDI_VERSION NTDDI_WINBLUE
#    define _WIN32_WINNT  _WIN32_WINNT_WINBLUE
#  endif
#  include <io.h>
#  include <shellscalingapi.h>  // GetDpiForMonitor
#  include <shlobj.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "Shcore.lib")
#  pragma comment(lib, "ws2_32.lib")
#else
#  if OSHOT_MACOS
#    include <CoreFoundation/CoreFoundation.h>
#    include <CoreGraphics/CoreGraphics.h>
#    include <mach-o/dyld.h>
#  elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#    include <sys/types.h>
#    include <sys/sysctl.h>
#  endif
#  include <pwd.h>
#  include <sys/file.h>
#  include <unistd.h>
#  include <X11/Xlib.h>
#endif
#if OSHOT_FREEDESKTOP
#  include <fontconfig/fontconfig.h>
#endif
// clang-format on

// Defined and registered in src/main_tool_* source files
namespace main_tool
{
void (*minimize_fn)()                 = nullptr;
bool (*hide_fn)()                     = nullptr;
void (*show_fn)()                     = nullptr;
void* (*dialog_parent_fn)()           = nullptr;
void (*_extern_glfwTerminate)()       = nullptr;
void (*_extern_glfwSwapInterval)(int) = nullptr;
}  // namespace main_tool

static const std::unordered_map<std::string, std::string>& get_xdg_user_dirs()
{
    static std::unordered_map<std::string, std::string> cache;
    if (!cache.empty())
        return cache;

#if !OSHOT_WINDOWS
    fs::path file = get_home_config_dir() / "user-dirs.dirs";

    if (!fs::exists(file))
        return cache;

    Dotenv env;
    if (!env.load(file.string()))
        return cache;

    cache = env.data();
#endif

    return cache;
}

GlfwGuard::~GlfwGuard()
{
    extern_glfwTerminate();
}

CdGuard::CdGuard(const fs::path& p) : saved(fs::current_path())
{
    if (!fs::exists(p))
        die("CdGuard: Path {} doesn't exist", p.string());
    if (!p.empty())
        fs::current_path(p);
}

constexpr ImVec4 rgba_t::to_imvec4() const
{
    return { float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, float(a) / 255.0f };
}

#if OSHOT_WINDOWS
static HANDLE g_tray_mutex = nullptr;

bool acquire_tray_lock()
{
    // "Local\\" is per-user session; "Global\\" is system-wide and needs privileges sometimes.
    g_tray_mutex = CreateMutexW(nullptr, TRUE, L"Local\\oshot_tray_daemon");
    if (!g_tray_mutex)
        return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(g_tray_mutex);
        g_tray_mutex = nullptr;
        return false;  // already running
    }

    return true;  // we own it
}

#else
fs::path get_runtime_dir()
{
    const char* xdg = ::getenv("XDG_RUNTIME_DIR");
    return xdg ? fs::path(xdg) : fs::temp_directory_path();
}

bool acquire_tray_lock()
{
    // Without XDG_RUNTIME_DIR this is the shared /tmp, where another user's
    // 0600 lock file would make open() fail and look like "already running"
    const char*       xdg       = ::getenv("XDG_RUNTIME_DIR");
    const std::string lock_name = xdg ? "oshot.lock" : fmt::format("oshot-{}.lock", getuid());
    const std::string lock_path = (get_runtime_dir() / lock_name).string();

    // O_CLOEXEC: don't leak the fd into grim/wl-copy/zenity children
    int fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        spdlog::warn("Failed to open tray lock {}: {}", lock_path, strerror(errno));
        return false;
    }

    struct flock fl{};
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;

    if (fcntl(fd, F_SETLK, &fl) == -1)
    {
        // Another instance holds the lock
        close(fd);
        return false;
    }

    // We hold the lock. it's released automatically when the process exits
    return true;
}
#endif

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

std::vector<uint8_t> encode_to_image(const capture_result_t& cap, ImageExt ext)
{
    std::vector<uint8_t> out;

    auto callback = [](void* context, void* data, int size) {
        auto* buf   = static_cast<std::vector<uint8_t>*>(context);
        auto* bytes = static_cast<uint8_t*>(data);

        buf->insert(buf->end(), bytes, bytes + size);
    };

    out.reserve(size_t(cap.w) * cap.h * 4);

    switch (ext)
    {
        case ImageExt::PNG:
            stbi_write_png_to_func(callback, &out, cap.w, cap.h, STBIR_RGBA, cap.data.data(), cap.w * 4);
            break;

        case ImageExt::JPEG:
            stbi_write_jpg_to_func(callback, &out, cap.w, cap.h, STBIR_RGBA, cap.data.data(), 90);
            break;

        case ImageExt::BMP: stbi_write_bmp_to_func(callback, &out, cap.w, cap.h, STBIR_RGBA, cap.data.data()); break;
        case ImageExt::TGA: stbi_write_tga_to_func(callback, &out, cap.w, cap.h, STBIR_RGBA, cap.data.data()); break;

        default: break;
    }

    return out;
}

void fit_to_screen(capture_result_t& img)
{
    if (img.w <= g_scr_w && img.h <= g_scr_h)
        return;

    float scale = std::min(float(g_scr_w) / float(img.w), float(g_scr_h) / float(img.h));

    int new_w = int(std::round(float(img.w) * scale));
    int new_h = int(std::round(float(img.h) * scale));

    std::vector<uint8_t> resized(size_t(new_w) * new_h * 4);

    bool ok = stbir_resize_uint8_linear(img.data.data(), img.w, img.h, 0, resized.data(), new_w, new_h, 0, STBIR_RGBA);
    if (!ok)
    {
        spdlog::warn("Failed to resize image: {}", STBI_ERROR);
        return;
    }

    img.data = std::move(resized);
    img.w    = new_w;
    img.h    = new_h;
}

std::string get_relative_path(const std::string_view relative_path, const char* env, const long long mode)
{
    const char* c_env = std::getenv(env);
    if (!c_env)
        return UNKNOWN;

    struct stat sb;
    std::string fullPath;
    fullPath.reserve(1024);

    for (const std::string& dir : split(c_env, ':'))
    {
        fullPath += dir;
        fullPath += '/';
        fullPath += relative_path;

        if (stat(fullPath.c_str(), &sb) == 0 && (sb.st_mode & mode) != 0)
            return fullPath;

        fullPath.clear();
    }

    return UNKNOWN;
}

std::string which(const std::string_view command)
{
    return get_relative_path(command, "PATH", S_IXUSR);
}

static std::vector<uint8_t> read_stdin_binary()
{
    std::vector<uint8_t> buffer;

    uint8_t temp[UINT16_MAX];  // NOLINT
    while (true)
    {
        size_t n = fread(temp, 1, sizeof(temp), stdin);
        if (n == 0)
            break;
        buffer.insert(buffer.end(), temp, temp + n);
    }

    return buffer;
}

Result<capture_result_t> load_image_rgba(const std::string& path)
{
    capture_result_t result{};

    int width    = 0;
    int height   = 0;
    int channels = 0;

    // Force RGBA output (4 channels)
    stbi_uc* pixels = nullptr;
    if (path != "-")
    {
        pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        // stb only says "can't fopen"; fopen() left the reason in errno
        if (!pixels && std::string_view(STBI_ERROR) == "can't fopen")
            return Err("{}: {}", path, strerror(errno));
    }
    else
    {
#if OSHOT_WINDOWS
        _setmode(_fileno(stdin), _O_BINARY);
#endif

        const std::vector<uint8_t>& input = read_stdin_binary();
        if (input.empty())
            return Err("No image data received from stdin");

        pixels = stbi_load_from_memory(input.data(), int(input.size()), &width, &height, &channels, STBI_rgb_alpha);
    }

    if (!pixels)
        return Err("{}: {}", path == "-" ? "stdin" : path, STBI_ERROR);

    result.w = width;
    result.h = height;

    const size_t size = size_t(width) * height * 4;
    result.data.assign(pixels, pixels + size);

    stbi_image_free(pixels);

    return Ok(std::move(result));
}

Result<std::string> get_config_image_out_fmt()
{
    // fmt formats a system_clock time_point as UTC, so convert to local time first
    const std::time_t now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm           now{};
#if OSHOT_WINDOWS
    localtime_s(&now, &now_t);
#else
    localtime_r(&now_t, &now);
#endif

    std::string out_path;
    try
    {
        out_path = fmt::format(
            fmt::runtime(g_config->File.image_out_fmt + "." + str_tolower(g_config->File.image_out_type.first)), now);
    }
    catch (fmt::format_error& err)
    {
        return Err("Bad image output format string: {}", std::string(err.what()));
    }
    return Ok(out_path);
}

static std::atomic<bool> dialogs_disabled{ false };

bool init_dialogs() noexcept
{
    static std::once_flag    once;
    static std::atomic<bool> ready{ false };

    if (dialogs_disabled)
        return false;

    try
    {
        std::call_once(once, [] {
            if (nvd_init() == 0)
                ready = true;
            else
                spdlog::warn("Failed to initialize nvdialog (error {}), dialogs are disabled",
                             static_cast<int>(nvd_get_error()));
        });
    }
    catch (const std::system_error&)
    {
        return false;
    }
    return ready;
}

void disable_dialogs() noexcept
{
    dialogs_disabled = true;
}

static bool own_spdlog_registry = false;

void set_shared_default_logger(std::shared_ptr<spdlog::logger> logger)
{
    // With a single registry (Linux, builds without plugins) it's already the default
    if (spdlog::default_logger_raw() == logger.get())
        return;
    own_spdlog_registry = true;
    spdlog::set_default_logger(std::move(logger));
}

void release_shared_default_logger()
{
    if (!own_spdlog_registry)
        return;
    // Same name, so it also replaces the old logger in the registry
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("oshot_logger", std::make_shared<spdlog::sinks::stderr_color_sink_mt>()));
}

Result<> write_image_file(const std::string& path, const capture_result_t& img, ImageExt ext)
{
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp)
        return Err("Failed to open {} to write the image: {}", path, strerror(errno));

    const std::vector<uint8_t>& data    = encode_to_image(img, ext);
    size_t                      written = fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    if (written != data.size())
        return Err("Failed to write image data to {}", path);
    return Ok();
}

Result<std::string> save_image_to_dir(const fs::path& dir, const capture_result_t& img, ImageExt ext)
{
    const Result<std::string>& name = get_config_image_out_fmt();
    TRY(name);

    std::error_code err;
    fs::create_directories(dir, err);
    if (err)
        return Err("Failed to create the folder {}: {}", dir.string(), err.message());

    const std::string path = (dir / name.get()).string();
    TRY(write_image_file(path, img, ext));
    return Ok(path);
}

Result<> save_image(SavingOp op, const capture_result_t& img, ImageExt ext)
{
    auto deleter = [](NvdNotification* p) {
        nvd_send_notification(p);
        nvd_delete_notification(p);
    };
    std::unique_ptr<NvdNotification, decltype(deleter)> notif(nullptr, deleter);

    const bool notify = init_dialogs();
    if (op == SavingOp::Clipboard)
    {
        const Result<>& res = g_clipboard.CopyImage(img, ext);
        if (res.ok() && notify)
            notif.reset(
                nvd_notification_new(_("Copied!"), _("Screenshot copied to clipboard"), NVD_NOTIFICATION_SIMPLE));
        return res;
    }

    // --instant-save=DIR: no dialog
    if (!g_config->Runtime.instant_save_dir.empty())
    {
        const Result<std::string>& saved = save_image_to_dir(g_config->Runtime.instant_save_dir, img, ext);
        TRY(saved);
        spdlog::info("Saved to {}", saved.get());
        if (notify)
            notif.reset(nvd_notification_new(_("Saved!"), _("Screenshot saved successfully"), NVD_NOTIFICATION_SIMPLE));
        return Ok();
    }

    const Result<std::string>& fmt = get_config_image_out_fmt();
    TRY(fmt);

    const bool      hidden         = hide_window();
    const fs::path& saved_path_dir = g_cache->GetValue(CacheEntry::ImgSavePath, get_home_pictures_dir().string());

    std::string filter_type = ("*." + str_tolower(g_config->File.image_out_type.first));
    const auto  filter      = std::to_array({ filter_type.c_str() });
    const char* save_path   = tinyfd_saveFileDialog(_("Save File"),
                                                    (saved_path_dir / fmt.get()).string().c_str(),  // default path
                                                    1,              // number of filter patterns
                                                    filter.data(),  // file filters
                                                    _("Images")     // filter description
    );

    if (hidden)
        show_window();

    if (!save_path)
    {
        if (notify)
            notif.reset(nvd_notification_new(_("Canceled"), _("Save canceled by user"), NVD_NOTIFICATION_WARNING));
        return Ok();  // Not really an error, maybe the user cancelled
    }

    TRY(write_image_file(save_path, img, ext));
    if (notify)
        notif.reset(nvd_notification_new(_("Saved!"), _("Screenshot saved successfully"), NVD_NOTIFICATION_SIMPLE));

    fs::path path(save_path);
    g_cache->SetValue(CacheEntry::ImgSavePath, path.parent_path().string());

    return Ok();
}

void rgba_to_grayscale(const uint8_t* src, uint8_t* result, int width, int height)
{
    const int pixels = width * height;
    for (int i = 0; i < pixels; ++i)
    {
        rgba_t c = load_rgba(src + static_cast<ptrdiff_t>(i * 4));
        // ITU-R BT.601 luminance
        result[i] = uint8_t((77 * c.r + 150 * c.g + 29 * c.b) >> 8);
    }
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

bool parse_hex_rgba(const std::string_view hex, rgba_t& out)
{
    if (hex.empty() || hex[0] != '#')
        return false;

    if (hex.size() != 7 && hex.size() != 9)
        return false;

    // strtoul needs a null-terminated string. Construct one explicitly
    // from the substring
    const std::string s(hex.substr(1));

    char*         end    = nullptr;
    unsigned long parsed = std::strtoul(s.c_str(), &end, 16);

    // Ensure the entire string was consumed and no overflow occurred.
    if (end != s.c_str() + s.size() || parsed > 0xFFFFFFFFul)
        return false;

    const auto value = uint32_t(parsed);
    rgba_t     v(hex.size() == 7 ? (value << 8) | 0xFF : value);

    out = v;
    return true;
}

std::string col_to_hexstr(const rgba_t& col)
{
    return fmt::format("#{:02x}{:02x}{:02x}{:02x}", col.r, col.g, col.b, col.a);
}

bool hexstr_to_imvec4(const std::string_view hex, ImVec4& out)
{
    rgba_t c;
    if (!parse_hex_rgba(hex, c))
        return false;
    out = c.to_imvec4();
    return true;
}

bool hexstr_to_col(const std::string_view hex, uint32_t& out)
{
    rgba_t c;
    if (!parse_hex_rgba(hex, c))
        return false;
    out = c.to_rgba();
    return true;
}

#if OSHOT_WINDOWS
static std::optional<fs::path> get_known_dir(REFKNOWNFOLDERID rfid, const char* backup_env)
{
    PWSTR widePath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, nullptr, &widePath)))
    {
        // Get required buffer size
        int         size = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
        std::string narrowPath(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, widePath, -1, &narrowPath[0], size, nullptr, nullptr);
        CoTaskMemFree(widePath);

        narrowPath.pop_back();  // the NUL
        return narrowPath;
    }

    const char* dir = std::getenv(backup_env);
    if (dir != nullptr && dir[0] != '\0' && fs::exists(dir))
        return fs::path(dir);

    return std::nullopt;
}

static bool is_hidden_directory(const fs::directory_entry& e)
{
    const std::wstring& wpath = e.path().wstring();
    DWORD               attrs = GetFileAttributesW(wpath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN);
}

fs::path get_exe_path()
{
    std::wstring buf(MAX_PATH, L'\0');
    DWORD        len = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    while (len == buf.size())
    {
        buf.resize(buf.size() * 2);
        len = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    }
    if (len == 0)
        return {};
    buf.resize(len);
    return fs::path(buf);
}

fs::path get_home_dir()
{
    if (const char* h = std::getenv("USERPROFILE"))
        return h;

    const char* d = std::getenv("HOMEDRIVE");
    const char* p = std::getenv("HOMEPATH");
    if (d && p)
        return std::string(d) + p;

    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, buf)))
        return buf;

    die("Cannot determine home directory");
}

fs::path get_home_config_dir()
{
    auto p(get_known_dir(FOLDERID_RoamingAppData, "APPDATA"));
    if (p)
        return *p;
    die("Failed to get %APPDATA% path");
}

fs::path get_home_cache_dir()
{
    auto p(get_known_dir(FOLDERID_LocalAppData, "LOCALAPPDATA"));
    if (p)
        return *p;
    die("Failed to get %LOCALAPPDATA% path");
}

fs::path get_home_pictures_dir()
{
    auto p(get_known_dir(FOLDERID_Pictures, "_1"));
    if (p)
        return *p;

    const char* dir = std::getenv("USERPROFILE");
    if (dir != nullptr && dir[0] != '\0')
    {
        fs::path pictures = fs::path(dir) / "Pictures";
        if (fs::exists(pictures))
            return pictures;
    }

    die("Failed to get Pictures directory path");
}
#else
// `is_user_dir`: a user dir like XDG_PICTURES_DIR, also looked up in user-dirs.dirs, which
// must exist. Otherwise a base dir like XDG_CONFIG_HOME, which oshot creates on first use
static fs::path get_known_dir(const char* xdg, const char* backup, bool is_user_dir)
{
    const char* dir = std::getenv(xdg);
    if (dir != nullptr && fs::path(dir).is_absolute() && (!is_user_dir || fs::exists(dir)))
        return dir;

    if (is_user_dir)
    {
        const auto& dirs = get_xdg_user_dirs();
        if (dirs.find(xdg) != dirs.end())
            return dirs.at(xdg);
    }

    return get_home_dir() / backup;
}

static bool is_hidden_directory(const fs::directory_entry& e)
{
    const std::string& name = e.path().filename().string();
    return !name.empty() && name.front() == '.';
}

fs::path get_exe_path()
{
    std::error_code ec;

#  if OSHOT_MACOS
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    return fs::canonical(buf.c_str(), ec);

#  elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#    if defined(__NetBSD__)
    int mib[4] = { CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME };
#    else
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
#    endif
    char   buf[PATH_MAX];
    size_t len = sizeof(buf);
    if (sysctl(mib, 4, buf, &len, nullptr, 0) != 0)
        return {};
    return fs::canonical(buf, ec);

#  else
    // Linux. OpenBSD has no way to get it, so this fails there and callers
    // fall back to what they'd do without the path
    return fs::canonical("/proc/self/exe", ec);
#  endif
}

fs::path get_home_dir()
{
    if (const char* h = std::getenv("HOME"))
        return h;

    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
        return pw->pw_dir;

    die("Cannot determine home directory");
}

fs::path get_home_config_dir()
{
    return get_known_dir("XDG_CONFIG_HOME", ".config", false);
}

fs::path get_home_cache_dir()
{
    return get_known_dir("XDG_CACHE_HOME", ".cache", false);
}

fs::path get_home_pictures_dir()
{
    return get_known_dir("XDG_PICTURES_DIR", "Pictures", true);
}
#endif

void register_window_callbacks(void (*minimize_fn)(),
                               bool (*hide_fn)(),
                               void (*show_fn)(),
                               void* (*dialog_parent_fn)(),
                               void (*_extern_glfwTerminate)(),
                               void (*_extern_glfwSwapInterval)(int))
{
    main_tool::minimize_fn              = minimize_fn;
    main_tool::hide_fn                  = hide_fn;
    main_tool::show_fn                  = show_fn;
    main_tool::dialog_parent_fn         = dialog_parent_fn;
    main_tool::_extern_glfwTerminate    = _extern_glfwTerminate;
    main_tool::_extern_glfwSwapInterval = _extern_glfwSwapInterval;
}

void minimize_window()
{
    if (main_tool::minimize_fn)
        main_tool::minimize_fn();
}
bool hide_window()
{
    return main_tool::hide_fn && main_tool::hide_fn();
}
void show_window()
{
    if (main_tool::show_fn)
        main_tool::show_fn();
}
void* dialog_parent_window()
{
    return main_tool::dialog_parent_fn ? main_tool::dialog_parent_fn() : nullptr;
}
void extern_glfwTerminate()
{
    if (main_tool::_extern_glfwTerminate)
        main_tool::_extern_glfwTerminate();
}
void extern_glfwSwapInterval(int v)
{
    if (main_tool::_extern_glfwSwapInterval)
        main_tool::_extern_glfwSwapInterval(v);
}

std::string expand_var(std::string ret)
{
    if (ret.empty())
        return ret;

    if (ret.front() == '~' && (ret.size() == 1 || ret[1] == '/' || ret[1] == DIR_SEP))
        ret.replace(0, 1, get_home_dir().string());

    auto is_name_char = [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

    std::string out;
    out.reserve(ret.size());
    for (size_t i = 0; i < ret.size(); ++i)
    {
        if (ret[i] != '$' || i + 1 == ret.size())
        {
            out += ret[i];
            continue;
        }

        // ${NAME} or $NAME, where NAME doesn't start with a digit
        size_t name_start = i + 1, name_end = 0, next = 0;
        if (ret[name_start] == '{')
        {
            ++name_start;
            name_end = ret.find('}', name_start);
            if (name_end == ret.npos)
            {
                out += ret[i];
                continue;
            }
            next = name_end + 1;
        }
        else
        {
            name_end = name_start;
            while (name_end < ret.size() && is_name_char(ret[name_end]))
                ++name_end;
            next = name_end;
        }

        const std::string name = ret.substr(name_start, name_end - name_start);
        const char*       value =
            (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) ? nullptr : std::getenv(name.c_str());

        // Unset variables stay as they are, so a literal '$' survives
        if (value)
            out += value;
        else
            out.append(ret, i, next - i);
        i = next - 1;
    }

    return out;
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

fs::path get_config_dir()
{
    return get_home_config_dir() / "oshot";
}

fs::path get_cache_dir()
{
    return get_home_cache_dir() / "oshot";
}

fs::path get_exe_dir()
{
    return get_exe_path().parent_path();
}

bool remove_tree(const fs::path& path) noexcept
{
    std::error_code ec;
#if OSHOT_WINDOWS
    // git marks its object files read-only, and Windows won't delete those
    for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
    ec.clear();
#endif
    fs::remove_all(path, ec);
    if (ec)
        spdlog::warn("Failed to remove '{}': {}", path.string(), ec.message());
    return !ec;
}

fs::path get_bundled_models_dir()
{
    const fs::path& exe_dir = get_exe_dir();
    if (exe_dir.empty())
        return {};

    std::error_code ec;
#if OSHOT_MACOS
    // oshot.app/Contents/MacOS/oshot -> oshot.app/Contents/Resources/tessdata
    const fs::path resources = exe_dir.parent_path() / "Resources" / "tessdata";
    if (fs::is_directory(resources, ec))
        return resources;
#endif

    const fs::path& models = exe_dir / "models";
    if (fs::is_directory(models, ec))
        return models;
    return {};
}

std::string get_default_ocr_path()
{
#if OSHOT_FREEDESKTOP
    static constexpr std::array candidates = {
#  ifdef OSHOT_TESSERACT_DATADIR
        OSHOT_TESSERACT_DATADIR "/tessdata",  // tesseract.pc's datadir, the prefix tesseract was built for
#  endif
        "/usr/share/tessdata",                     // Arch, openSUSE, Alpine, Gentoo, Void
        "/usr/share/tesseract-ocr/5/tessdata",     // Debian 12+, Ubuntu 23.04+
        "/usr/share/tesseract-ocr/4.00/tessdata",  // Debian 11, Ubuntu 22.04
        "/usr/share/tesseract/tessdata",           // Fedora, RHEL
        "/usr/local/share/tessdata",               // FreeBSD, OpenBSD, source installs
        "/usr/pkg/share/tessdata",                 // NetBSD (pkgsrc)
    };

    std::error_code ec;
    for (const char* dir : candidates)
    {
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
            if (entry.path().extension() == ".traineddata")
                return std::string(dir) + '/';
    }
    // No models installed yet: keep the path most distros use
    return "/usr/share/tessdata/";
#else
    // Windows and macOS ship eng.traineddata, which Config seeds into the config dir
    return "./models";
#endif
}

fs::path get_font_path(const std::string& font)
{
    if (font.empty())
        return {};

#if OSHOT_WINDOWS
    static constexpr std::array<std::string_view, 2> default_search_paths = {
        "C:\\Windows\\Fonts\\",
        "C:\\Windows\\Resources\\Themes\\Fonts\\",
    };
#else
    static constexpr std::array<std::string_view, 4> default_search_paths = {
        "/usr/share/fonts/",
        "/usr/local/share/fonts/",
        "~/.fonts/",
        "~/.local/share/fonts/",
    };
#endif

    fs::path font_path(font);
    if (fs::exists(font_path))
        return font_path;

    // A file directly in a font directory
    for (const std::string_view root_sv : default_search_paths)
    {
        const fs::path& root      = expand_var(std::string(root_sv));
        const fs::path& candidate = root / font_path;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec)
            return candidate;
    }

    // Anywhere under one, by file name
    for (const std::string_view root_sv : default_search_paths)
    {
        const fs::path& root = expand_var(std::string(root_sv));
        std::error_code ec;
        if (!fs::exists(root, ec) || ec)
            continue;

        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }

            const fs::directory_entry& e = *it;
            if (e.is_directory(ec) && is_hidden_directory(e))
            {
                it.disable_recursion_pending();
                continue;
            }
            if (!e.is_regular_file(ec))
                continue;

            if (e.path().filename() == font_path)
                return e.path();
        }
    }

    return {};
}

// A system font file, and the font's index in it for a collection (.ttc)
struct font_file_t
{
    std::string path;
    int         index = 0;
};

/**
 * Find the system UI font, plus the fonts covering a language's script when it lacks them.
 * Uses fontconfig on Linux/BSD, a list of the fonts shipped with the OS on Windows/macOS
 * @param lang A language code like "ru" or "zh_TW"
 * @return The fonts, the first one for Latin text too, the others to fill in its missing glyphs
 **/
static std::vector<font_file_t> find_language_fonts(const std::string& lang)
{
    std::vector<font_file_t> fonts;

#if OSHOT_FREEDESKTOP
    // fontconfig wants RFC 3066 names: "zh-tw"
    std::string fc_lang = str_tolower(lang);
    std::ranges::replace(fc_lang, '_', '-');

    if (!FcInit())
        return fonts;

    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(fmt::format("sans-serif:lang={}", fc_lang).c_str()));
    if (!pat)
        return fonts;
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult   result;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);
    if (!match)
        return fonts;

    // The best match isn't guaranteed to support the language, e.g. with no CJK font installed
    FcChar8*   file  = nullptr;
    FcLangSet* langs = nullptr;
    int        index = 0;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
        FcPatternGetLangSet(match, FC_LANG, 0, &langs) == FcResultMatch &&
        FcLangSetHasLang(langs, reinterpret_cast<const FcChar8*>(fc_lang.c_str())) != FcLangDifferentLang)
    {
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        fonts.push_back({ .path = reinterpret_cast<const char*>(file), .index = index });
    }
    FcPatternDestroy(match);
#else
    const std::string base = lang.substr(0, lang.find('_'));
#  if OSHOT_WINDOWS
    const char*    windir = std::getenv("WINDIR");
    const fs::path dir    = fs::path(windir ? windir : "C:\\Windows") / "Fonts";

    // Segoe UI covers Latin, Greek, Cyrillic, Arabic and Hebrew
    std::vector<std::string_view> names = { "segoeui.ttf" };
    if (lang == "zh_TW" || lang == "zh_HK")
        names.emplace_back("msjh.ttc");
    else if (base == "zh")
        names.emplace_back("msyh.ttc");
    else if (base == "ja")
        names.insert(names.end(), { "YuGothM.ttc", "meiryo.ttc" });
    else if (base == "ko")
        names.emplace_back("malgun.ttf");
    else if (base == "th")
        names.emplace_back("leelawui.ttf");
    else if (base == "hi" || base == "mr" || base == "ne" || base == "bn")
        names.emplace_back("Nirmala.ttf");
#  elif OSHOT_MACOS
    const fs::path dir = "/System/Library/Fonts";

    // Helvetica covers Latin, Greek and Cyrillic; Arial Unicode most other scripts
    std::vector<std::string_view> names = { "Helvetica.ttc" };
    if (base == "ja")
        names.emplace_back("ヒラギノ角ゴシック W3.ttc");
    else if (base == "ko")
        names.emplace_back("AppleSDGothicNeo.ttc");
    else if (base == "zh")
        names.emplace_back("Hiragino Sans GB.ttc");
    if (i18n::charset() == i18n::Charset::Other)
        names.emplace_back("Supplemental/Arial Unicode.ttf");
#  else
    const fs::path                dir;
    std::vector<std::string_view> names;
#  endif

    for (const std::string_view name : names)
    {
        std::error_code ec;
        const fs::path& path = dir / fs::path(std::u8string(name.begin(), name.end()));
        if (fs::exists(path, ec))
            fonts.push_back({ .path = path.string() });
    }
#endif

    return fonts;
}

void build_font_atlas(ImGuiIO& io)
{
    io.Fonts->Clear();

    ImFontConfig font_cfg;
    ImFont*      first = nullptr;

    for (const std::string& font : g_config->File.fonts)
    {
        const fs::path& path = get_font_path(font);
        if (path.empty())
        {
            spdlog::warn("Font '{}' not found, skipping", font);
            continue;
        }

        ImFont* f = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f, &font_cfg);
        if (!f)
        {
            spdlog::warn("Font '{}' failed to load", font);
            continue;
        }

        if (!first)
        {
            first = f;
            // MergeMode needs a font added first. The next fonts then fill in the glyphs
            // the first one lacks, e.g. Arabic glyphs for a font without them
            font_cfg.MergeMode = true;
        }
    }

    // The system UI font is the default one, or fills in the language's script after the configured fonts
    const std::vector<font_file_t>& system_fonts = find_language_fonts(i18n::current());
    if (system_fonts.empty() && i18n::charset() != i18n::Charset::Latin1)
        spdlog::warn("No font found for language {}: set one in Preferences > Fonts", i18n::current());

    for (const font_file_t& font : system_fonts)
    {
        font_cfg.FontNo = font.index;
        ImFont* f       = io.Fonts->AddFontFromFileTTF(font.path.c_str(), 16.0f, &font_cfg);
        if (!f)
            continue;

        spdlog::debug("Loaded font {} for language {}", font.path, i18n::current());
        if (!first)
        {
            first              = f;
            font_cfg.MergeMode = true;
        }
    }

    if (!first)
        first = io.Fonts->AddFontDefault();  // no system font found, use ImGui's built-in one

    io.FontDefault = first;
}
