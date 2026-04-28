#include "screen_capture.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "tiny-process-library/process.hpp"
#include "util.hpp"

#if defined(__linux__)
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <X11/extensions/Xrandr.h>
#  include <gio/gio.h>
#  include <unistd.h>

#  include "stb_image.h"
#elif defined(__APPLE__)
#  include <CoreGraphics/CoreGraphics.h>
#  include <unistd.h>

#  include "stb_image.h"
#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define INITGUID
#  include <d3d11.h>
#  include <dxgi1_2.h>
#  include <stdio.h>
#  include <windows.h>

#  pragma comment(lib, "d3d11")
#  pragma comment(lib, "dxgi")
#endif

using namespace spdlog;

#ifndef _WIN32
static const char* create_temp_png()
{
    char tmppath[] = "/tmp/oshot_XXXXXX.png";
    int  fd        = mkstemps(tmppath, 4);
    if (fd < 0)
        return nullptr;
    close(fd);
    return strdup(tmppath);
}
#endif

SessionType get_session_type()
{
#ifdef _WIN32
    return SessionType::Windows;
#elif defined(__APPLE__)
    return SessionType::MacOS;
#else
    const char* xdg     = std::getenv("XDG_CURRENT_DESKTOP");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* x11     = std::getenv("DISPLAY");
    const char* kde     = std::getenv("KDE_FULL_SESSION");

    if (xdg && strstr(xdg, "KDE") != nullptr)
        return SessionType::KDE;
    if (kde && kde[0] != '\0')
        return SessionType::KDE;

    xdg = std::getenv("XDG_SESSION_TYPE");

    if (xdg && strcmp(xdg, "wayland") == 0)
        return SessionType::Wayland;
    if (wayland && wayland[0] != '\0')
        return SessionType::Wayland;

    if (x11 && x11[0] != '\0')
        return SessionType::X11;
    if (xdg && strcmp(xdg, "x11") == 0)
        return SessionType::X11;
#endif

    return SessionType::Unknown;
}

#ifdef __linux__
Result<capture_result_t> capture_full_screen_portal();

// Query the geometry of the monitor under the cursor via Xlib + XRandR.
// Works on native X11 and on any Wayland compositor that runs XWayland.
// Returns false if X is unavailable (pure Wayland without XWayland).
static bool get_cursor_monitor_xrandr(Display* display, int& out_x, int& out_y, int& out_w, int& out_h)
{
    const char* disp_env = std::getenv("DISPLAY");
    if (!disp_env || disp_env[0] == '\0')
        return false;

    Display* dpy = display ? display : XOpenDisplay(disp_env);
    if (!dpy)
        return false;

    // Query the cursor position so we know which monitor the user is on
    Window       root = DefaultRootWindow(dpy);
    Window       root_ret, child_ret;
    int          root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;
    XQueryPointer(dpy, root, &root_ret, &child_ret, &root_x, &root_y, &win_x, &win_y, &mask);

    int             nmon     = 0;
    XRRMonitorInfo* monitors = XRRGetMonitors(dpy, root, True, &nmon);

    bool found = false;
    if (monitors && nmon > 0)
    {
        // Default to first monitor in case the cursor position is ambiguous
        out_x = monitors[0].x;
        out_y = monitors[0].y;
        out_w = monitors[0].width;
        out_h = monitors[0].height;
        found = true;

        debug("Found {} monitor{}", nmon, nmon > 1 ? "s" : "");
        for (int i = 0; i < nmon; ++i)
        {
            debug("Iterating monitor #{}", i);
            const int mx = monitors[i].x, my = monitors[i].y;
            const int mw = monitors[i].width, mh = monitors[i].height;
            if (root_x >= mx && root_x < mx + mw && root_y >= my && root_y < my + mh)
            {
                out_x = mx;
                out_y = my;
                out_w = mw;
                out_h = mh;
                debug("XRandR capturing: monitor {}x{}+{}+{} (cursor at {},{}) ", mw, mh, mx, my, root_x, root_y);
                break;
            }
        }
        XRRFreeMonitors(monitors);
    }

    if (!display)
        XCloseDisplay(dpy);
    return found;
}

