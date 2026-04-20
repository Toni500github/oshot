#include "util.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

#include "clipboard.hpp"
#include "config.hpp"
#include "dotenv.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "socket.hpp"
#include "tinyfiledialogs.h"

#define SVPNG_LINKAGE inline
#define SVPNG_OUTPUT  std::vector<uint8_t>* output
#define SVPNG_PUT(u)  output->push_back(static_cast<uint8_t>(u))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#include "svpng.h"
#pragma GCC diagnostic pop

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

// clang-format off
#ifdef _WIN32
#  ifdef __MINGW64__
#    define NTDDI_VERSION NTDDI_WINBLUE
#    define _WIN32_WINNT  _WIN32_WINNT_WINBLUE
#  endif
#  include <fcntl.h>
#  include <io.h>
#  include <shellscalingapi.h>  // GetDpiForMonitor
#  include <shlobj.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "Shcore.lib")
#  pragma comment(lib, "ws2_32.lib")
#else
#  ifdef __APPLE__
#    include <CoreFoundation/CoreFoundation.h>
#    include <CoreGraphics/CoreGraphics.h>
#  endif
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <pwd.h>
#  include <fcntl.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <sys/un.h>
#endif
// clang-format on

char g_sock_path[100];
int  g_sock = -1;

constexpr ImVec4 rgba_t::to_imvec4() const
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

#if __linux__
std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height)
{
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);

    // 32bpp packed pixels
    // Faster method than XGetPixel() if possible
    if (image && image->bits_per_pixel == 32 && image->data && image->bytes_per_line >= width * 4)
    {
        if ((image->red_mask == 0x00ff0000ul) && (image->green_mask == 0x0000ff00ul) &&
            (image->blue_mask == 0x000000fful))
        {
            for (int y = 0; y < height; ++y)
            {
                const uint32_t* px =
                    reinterpret_cast<const uint32_t*>(image->data) + static_cast<size_t>(y) * image->bytes_per_line / 4;

                uint8_t* dst = out.data() + static_cast<size_t>(y) * width * 4;
                for (int x = 0; x < width; ++x)
                {
                    rgba_t c = rgba_t::from_argb(px[x]);
                    c.a      = 0xFF;
                    store_rgba(dst + x * 4, c);
                }
            }
            return out;
        }
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            rgba_t c = rgba_t::from_argb(XGetPixel(image, x, y));
            c.a      = 0xFF;
            store_rgba(out.data() + (y * width + x) * 4, c);
        }
    }
    return out;
}
#endif

#ifdef _WIN32
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

int get_screen_dpi()
{
    uint32_t dpiX = 96;
    uint32_t dpiY = 96;

    HMONITOR hMonitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    if (hMonitor && SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        return dpiX;
    return 96;  // fallback
}
#else
fs::path get_runtime_dir()
{
    const char* xdg = ::getenv("XDG_RUNTIME_DIR");
    return xdg ? fs::path(xdg) : fs::temp_directory_path();
}

bool acquire_tray_lock()
{
    // we are the "client" (not systray)
    if (g_sender->Start().ok())
        return false;

    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock < 0)
        return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    // Write 99 bytes at most to `addr.sun_path`, with the path to `oshot.sock`.
    strncpy(addr.sun_path, (get_runtime_dir() / "oshot.sock").c_str(), 99);
    // ensure null-termination
    addr.sun_path[99] = '\0';

    strncpy(g_sock_path, addr.sun_path, 100);

    unlink(addr.sun_path);  // remove stale socket
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        close(g_sock);
        g_sock = -1;
        return false;
    }

    fcntl(g_sock, F_SETFD, FD_CLOEXEC);
    listen(g_sock, 1);
    return true;
}

int get_screen_dpi()
{
#  if defined(__APPLE__)
    // CGDisplayScreenSize returns physical size in millimetres
    CGDirectDisplayID display  = CGMainDisplayID();
    CGSize            size_mm  = CGDisplayScreenSize(display);
    size_t            width_px = CGDisplayPixelsWide(display);

    if (size_mm.width <= 0)
        return 96;  // fallback

    double dpi = static_cast<double>(width_px) / (size_mm.width / 25.4);
    return static_cast<int>(dpi + 0.5);
#  else
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return 96;

    double width_mm = DisplayWidthMM(dpy, DefaultScreen(dpy));
    double width_px = DisplayWidth(dpy, DefaultScreen(dpy));
    XCloseDisplay(dpy);

    double dpi = width_px / (width_mm / 25.4);
    return static_cast<int>(dpi + 0.5);
#  endif
}
#endif

