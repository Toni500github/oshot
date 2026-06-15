#include "util.hpp"

#include <fcntl.h>

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
#include "dotenv.hpp"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "nvdialog/nvdialog_notification.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "tinyfiledialogs.h"

#define SVPNG_LINKAGE inline
#define SVPNG_OUTPUT  std::vector<uint8_t>* output
#define SVPNG_PUT(u)  output->push_back(uint8_t(u))
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
#  include <io.h>
#  include <shellscalingapi.h>  // GetDpiForMonitor
#  include <shlobj.h>
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
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/file.h>
#  include <unistd.h>
#  include <sys/un.h>
#endif
// clang-format on

char g_sock_path[100];
int  g_sock = -1;

static const std::unordered_map<std::string, std::string>& get_xdg_user_dirs()
{
    static std::unordered_map<std::string, std::string> cache;
    if (!cache.empty())
        return cache;

#ifndef _WIN32
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

constexpr ImVec4 rgba_t::to_imvec4() const
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

#if __linux__
std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height)
{
    std::vector<uint8_t> out(size_t(width) * height * 4);

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
                    reinterpret_cast<const uint32_t*>(image->data) + size_t(y) * image->bytes_per_line / 4;

                uint8_t* dst = out.data() + size_t(y) * width * 4;
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
    std::string lock_path = (get_runtime_dir() / "oshot.lock").string();
    int         fd        = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return false;

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

int get_screen_dpi()
{
#  if defined(__APPLE__)
    // CGDisplayScreenSize returns physical size in millimetres
    CGDirectDisplayID display  = CGMainDisplayID();
    CGSize            size_mm  = CGDisplayScreenSize(display);
    size_t            width_px = CGDisplayPixelsWide(display);

    if (size_mm.width <= 0)
        return 96;  // fallback

    double dpi = double(width_px) / (size_mm.width / 25.4);
    return int(dpi + 0.5);
#  else
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return 96;

    double width_mm = DisplayWidthMM(dpy, DefaultScreen(dpy));
    double width_px = DisplayWidth(dpy, DefaultScreen(dpy));
    XCloseDisplay(dpy);

    double dpi = width_px / (width_mm / 25.4);
    return int(dpi + 0.5);
#  endif
}
#endif

std::vector<uint8_t> encode_to_png(const capture_result_t& cap)
{
    std::vector<uint8_t> png;
    png.reserve(size_t(cap.w) * cap.h * 4);
    svpng(&png, cap.w, cap.h, cap.view().data(), 1);
    return png;
}

void fit_to_screen(capture_result_t& img)
{
    const int img_w = img.w;
    const int img_h = img.h;

    if (img_w <= g_scr_w && img_h <= g_scr_h)
        return;

    float scale = std::min(float(g_scr_w) / img_w, float(g_scr_h) / img_h);

    int new_w = int(std::round(img_w * scale));
    int new_h = int(std::round(img_h * scale));

    std::vector<uint8_t> resized(new_w * new_h * 4);

    bool ok = stbir_resize_uint8_linear(img.data.data(), img_w, img_h, 0, resized.data(), new_w, new_h, 0, STBIR_RGBA);
    if (!ok)
    {
        warn("Failed to resize image: {}", STBI_ERROR);
        return;
    }

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
            return Err("No image data received from stdin");

        pixels = stbi_load_from_memory(input.data(), int(input.size()), &width, &height, &channels, STBI_rgb_alpha);
    }

    if (!pixels)
        return Err("Failed to load image: " + STBI_ERROR);

    result.w = width;
    result.h = height;

    const size_t size = size_t(width) * height * 4;
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
    auto deleter = [](NvdNotification* p) {
        nvd_send_notification(p);
        nvd_delete_notification(p);
    };
    std::unique_ptr<NvdNotification, decltype(deleter)> notif(nullptr, deleter);

    if (op == SavingOp::Clipboard)
    {
        const Result<>& res = g_clipboard.CopyImage(img);
        if (res.ok())
            notif.reset(nvd_notification_new("Copied!", "Screenshot copied to clipboard", NVD_NOTIFICATION_SIMPLE));
        return res;
    }

    const Result<std::string>& fmt = get_config_image_out_fmt();
    TRY(fmt);

    minimize_window();
    const fs::path& saved_path_dir = g_cache->GetValue(CacheEntry::ImgSavePath, get_home_pictures_dir().string());

    const char* filter[]  = { "*.png" };
    const char* save_path = tinyfd_saveFileDialog("Save File",
                                                  (saved_path_dir / fmt.get()).string().c_str(),  // default path
                                                  1,                // number of filter patterns
                                                  filter,           // file filters
                                                  "Images (*.png)"  // filter description
    );

    maximize_window();

    if (!save_path)
    {
        notif.reset(nvd_notification_new("Canceled", "Save canceled by user", NVD_NOTIFICATION_WARNING));
        return Ok();  // Not really an error, maybe the user cancelled
    }

    FILE* fp = fopen(save_path, "wb");
    if (!fp)
        return Err("Failed to open file to write image");

    const std::vector<uint8_t>& data    = encode_to_png(img);
    size_t                      written = fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    if (written != data.size())
        return Err("Failed to write image data");
    notif.reset(nvd_notification_new("Saved!", "Screenshot saved successfully", NVD_NOTIFICATION_SIMPLE));

    fs::path path(save_path);
    g_cache->SetValue(CacheEntry::ImgSavePath, path.parent_path().string());

    return Ok();
}