Result<capture_result_t> capture_full_screen_x11()
{
    capture_result_t result;

    Display* display = XOpenDisplay(nullptr);
    if (!display)
    {
        warn("Failed to open X display");
        return capture_full_screen_portal();
    }

    Window root = DefaultRootWindow(display);

    int capture_x = 0, capture_y = 0;
    int capture_w = 0, capture_h = 0;

    if (!get_cursor_monitor_xrandr(display, capture_x, capture_y, capture_w, capture_h))
    {
        // Fall back to root window dimensions (old single-monitor behavior)
        warn("XRandR returned no monitors, falling back to root window size");
        XWindowAttributes attrs;
        XGetWindowAttributes(display, root, &attrs);
        capture_w = attrs.width;
        capture_h = attrs.height;
    }

    XImage* image = XGetImage(display,
                              root,
                              capture_x,
                              capture_y,
                              static_cast<unsigned int>(capture_w),
                              static_cast<unsigned int>(capture_h),
                              AllPlanes,
                              ZPixmap);
    if (!image)
    {
        warn("Failed to capture screen image");
        XCloseDisplay(display);
        return capture_full_screen_portal();
    }

    result.data = ximage_to_rgba(image, capture_w, capture_h);
    result.w    = capture_w;
    result.h    = capture_h;

    XDestroyImage(image);
    XCloseDisplay(display);

    return Ok(std::move(result));
}

// Capture the monitor that contains the pointer using KDE's `spectacle`.
// `spectacle -m` (--screen) always captures the monitor the cursor is on,
// which is exactly what we need and what the generic XDG portal does NOT
// guarantee when it runs non-interactively on a multi-monitor setup.
Result<capture_result_t> capture_full_screen_spectacle()
{
    capture_result_t result;

    const char* tmppath = create_temp_png();
    if (!tmppath)
        return Err("Failed to create temp png");

    // -b  run in background (no GUI window)
    // -n  suppress the "screenshot saved" desktop notification
    // -m  capture the monitor containing the mouse pointer
    // -o  write to the given path instead of the default Pictures folder
    TinyProcessLib::Process proc({ "spectacle", "-b", "-n", "-m", "-o", tmppath }, "");

    const int exit_code = proc.get_exit_status();
    if (exit_code != 0)
    {
        unlink(tmppath);
        ::warn("spectacle exited with code {}. Trying wayland capture...", exit_code);
        return capture_full_screen_wayland();
    }

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load(tmppath, &w, &h, &comp, STBI_rgb_alpha);
    unlink(tmppath);

    if (!rgba)
    {
        const char* reason = stbi_failure_reason();
        return Err("Failed to decode PNG: " + std::string(reason ? reason : "unknown"));
    }

    result.w = w;
    result.h = h;
    result.data.assign(rgba, rgba + size_t(w) * h * 4);
    stbi_image_free(rgba);

    return Ok(std::move(result));
}

Result<capture_result_t> capture_full_screen_wayland()
{
    const Result<capture_result_t> res = capture_full_screen_portal();
    if (res.ok())
        return res;

    capture_result_t result;

    std::vector<uint8_t>    buf;
    TinyProcessLib::Process proc({ "grim", "-t", "ppm", "-" },
                                 "",  // cwd
                                 [&](const char* bytes, size_t n) {
                                     // stdout (binary)
                                     const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes);
                                     buf.insert(buf.end(), p, p + n);
                                 });

    const int exit_code = proc.get_exit_status();

    if (exit_code != 0)
        return Err("grim failed with exit code: " + fmt::to_string(exit_code));

    // stbi_load_from_memory takes an int length
    if (buf.size() > INT_MAX)
        return Err("Screenshot too large to decode (buffer > INT_MAX).");

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load_from_memory(buf.data(), int(buf.size()), &w, &h, &comp, STBI_rgb_alpha);

    if (!rgba)
    {
        const char* reason = stbi_failure_reason();
        return Err("Failed to read PPM data: " + std::string(reason ? reason : "Unknown"));
    }

    result.w = w;
    result.h = h;
    result.data.assign(rgba, rgba + (size_t(w) * size_t(h) * 4));
    stbi_image_free(rgba);

    return Ok(std::move(result));
}

struct portal_state_t
{
    GMainLoop* loop;
    guint      subscription_id;
    guint      timeout_id;  // tracked so we can cancel it before the stack frame is gone