std::vector<uint8_t> encode_to_png(const capture_result_t& cap)
{
    std::vector<uint8_t> png;
    png.reserve(static_cast<size_t>(cap.w) * cap.h * 4);
    svpng(&png, cap.w, cap.h, cap.view().data(), 1);
    return png;
}

void fit_to_screen(capture_result_t& img)
{
    const int img_w = img.w;
    const int img_h = img.h;

    if (img_w <= g_scr_w && img_h <= g_scr_h)
        return;

    float scale = std::min(static_cast<float>(g_scr_w) / img_w, static_cast<float>(g_scr_h) / img_h);

    int new_w = static_cast<int>(std::round(img_w * scale));
    int new_h = static_cast<int>(std::round(img_h * scale));

    std::vector<uint8_t> resized(new_w * new_h * 4);

    bool ok = stbir_resize_uint8_linear(img.data.data(), img_w, img_h, 0, resized.data(), new_w, new_h, 0, STBIR_RGBA);

    if (!ok)
        return;

    img.data = std::move(resized);
    img.w    = new_w;
    img.h    = new_h;
}

static std::vector<uint8_t> read_stdin_binary()
{
    std::vector<uint8_t> buffer;

    uint8_t temp[UINT16_MAX];
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
    }
    else
    {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif

        const std::vector<uint8_t>& input = read_stdin_binary();
        if (input.empty())
            return Err("stdin reported data but was empty");

        pixels = stbi_load_from_memory(
            input.data(), static_cast<int>(input.size()), &width, &height, &channels, STBI_rgb_alpha);
    }

    if (!pixels)
        return Err("Failed to load image: " +
                   (stbi_failure_reason() ? std::string(stbi_failure_reason()) : "Unknown Error"));

    result.w = width;
    result.h = height;

    const size_t size = static_cast<size_t>(width) * height * 4;
    result.data.assign(pixels, pixels + size);

    stbi_image_free(pixels);

    return Ok(std::move(result));
}

Result<std::string> get_config_image_out_fmt()
{
    auto        now = std::chrono::system_clock::now();
    std::string out_path;
    try
    {
        out_path = fmt::format(fmt::runtime(g_config->File.image_out_fmt + ".png"), now);
    }
    catch (fmt::format_error& err)
    {
        return Err("Bad image output format string: " + std::string(err.what()));
    }
    return Ok(out_path);
}

Result<> save_png(SavingOp op, const capture_result_t& img)
{
    if (op == SavingOp::Clipboard)
        return g_clipboard->CopyImage(img);

    const Result<std::string>& r = get_config_image_out_fmt();
    if (!r.ok())
        return r.error();

    minimize_window();

    const char* filter[]  = { "*.png" };
    const char* save_path = tinyfd_saveFileDialog("Save File",
                                                  r.get().c_str(),  // default path
                                                  1,                // number of filter patterns
                                                  filter,           // file filters
                                                  "Images (*.png)"  // filter description
    );

    maximize_window();

    if (!save_path)
        return Ok();  // Not really an error, maybe the user cancelled

    FILE* fp = fopen(save_path, "wb");
    if (!fp)
        return Err("Failed to open file to write");

    const std::vector<uint8_t>& data    = encode_to_png(img);
    size_t                      written = fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    if (written != data.size())
        return Err("Failed to write image data");
    return Ok();
}