void rgba_to_grayscale(const uint8_t* src, uint8_t* result, int width, int height)
{
    const int pixels = width * height;
    for (int i = 0; i < pixels; ++i)
    {
        rgba_t c = load_rgba(src + i * 4);
        // ITU-R BT.601 luminance
        result[i] = uint8_t((77 * c.r + 150 * c.g + 29 * c.b) >> 8);
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

#ifdef _WIN32
static std::optional<fs::path> get_known_dir(REFKNOWNFOLDERID rfid, const char* backup_env)
{
    PWSTR widePath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, NULL, &widePath)))
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

    const char* dir = std::getenv(backup_env);
    if (dir != NULL && dir[0] != '\0' && fs::exists(dir))
        return fs::path(dir);

    return nullopt;
}

static bool is_hidden_directory(const fs::directory_entry& e)
{
    const std::wstring& wpath = e.path().wstring();
    DWORD               attrs = GetFileAttributesW(wpath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN);
}

bool is_system_dark_mode()
{
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
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, buf)))
        return buf;

    die("Cannot determine home directory");
}

fs::path get_home_config_dir()
{
    auto p(get_known_dir(FOLDERID_AppData, "APPDATA"));
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
    if (dir != NULL && dir[0] != '\0')
    {
        fs::path pictures = fs::path(dir) / "Pictures";
        if (fs::exists(pictures))
            return pictures;
    }

    die("Failed to get Pictures directory path");
}
#else
static fs::path get_known_dir(const char* xdg, const char* backup, bool search_xdg_user_dirs = true)
{
    const char* dir = std::getenv(xdg);
    if (dir != NULL && dir[0] != '\0' && fs::exists(dir))
        return fs::path(dir);

    if (search_xdg_user_dirs)
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

bool is_system_dark_mode()
{
#  ifdef __APPLE__
    // CFPreferencesCopyAppValue returns nullptr when no preference is set (= light)
    CFStringRef style =
        (CFStringRef)CFPreferencesCopyAppValue(CFSTR("AppleInterfaceStyle"), kCFPreferencesAnyApplication);
    if (!style)
        return false;
    bool dark = (CFStringCompare(style, CFSTR("Dark"), kCFCompareCaseInsensitive) == kCFCompareEqualTo);
    CFRelease(style);
    return dark;

#  else
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
    std::ifstream ini((get_home_dir() / ".config/gtk-3.0/settings.ini").string());
    for (std::string line; std::getline(ini, line);)
        if (line.find("gtk-application-prefer-dark-theme") != std::string::npos && line.find('1') != std::string::npos)
            return true;
#  endif
    return true;
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
    return get_known_dir("XDG_CACHE_HOME", ".cache");
}

fs::path get_home_pictures_dir()
{
    return get_known_dir("XDG_PICTURES_DIR", "Pictures");
}
#endif

std::string expand_var(std::string ret)
{
    if (ret.empty())
        return ret;

    if (ret.front() == '~')
        ret.replace(0, 1, get_home_dir().string());

    Dotenv env;
    env.parse_line(ret);

    return ret;
}

fs::path get_config_dir()
{
    return get_home_config_dir() / "oshot";
}

fs::path get_cache_dir()
{
    return get_home_cache_dir() / "oshot";
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
            warn("Font '{}' not found, skipping", font);
            continue;
        }

        ImFont* f = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f, &font_cfg);
        if (!f)
        {
            warn("Font '{}' failed to load", font);
            continue;
        }

        if (!first)
        {
            first = f;
            // this value is false by default, and we can't set it to true without adding atleast one font first.
            // so, after we add the first font, this will be true (and will stay true).
            // MergeMode fills the gap in previous fonts with glyphs from this font, for example, adding Arabic glyphs to a non-Arabic font.
            font_cfg.MergeMode = true;
        }
    }

    if (!first)
        first = io.Fonts->AddFontDefault();  // nothing loaded, use ImGui built-in

    io.FontDefault = first;
}