    std::string      png_path;
    std::string      error_msg;
    capture_result_t cap;
};

static gboolean on_timeout(gpointer user_data)
{
    portal_state_t* st = reinterpret_cast<portal_state_t*>(user_data);
    st->error_msg      = "Timed out waiting for portal response (is xdg-desktop-portal running?)";
    st->timeout_id     = 0;  // GLib will remove the source; mark it gone so cleanup skips it
    g_main_loop_quit(st->loop);
    return G_SOURCE_REMOVE;
}

static Result<std::string> uri_to_path(const char* uri)
{
    if (!uri)
        return Err("URI path to screenshot is null");

    GFile* file = g_file_new_for_uri(uri);
    if (!file)
        return Err("g_file_new_for_uri returned null");

    char* path = g_file_get_path(file);
    g_object_unref(file);
    if (!path)
        return Err("URI is not a local file path (g_file_get_path returned nullptr)");

    std::string out(path);
    g_free(path);
    return Ok(std::move(out));
}

static void on_response(GDBusConnection* conn,
                        const gchar*,  // sender_name,
                        const gchar*,  // object_path,
                        const gchar*,  // interface_name,
                        const gchar*,  // signal_name,
                        GVariant* parameters,
                        gpointer  user_data)
{
    portal_state_t* st = reinterpret_cast<portal_state_t*>(user_data);

    guint32      response = 2;
    GVariant*    results  = nullptr;
    const gchar* uri      = nullptr;

    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    if (response != 0)
        st->error_msg = fmt::format("Cancelled or failed (response={})", (unsigned)response);
    else if (!(g_variant_lookup(results, "uri", "&s", &uri) && uri))
        st->error_msg = "Success, but portal returned no uri";

    if (results)
        g_variant_unref(results);

    if (st->subscription_id)
    {
        g_dbus_connection_signal_unsubscribe(conn, st->subscription_id);
        st->subscription_id = 0;  // mark as removed so cleanup in capture_full_screen_portal skips it
    }

    const Result<std::string>& res = uri_to_path(uri);
    if (res.ok())
    {
        st->png_path  = res.get();
        st->error_msg = "";
    }
    else
    {
        st->png_path  = "";
        st->error_msg = res.error_v();
    }

    g_main_loop_quit(st->loop);
}