void rgba_to_grayscale(const uint8_t* src, uint8_t* result, int width, int height)
{
    const int pixels = width * height;
    for (int i = 0; i < pixels; ++i)
    {
        rgba_t c = load_rgba(src + i * 4);
        // ITU-R BT.601 luminance
        result[i] = static_cast<uint8_t>((77 * c.r + 150 * c.g + 29 * c.b) >> 8);
    }
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

    uint32_t               value;
    const std::string_view s = hex.data() + 1;
    if (std::from_chars(s.data(), s.data() + s.size(), value, 16).ec != std::errc())
        return false;

    rgba_t v(hex.size() == 7 ? (value << 8) : value);
    if (hex.size() == 7)
        v.a = 0xFF;

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

bool is_system_dark_mode()
{
#if defined(_WIN32)
    DWORD value = 1;
    DWORD size  = sizeof(value);
    // AppsUseLightTheme == 0  -> dark mode
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme",
                 RRF_RT_DWORD,
                 nullptr,
                 &value,
                 &size);
    return value == 0;

#elif defined(__APPLE__)
    // CFPreferencesCopyAppValue returns nullptr when no preference is set (= light)
    CFStringRef style =
        (CFStringRef)CFPreferencesCopyAppValue(CFSTR("AppleInterfaceStyle"), kCFPreferencesAnyApplication);
    if (!style)
        return false;
    bool dark = (CFStringCompare(style, CFSTR("Dark"), kCFCompareCaseInsensitive) == kCFCompareEqualTo);
    CFRelease(style);
    return dark;

#elif defined(__linux__)
    // 1. GSettings (GNOME / most DEs)
    if (FILE* f = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r"))
    {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), f))
        {
            pclose(f);
            return std::string_view(buf).find("dark") != std::string_view::npos;
        }
        pclose(f);
    }

    // 2. GTK_THEME env var (e.g. "Adwaita:dark")
    if (const char* t = ::getenv("GTK_THEME"))
    {
        std::string s(t);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s.find("dark") != std::string::npos)
            return true;
        if (s.find("light") != std::string::npos)
            return false;
    }

    // 3. ~/.config/gtk-3.0/settings.ini
    // gtk-application-prefer-dark-theme=1
    if (const char* home = std::getenv("HOME"))
    {
        std::ifstream ini(std::string(home) + "/.config/gtk-3.0/settings.ini");
        for (std::string line; std::getline(ini, line);)
            if (line.find("gtk-application-prefer-dark-theme") != std::string::npos &&
                line.find('1') != std::string::npos)
                return true;
    }
#endif

    return true;
}

fs::path get_home_dir()
{
#ifdef _WIN32
    if (const char* h = std::getenv("USERPROFILE"))
        return h;

    const char* d = std::getenv("HOMEDRIVE");
    const char* p = std::getenv("HOMEPATH");
    if (d && p)
        return std::string(d) + p;

    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, buf)))
        return buf;
#else
    if (const char* h = std::getenv("HOME"))
        return h;

    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
        return pw->pw_dir;

#endif
    die("Cannot determine home directory");
}

std::string expand_var(std::string ret, bool dont)
{
    if (ret.empty() || dont)
        return ret;

    if (ret.front() == '~')
    {
        ret.replace(0, 1, get_home_dir().string());
    }

    Dotenv env;
    env.parse_line(ret);

    return ret;
}

fs::path get_home_config_dir()
{
#ifndef _WIN32
    const char* dir = std::getenv("XDG_CONFIG_HOME");
    if (dir != NULL && dir[0] != '\0' && fs::exists(dir))
        return fs::path(dir);
    else
        return get_home_dir() / ".config";
#else
    PWSTR widePath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &widePath)))
    {
        // Get required buffer size
        int         size = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, NULL, 0, NULL, NULL);
        std::string narrowPath(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, widePath, -1, &narrowPath[0], size, NULL, NULL);
        CoTaskMemFree(widePath);

        // Remove null terminator from string
        narrowPath.pop_back();
        return narrowPath;
    }
    const char* dir = std::getenv("APPDATA");
    if (dir != NULL && dir[0] != '\0' && fs::exists(dir))
        return fs::path(dir);
    else
        die("Failed to get %APPDATA% path");
#endif
}

fs::path get_config_dir()
{
    return get_home_config_dir() / "oshot";
}

fs::path get_font_path(const std::string& font)
{
    if (font.empty())
        return {};

#ifdef _WIN32
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

    // Direct join (fast)
    for (const std::string_view root_sv : default_search_paths)
    {
        const fs::path& root      = expand_var(std::string(root_sv));
        const fs::path& candidate = root / font_path;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec)
            return candidate;
    }

    // Recursive filename match (correct)
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

            const auto& e = *it;
            if (!e.is_regular_file(ec))
                continue;

            if (e.path().filename() == font_path)
                return e.path();
        }
    }

    return {};
}
