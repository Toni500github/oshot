#include "util.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "clipboard.hpp"
#include "config.hpp"
#include "dotenv.h"
#include "fmt/compile.h"
#include "fmt/format.h"
#include "frozen/string.h"
#include "langs.hpp"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
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
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <pwd.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif
// clang-format on

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
                const uint8_t* row =
                    reinterpret_cast<const uint8_t*>(image->data) + static_cast<size_t>(y) * image->bytes_per_line;

                uint8_t* dst = out.data() + static_cast<size_t>(y) * width * 4;

                // Row is 4 bytes/pixel, order is typically B,G,R,X or X,R,G,B depending on endianness.
                // On little-endian with these masks, reading as uint32_t and extracting works.
                const uint32_t* px = reinterpret_cast<const uint32_t*>(row);

                for (int x = 0; x < width; ++x)
                {
                    uint32_t p     = px[x];
                    dst[x * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xff);  // R
                    dst[x * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xff);   // G
                    dst[x * 4 + 2] = static_cast<uint8_t>((p >> 0) & 0xff);   // B
                    dst[x * 4 + 3] = 0xff;                                    // A
                }
            }
            return out;
        }
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            uint32_t p = XGetPixel(image, x, y);

            int i      = (y * width + x) * 4;
            out[i + 0] = (p >> 16) & 0xff;  // R
            out[i + 1] = (p >> 8) & 0xff;   // G
            out[i + 2] = (p) & 0xff;        // B
            out[i + 3] = 0xff;              // A
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
static int g_lock_sock = -1;

bool acquire_tray_lock()
{
    g_lock_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_lock_sock < 0)
        return false;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(6015);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1

    int yes = 1;
    setsockopt(g_lock_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(g_lock_sock, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(g_lock_sock);
        g_lock_sock = -1;
        return false;
    }

    listen(g_lock_sock, 1);
    return true;
}

int get_screen_dpi()
{
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return 96;  // fallback

    double width_mm = DisplayWidthMM(dpy, DefaultScreen(dpy));
    double width_px = DisplayWidth(dpy, DefaultScreen(dpy));

    XCloseDisplay(dpy);

    // dpi = pixels per inch
    double dpi = width_px / (width_mm / 25.4);
    return static_cast<int>(dpi + 0.5);
}
#endif

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

    bool ok =
        stbir_resize_uint8_linear(img.view().data(), img_w, img_h, 0, resized.data(), new_w, new_h, 0, STBIR_RGBA);

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

        std::vector<uint8_t> input = read_stdin_binary();
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

Result<> save_png(SavingOp op, const capture_result_t& img)
{
    std::vector<uint8_t> data;
    data.reserve(static_cast<size_t>(img.w) * img.h * 4);

    svpng(&data, img.w, img.h, img.view().data(), 1);
    const size_t size = data.size();

    if (op == SavingOp::Clipboard)
        return g_clipboard->CopyImage(img);

    auto        now       = std::chrono::system_clock::now();
    const char* filter[]  = { "*.png" };
    const char* save_path = tinyfd_saveFileDialog("Save File",
                                                  fmt::format("oshot_{:%F_%H-%M}.png", now).c_str(),  // default path
                                                  1,                // number of filter patterns
                                                  filter,           // file filters
                                                  "Images (*.png)"  // filter description
    );

    if (!save_path)
        return Ok();  // Not really an error, maybe the user cancelled

    FILE* fp = fopen(save_path, "wb");
    if (!fp)
        return Err("Failed to open file to write");

    fwrite(data.data(), 1, size, fp);
    fclose(fp);
    return Ok();
}

void rgba_to_grayscale(const uint8_t* rgba, uint8_t* result, int width, int height)
{
    const int pixels = width * height;
    for (int i = 0; i < pixels; ++i)
    {
        const uint8_t r = rgba[i * 4 + 0];
        const uint8_t g = rgba[i * 4 + 1];
        const uint8_t b = rgba[i * 4 + 2];

        // ITU-R BT.601 luminance
        result[i] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
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

std::filesystem::path get_home_dir()
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

void debug_msg(const std::string_view msg) noexcept
{
    if (g_config && g_config->File.debug_print)
    {
        fmt::print(g_fp_log,
                   BOLD_COLOR((fmt::rgb(fmt::color::hot_pink))),
                   "[ {:%T} ] [DEBUG]: {}\n",
                   std::chrono::system_clock::now(),
                   msg);
    }
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

std::filesystem::path get_home_config_dir()
{
#ifndef _WIN32
    const char* dir = std::getenv("XDG_CONFIG_HOME");
    if (dir != NULL && dir[0] != '\0' && std::filesystem::exists(dir))
        return std::filesystem::path(dir);
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
    if (dir != NULL && dir[0] != '\0' && std::filesystem::exists(dir))
        return std::filesystem::path(dir);
    else
        die("Failed to get %APPDATA% path");
#endif
}

std::filesystem::path get_config_dir()
{
    return get_home_config_dir() / "oshot";
}

std::filesystem::path get_font_path(const std::string& font)
{
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
    if (std::filesystem::path(font).is_absolute())
        return font;

    for (const std::string_view path : default_search_paths)
    {
        const std::string& font_path = expand_var(fmt::format(FMT_COMPILE("{}{}"), path, font));
        if (std::filesystem::exists(font_path))
            return font_path;
    }

    return {};
}

std::filesystem::path get_lang_font_path(const std::string& lang)
{
    if (g_config->File.lang_fonts_paths.find(lang) != g_config->File.lang_fonts_paths.end())
    {
        const std::filesystem::path font_path_config(g_config->File.lang_fonts_paths[lang]);
        if (font_path_config.is_absolute())
            return font_path_config;

        return get_font_path(font_path_config.string());
    }

    const auto& it = lang_fonts.find(frozen::string(lang));
    if (it != lang_fonts.end())
    {
        for (const frozen::string font : it->second)
        {
            const auto& path = get_font_path(font.data());
            if (!path.empty())
                return path;
        }
    }

    return {};
}