Result<capture_result_t> capture_full_screen_portal()
{
    portal_state_t st{};
    warn("Fallback to portal capture");

    GError*          error = nullptr;
    GDBusConnection* bus   = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus)
    {
        st.error_msg = "Failed to connect to session bus: " + std::string(error->message ? error->message : "Unknown");
        g_error_free(error);
        return Err(st.error_msg);
    }

    // Call Screenshot portal synchronously so we can fail loudly if the service isn't there.
    // Screenshot returns a "request handle" object path (type 'o').
    GVariant* reply = g_dbus_connection_call_sync(bus,
                                                  "org.freedesktop.portal.Desktop",
                                                  "/org/freedesktop/portal/desktop",
                                                  "org.freedesktop.portal.Screenshot",
                                                  "Screenshot",
                                                  g_variant_new("(sa{sv})", "", nullptr),
                                                  G_VARIANT_TYPE("(o)"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  3000,  // 3s timeout for the method call itself
                                                  nullptr,
                                                  &error);

    if (!reply)
    {
        st.error_msg = "Portal call failed: " + std::string(error->message ? error->message : "Unknown");
        g_error_free(error);
        g_object_unref(bus);
        return Err(st.error_msg);
    }

    const char* request_path = nullptr;
    g_variant_get(reply, "(&o)", &request_path);

    if (!request_path || request_path[0] == '\0')
    {
        g_variant_unref(reply);
        g_object_unref(bus);
        return Err("Portal returned an empty request handle");
    }

    st.loop            = g_main_loop_new(nullptr, FALSE);
    st.subscription_id = 0;
    st.timeout_id      = 0;

    // Subscribe specifically to the request object's Response signal.
    st.subscription_id = g_dbus_connection_signal_subscribe(bus,
                                                            "org.freedesktop.portal.Desktop",
                                                            "org.freedesktop.portal.Request",
                                                            "Response",
                                                            request_path,
                                                            nullptr,
                                                            G_DBUS_SIGNAL_FLAGS_NONE,
                                                            on_response,
                                                            &st,
                                                            nullptr);

    // Don't hang forever if nothing responds.
    // Save the source ID so we can cancel it if on_response fires first -- otherwise
    // the timeout fires after this stack frame is gone, passing a dangling portal_state_t*
    // to on_timeout and crashing on g_main_loop_quit (the SIGSEGV from the tray loop).
    st.timeout_id = g_timeout_add_seconds(5, on_timeout, &st);

    g_main_loop_run(st.loop);

    // Cancel whichever of {timeout, signal subscription} did NOT fire.
    // on_timeout  zeros st.timeout_id      before returning G_SOURCE_REMOVE.
    // on_response zeros st.subscription_id after calling signal_unsubscribe.
    // A non-zero value here means the other path exited the loop and we must
    // remove this source ourselves before st goes out of scope.
    if (st.timeout_id)
        g_source_remove(st.timeout_id);
    if (st.subscription_id)
        g_dbus_connection_signal_unsubscribe(bus, st.subscription_id);

    g_variant_unref(reply);
    g_main_loop_unref(st.loop);
    g_object_unref(bus);

    if (st.png_path.empty())
    {
        if (st.error_msg.empty())
            return Err("Failed to retrieve uri path to screenshot PNG");
        return Err(st.error_msg);
    }

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load(st.png_path.c_str(), &w, &h, &comp, STBI_rgb_alpha);

    if (!rgba)
    {
        const char* reason = stbi_failure_reason();
        unlink(st.png_path.c_str());
        return Err("Failed to read PNG data: " + std::string(reason ? reason : "Unknown"));
    }

    st.cap.w = w;
    st.cap.h = h;
    st.cap.data.assign(rgba, rgba + (size_t(w) * h * 4));
    stbi_image_free(rgba);

    // The portal backend (on KDE mostly) writes a permanent file to ~/Pictures named "Screenshot_*.png".
    // Delete it now that we have the pixels in memory.
    unlink(st.png_path.c_str());

    // The portal always captures the full virtual desktop on multi-monitor
    // setups. Crop down to the monitor that contains the cursor.
    // XRandR works on native X11 and on KDE/GNOME Wayland via XWayland.
    {
        int mx = 0, my = 0, mw = 0, mh = 0;
        if (get_cursor_monitor_xrandr(nullptr, mx, my, mw, mh) && (st.cap.w > mw || st.cap.h > mh))
        {
            debug("Portal: cropping {}x{} capture to monitor {}x{}+{}+{}", st.cap.w, st.cap.h, mw, mh, mx, my);

            const int x0    = std::max(0, mx);
            const int y0    = std::max(0, my);
            const int x1    = std::min(st.cap.w, mx + mw);
            const int y1    = std::min(st.cap.h, my + mh);
            const int new_w = x1 - x0;
            const int new_h = y1 - y0;

            if (new_w > 0 && new_h > 0)
            {
                const int            src_stride = st.cap.w;
                std::vector<uint8_t> cropped(size_t(new_w) * new_h * 4);
                for (int row = 0; row < new_h; ++row)
                {
                    const uint8_t* src = st.cap.data.data() + (size_t(y0 + row) * src_stride + x0) * 4;
                    uint8_t*       dst = cropped.data() + size_t(row) * new_w * 4;
                    std::memcpy(dst, src, size_t(new_w) * 4);
                }
                st.cap.data = std::move(cropped);
                st.cap.w    = new_w;
                st.cap.h    = new_h;
            }
        }
    }

    return Ok(std::move(st.cap));
}

#else
Result<capture_result_t> capture_full_screen_x11()
{
    return Err();
}
Result<capture_result_t> capture_full_screen_wayland()
{
    return Err();
}
Result<capture_result_t> capture_full_screen_spectacle()
{
    return Err();
}
Result<capture_result_t> capture_full_screen_portal()
{
    return Err();
}
#endif  // __linux__

// The OS automatically prompts for Screen Recording permission on first use.
#ifdef __APPLE__

// Returns the 1-based screencapture display index (-D flag) for the monitor that
// currently contains the cursor, or 1 (main display) if the query fails.
static int cursor_display_index()
{
    // Get current cursor position via CoreGraphics (no special permissions needed).
    CGEventRef event  = CGEventCreate(nullptr);
    CGPoint    cursor = CGEventGetLocation(event);
    CFRelease(event);

    // Find which display(s) contain that point.
    CGDirectDisplayID hit    = kCGNullDirectDisplay;
    uint32_t          nfound = 0;
    CGGetDisplaysWithPoint(cursor, 1, &hit, &nfound);

    if (nfound == 0 || hit == kCGNullDirectDisplay)
        return 1;  // fallback: main display

    // screencapture -D uses a 1-based index into the active display list.
    CGDirectDisplayID active[32];
    uint32_t          active_count = 0;
    CGGetActiveDisplayList(32, active, &active_count);

    for (uint32_t i = 0; i < active_count; ++i)
    {
        if (active[i] == hit)
        {
            debug("macOS capture: display index {} (CGDirectDisplayID {})", i + 1, unsigned(hit));
            return int(i + 1);
        }
    }

    return 1;  // fallback: main display
}

Result<capture_result_t> capture_full_screen_macos()
{
    capture_result_t result;

    const char* tmppath = create_temp_png();
    if (!tmppath)
        return Err("Failed to create temp png");

    // -x        suppress shutter sound
    // -t png    force PNG format
    // -D <n>    capture only display n (1-based index in active display list,
    //           matching the monitor that currently contains the cursor)
    const std::string       display_idx = fmt::to_string(cursor_display_index());
    TinyProcessLib::Process proc({ "screencapture", "-x", "-t", "png", "-D", display_idx.c_str(), tmppath }, "");

    const int exit_code = proc.get_exit_status();
    if (exit_code != 0)
    {
        unlink(tmppath);
        return Err("screencapture failed (exit " + std::to_string(exit_code) +
                   "). Please check Screen Recording permission in System Settings -> Privacy & Security");
    }

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load(tmppath, &w, &h, &comp, STBI_rgb_alpha);
    unlink(tmppath);  // clean up regardless

    if (!rgba)
    {
        const char* reason = stbi_failure_reason();
        return Err("Failed to decode screenshot PNG: " + std::string(reason ? reason : "unknown"));
    }

    result.w = w;
    result.h = h;
    result.data.assign(rgba, rgba + size_t(w) * h * 4);
    stbi_image_free(rgba);

    return Ok(std::move(result));
}
#else
Result<capture_result_t> capture_full_screen_macos()
{
    return Err();
}
#endif  // __APPLE__

#ifdef _WIN32
Result<capture_result_t> capture_full_screen_windows_fallback()
{
    capture_result_t result;

    // Find the monitor that currently contains the cursor.
    POINT    cursor_pt{};
    HMONITOR hmon = nullptr;
    if (GetCursorPos(&cursor_pt))
        hmon = MonitorFromPoint(cursor_pt, MONITOR_DEFAULTTOPRIMARY);
    else
        hmon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hmon, &mi))
    {
        // Fallback to primary monitor metrics
        mi.rcMonitor = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }

    const int origin_x = mi.rcMonitor.left;
    const int origin_y = mi.rcMonitor.top;
    const int width    = mi.rcMonitor.right - mi.rcMonitor.left;
    const int height   = mi.rcMonitor.bottom - mi.rcMonitor.top;

    debug("GDI fallback capture: monitor {}x{}+{}+{}", width, height, origin_x, origin_y);

    result.w = width;
    result.h = height;
    result.data.resize(size_t(width) * height * 4);

    // Get Device Contexts
    HDC hScreenDC = GetDC(nullptr);  // virtual-desktop DC
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);

    // Create a 32-bit bitmap for RGBA capture
    BITMAPINFO bmi              = { 0 };
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;  // Negative for top-down DIB
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    // Create DIB section
    void*   pBits   = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);

    if (!hBitmap)
    {
        DeleteDC(hMemoryDC);
        ReleaseDC(nullptr, hScreenDC);
        return Err("Failed to create DIB section");
    }

    // Select the bitmap into the memory DC
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);

    // Copy the target monitor's region from the virtual-desktop DC.
    // origin_x/origin_y are the monitor's coordinates in virtual-desktop space.
    BitBlt(hMemoryDC,
           0,
           0,
           width,
           height,
           hScreenDC,
           origin_x,
           origin_y,
           SRCCOPY | CAPTUREBLT  // CAPTUREBLT captures layered windows
    );

    // Convert BGRA DIB -> RGBA
    const uint8_t* s = reinterpret_cast<const uint8_t*>(pBits);
    uint8_t*       d = reinterpret_cast<uint8_t*>(result.data.data());

    const int n = width * height;
    for (int i = 0; i < n; ++i)
    {
        const uint8_t* p = s + i * 4;
        store_rgba(d + i * 4, rgba_t(p[2], p[1], p[0], 0xFF));
    }

    // Cleanup
    SelectObject(hMemoryDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(nullptr, hScreenDC);

    return Ok(std::move(result));
}

static bool hr_failed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        debug("{} failed: 0x{:08X}", what, unsigned(hr));
        return true;
    }
    return false;
}

template <typename T>
struct com_ptr
{
    T* ptr = nullptr;

    ~com_ptr()
    {
        if (ptr)
            ptr->Release();
    }

    T** operator&() { return &ptr; }
    T*  operator->() const { return ptr; }
        operator T*() const { return ptr; }
        operator bool() const { return ptr != nullptr; }

    T** reset_and_get_address()
    {
        if (ptr)
        {
            ptr->Release();
            ptr = nullptr;
        }
        return &ptr;
    }
};

Result<capture_result_t> capture_full_screen_windows()
{
    capture_result_t result;

    com_ptr<IDXGIFactory1> factory;
    HRESULT                hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (hr_failed(hr, "CreateDXGIFactory1"))
        return capture_full_screen_windows_fallback();

    // Identify which monitor the cursor is currently on.
    POINT    cursor_pt{};
    HMONITOR cursor_monitor = nullptr;
    if (GetCursorPos(&cursor_pt))
        cursor_monitor = MonitorFromPoint(cursor_pt, MONITOR_DEFAULTTOPRIMARY);
    else
        cursor_monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    // Enumerate adapters/outputs and prefer the one whose HMONITOR matches
    // the cursor's monitor. Fall back to the first desktop-attached output
    // if no exact match is found (e.g., GetCursorPos failed).
    com_ptr<IDXGIAdapter1> adapter, fallback_adapter;
    com_ptr<IDXGIOutput>   output, fallback_output;

    bool found_exact    = false;
    bool found_fallback = false;

    for (UINT ai = 0; factory->EnumAdapters1(ai, adapter.reset_and_get_address()) != DXGI_ERROR_NOT_FOUND; ++ai)
    {
        for (UINT oi = 0; adapter->EnumOutputs(oi, output.reset_and_get_address()) != DXGI_ERROR_NOT_FOUND; ++oi)
        {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);

            if (!od.AttachedToDesktop)
                continue;

            if (od.Monitor == cursor_monitor)
            {
                // Exact match: this is the monitor under the cursor.
                // Promote to the selected adapter/output and stop searching.
                found_exact = true;
                debug("DXGI capture: matched cursor monitor (adapter {}, output {})", ai, oi);
                break;
            }

            if (!found_fallback)
            {
                // Remember the first desktop-attached output as a fallback.
                // We can't call reset_and_get_address on the com_ptrs we still
                // need, so we re-query them if we end up using the fallback.
                found_fallback = true;
                // Store adapter/output indices for a potential second pass.
                // Simpler: just keep a raw pointer snapshot. We re-AddRef to
                // be safe because the com_ptr loop will Release on next iteration.
                fallback_adapter.ptr = adapter.ptr;
                fallback_adapter.ptr->AddRef();
                fallback_output.ptr = output.ptr;
                fallback_output.ptr->AddRef();
            }
        }

        if (found_exact)
            break;
    }

    if (!found_exact)
    {
        if (!found_fallback)
        {
            debug("No desktop-attached DXGI output found");
            return capture_full_screen_windows_fallback();
        }
        debug("DXGI capture: no output matched cursor monitor, using first desktop-attached output");
        // Swap in the fallback (adapter/output already hold the right COM objects).
        adapter.ptr          = fallback_adapter.ptr;
        fallback_adapter.ptr = nullptr;
        output.ptr           = fallback_output.ptr;
        fallback_output.ptr  = nullptr;
    }

    com_ptr<IDXGIOutput1> output1;
    hr = output->QueryInterface(IID_PPV_ARGS(&output1));
    if (hr_failed(hr, "QueryInterface(IDXGIOutput1)"))
        return capture_full_screen_windows_fallback();

    // D3D11 device bound to output adapter
    com_ptr<ID3D11Device>        device;
    com_ptr<ID3D11DeviceContext> context;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#  if DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#  endif

    hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);

#  if DEBUG
    // Debug layer not installed (Graphics Tools optional feature).
    // Retry without it so debug builds work on plain machines.
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        debug("D3D debug layer unavailable, retrying without it");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(adapter,
                               D3D_DRIVER_TYPE_UNKNOWN,
                               nullptr,
                               flags,
                               nullptr,
                               0,
                               D3D11_SDK_VERSION,
                               &device,
                               nullptr,
                               &context);
    }
#  endif

    if (hr_failed(hr, "D3D11CreateDevice"))
        return capture_full_screen_windows_fallback();

    // Output duplication
    com_ptr<IDXGIOutputDuplication> duplication;
    hr = output1->DuplicateOutput(device, &duplication);
    if (hr_failed(hr, "DuplicateOutput"))
        return capture_full_screen_windows_fallback();

    // Acquire frame
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    com_ptr<IDXGIResource>  desktop_resource;

    hr = duplication->AcquireNextFrame(100, &frame_info, &desktop_resource);

    if (hr == DXGI_ERROR_ACCESS_LOST)
        return capture_full_screen_windows_fallback();

    if (hr_failed(hr, "AcquireNextFrame"))
        return capture_full_screen_windows_fallback();

    if (frame_info.AccumulatedFrames == 0)
    {
        duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    // Desktop texture
    com_ptr<ID3D11Texture2D> desktop_texture;
    hr = desktop_resource->QueryInterface(IID_PPV_ARGS(&desktop_texture));
    if (hr_failed(hr, "QueryInterface(ID3D11Texture2D)"))
    {
        duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    D3D11_TEXTURE2D_DESC desc{};
    desktop_texture->GetDesc(&desc);

    // Expect 4 bytes per pixel formats
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        duplication->ReleaseFrame();
        debug("Unsupported DXGI format: {}", unsigned(desc.Format));
        return capture_full_screen_windows_fallback();
    }

    // Staging texture
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags      = 0;

    com_ptr<ID3D11Texture2D> staging;
    hr = device->CreateTexture2D(&desc, nullptr, &staging);
    if (hr_failed(hr, "CreateTexture2D(staging)"))
    {
        duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    context->CopyResource(staging, desktop_texture);
    context->Flush();

    // Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (hr_failed(hr, "Map"))
    {
        duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    // Copy to RGBA buffer
    const uint8_t* src    = static_cast<const uint8_t*>(mapped.pData);
    const uint32_t width  = desc.Width;
    const uint32_t height = desc.Height;

    result.w = int(width);
    result.h = int(height);
    result.data.resize(size_t(width) * height * 4);

    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = src + size_t(y) * mapped.RowPitch;
            uint8_t*       out = result.data.data() + size_t(y) * width * 4;

            for (uint32_t x = 0; x < width; ++x)
            {
                out[x * 4 + 0] = row[x * 4 + 2];  // R <- B
                out[x * 4 + 1] = row[x * 4 + 1];  // G <- G
                out[x * 4 + 2] = row[x * 4 + 0];  // B <- R
                out[x * 4 + 3] = 0xff;
            }
        }
    }
    else
    {
        // R8G8B8A8
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = src + size_t(y) * mapped.RowPitch;
            uint8_t*       out = result.data.data() + size_t(y) * width * 4;
            std::memcpy(out, row, size_t(width) * 4);
        }
    }

    context->Unmap(staging, 0);

    duplication->ReleaseFrame();

    return Ok(std::move(result));
}
#else
Result<capture_result_t> capture_full_screen_windows_fallback()
{
    return Err();
}
Result<capture_result_t> capture_full_screen_windows()
{
    return Err();
}
#endif
