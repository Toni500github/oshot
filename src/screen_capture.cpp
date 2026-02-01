#include "screen_capture.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "tiny-process-library/process.hpp"
#include "util.hpp"

#if defined(__linux__)
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <gio/gio.h>
#  include <unistd.h>

#  include "stb_image.h"
#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define INITGUID
#  include <d3d11.h>
#  include <dxgi1_2.h>
#  include <stdio.h>
#  include <windows.h>

#  include "fmt/format.h"
#  pragma comment(lib, "d3d11")
#  pragma comment(lib, "dxgi")
#endif

SessionType get_session_type()
{
#ifdef _WIN32
    return SessionType::Windows;
#else
    const char* xdg     = std::getenv("XDG_SESSION_TYPE");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* x11     = std::getenv("DISPLAY");

    if (xdg && strncmp(xdg, "wayland", 8) == 0)
        return SessionType::Wayland;
    if (wayland && wayland[0] != '\0')
        return SessionType::Wayland;

    if (x11 && x11[0] != '\0')
        return SessionType::X11;
    if (xdg && strncmp(xdg, "x11", 4) == 0)
        return SessionType::X11;
#endif

    return SessionType::Unknown;
}

#ifdef __linux__
Result<capture_result_t> capture_full_screen_portal();

Result<capture_result_t> capture_full_screen_x11()
{
    capture_result_t result;

    Display* display = XOpenDisplay(nullptr);
    if (!display)
    {
        warn("Failed to open X display");
        return capture_full_screen_portal();
    }

    Window            root = DefaultRootWindow(display);
    XWindowAttributes attrs;
    XGetWindowAttributes(display, root, &attrs);

    XImage* image = XGetImage(display, root, 0, 0, attrs.width, attrs.height, AllPlanes, ZPixmap);
    if (!image)
    {
        warn("Failed to capture screen image");
        XCloseDisplay(display);
        return capture_full_screen_portal();
    }

    result.data = ximage_to_rgba(image, attrs.width, attrs.height);
    result.w    = attrs.width;
    result.h    = attrs.height;

    XDestroyImage(image);
    XCloseDisplay(display);

    return Ok(std::move(result));
}

Result<capture_result_t> capture_full_screen_wayland()
{
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
    {
        warn("grim failed with exit code {}", exit_code);
        return capture_full_screen_portal();
    }

    // stbi_load_from_memory takes an int length
    if (buf.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        warn("Screenshot too large to decode (buffer > INT_MAX).");
        return capture_full_screen_portal();
    }

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()), &w, &h, &comp, STBI_rgb_alpha);

    if (!rgba)
    {
        const char* reason = stbi_failure_reason();
        warn("Failed to read PPM data: {}", reason ? reason : "Unknown");
        return capture_full_screen_portal();
    }

    result.w = w;
    result.h = h;
    result.data.assign(rgba, rgba + (static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    stbi_image_free(rgba);

    return Ok(std::move(result));
}

struct portal_cap_context
{
    std::string      png_path;
    std::string      error_msg;
    capture_result_t cap;
} cap_portal;

struct g_state_t
{
    GMainLoop* loop;
    guint      subscription_id;
};

static gboolean on_timeout(gpointer user_data)
{
    g_state_t* st        = reinterpret_cast<g_state_t*>(user_data);
    cap_portal.error_msg = "Timed out waiting for portal response (is xdg-desktop-portal running?)";
    g_main_loop_quit(st->loop);
    return G_SOURCE_REMOVE;
}

static std::string uri_to_path(const char* uri, std::string& err_out)
{
    GFile* file = g_file_new_for_uri(uri);
    if (!file)
    {
        err_out = "g_file_new_for_uri returned null";
        return {};
    }

    char* path = g_file_get_path(file);
    g_object_unref(file);

    if (!path)
    {
        err_out = "URI is not a local file path (g_file_get_path returned NULL)";
        return {};
    }

    std::string out(path);
    g_free(path);
    return out;
}

static void on_response(GDBusConnection* conn,
                        const gchar*     sender_name,
                        const gchar*     object_path,
                        const gchar*     interface_name,
                        const gchar*     signal_name,
                        GVariant*        parameters,
                        gpointer         user_data)
{
    g_state_t* st = reinterpret_cast<g_state_t*>(user_data);

    guint32      response = 2;
    GVariant*    results  = NULL;
    const gchar* uri      = NULL;

    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    if (response != 0)
        cap_portal.error_msg = fmt::format("Cancelled or failed (response={})", (unsigned)response);
    else if (!(g_variant_lookup(results, "uri", "&s", &uri) && uri))
        cap_portal.error_msg = "Success, but portal returned no uri";

    if (results)
        g_variant_unref(results);

    if (st->subscription_id)
        g_dbus_connection_signal_unsubscribe(conn, st->subscription_id);

    cap_portal.png_path = uri_to_path(uri, cap_portal.error_msg);
    g_main_loop_quit(st->loop);
}

Result<capture_result_t> capture_full_screen_portal()
{
    warn("Fallback to portal capture");

    GError*          error = NULL;
    GDBusConnection* bus   = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus)
    {
        cap_portal.error_msg =
            "Failed to connect to session bus: " + std::string(error->message ? error->message : "Unknown");
        if (error)
            g_error_free(error);
        return Err(cap_portal.error_msg);
    }

    // Call Screenshot portal synchronously so we can fail loudly if the service isn't there.
    // Screenshot returns a "request handle" object path (type 'o').
    GVariant* reply = g_dbus_connection_call_sync(bus,
                                                  "org.freedesktop.portal.Desktop",
                                                  "/org/freedesktop/portal/desktop",
                                                  "org.freedesktop.portal.Screenshot",
                                                  "Screenshot",
                                                  g_variant_new("(sa{sv})", "", NULL),
                                                  G_VARIANT_TYPE("(o)"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  3000,  // 3s timeout for the method call itself
                                                  NULL,
                                                  &error);

    if (!reply)
    {
        cap_portal.error_msg = "Portal call failed: " + std::string(error->message ? error->message : "Unknown");
        if (error)
            g_error_free(error);
        g_object_unref(bus);
        return Err(cap_portal.error_msg);
    }

    const char* request_path = NULL;
    g_variant_get(reply, "(&o)", &request_path);

    if (!request_path || request_path[0] == '\0')
    {
        g_variant_unref(reply);
        g_object_unref(bus);
        return Err("Portal returned an empty request handle");
    }

    g_state_t st;
    st.loop = g_main_loop_new(NULL, FALSE);

    // Subscribe specifically to the request object's Response signal.
    st.subscription_id = g_dbus_connection_signal_subscribe(bus,
                                                            "org.freedesktop.portal.Desktop",
                                                            "org.freedesktop.portal.Request",
                                                            "Response",
                                                            request_path,
                                                            NULL,
                                                            G_DBUS_SIGNAL_FLAGS_NONE,
                                                            on_response,
                                                            &st,
                                                            NULL);

    // Don’t hang forever if nothing responds.
    g_timeout_add_seconds(5, on_timeout, &st);

    g_main_loop_run(st.loop);

    g_variant_unref(reply);
    g_main_loop_unref(st.loop);
    g_object_unref(bus);

    if (cap_portal.png_path.empty())
    {
        if (cap_portal.error_msg.empty())
            return Err("Failed to retrive uri path to screenshot PNG");
        return Err(cap_portal.error_msg);
    }

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load(cap_portal.png_path.c_str(), &w, &h, &comp, STBI_rgb_alpha);

    if (!rgba)
    {
        const char* reason = stbi_failure_reason();
        return Err("Failed to read PNG data: " + std::string(reason ? reason : "Unknown"));
    }

    cap_portal.cap.w = w;
    cap_portal.cap.h = h;
    cap_portal.cap.data.assign(rgba, rgba + (static_cast<size_t>(w) * h * 4));
    stbi_image_free(rgba);

    return Ok(std::move(cap_portal.cap));
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
Result<capture_result_t> capture_full_screen_portal()
{
    return Err();
}
#endif  // __linux__

#ifdef _WIN32
Result<capture_result_t> capture_full_screen_windows_fallback()
{
    capture_result_t result;

    int width  = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

    result.w = width;
    result.h = height;
    result.data.resize(width * height * 4);
    std::fill(result.data.begin(), result.data.end(), 0);

    // Get Device Contexts
    HDC hScreenDC = GetDC(NULL);
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
    HBITMAP hBitmap = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);

    if (!hBitmap)
    {
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        return Err("Failed to create DIB section");
    }

    // Select the bitmap into the memory DC
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);

    // Copy the screen to the bitmap
    BitBlt(hMemoryDC,
           0,
           0,
           width,
           height,
           hScreenDC,
           0,
           0,
           SRCCOPY | CAPTUREBLT  // CAPTUREBLT captures layered windows
    );

    // Now we have the RGB data in pBits, but we need to convert to RGBA
    // The DIB section gives us BGRA format in memory
    const uint32_t* s = reinterpret_cast<const uint32_t*>(pBits);
    uint32_t*       d = reinterpret_cast<uint32_t*>(result.data.data());

    const int n = width * height;
    for (int i = 0; i < n; ++i)
    {
        uint32_t bgra = s[i];
        // bgra: [BB][GG][RR][AA] in memory ordering for 32-bit little-endian DIB (commonly)
        uint32_t rb = (bgra & 0x00FF00FFu);
        uint32_t g  = (bgra & 0x0000FF00u);
        uint32_t r  = (rb & 0x000000FFu) << 16;
        uint32_t b  = (rb & 0x00FF0000u) >> 16;
        d[i]        = 0xFF000000u | r | g | b;
    }

    // Cleanup
    SelectObject(hMemoryDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

    return Ok(std::move(result));
}

static bool hr_failed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        debug("{} failed: 0x{:08X}", what, static_cast<unsigned>(hr));
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
};

Result<capture_result_t> capture_full_screen_windows()
{
    capture_result_t result;

    com_ptr<IDXGIFactory1> factory;
    HRESULT                hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (hr_failed(hr, "CreateDXGIFactory1"))
        return capture_full_screen_windows_fallback();

    // Find first desktop-attached output
    com_ptr<IDXGIAdapter1> adapter;
    com_ptr<IDXGIOutput>   output;

    bool found_output = false;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai)
    {
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi)
        {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);

            if (od.AttachedToDesktop)
            {
                found_output = true;
                break;
            }

            if (output)
            {
                output->Release();
                output.ptr = nullptr;
            }
        }

        if (found_output)
            break;

        if (adapter)
        {
            adapter->Release();
            adapter.ptr = nullptr;
        }
    }

    if (!found_output)
    {
        debug("No desktop-attached DXGI output found");
        return capture_full_screen_windows_fallback();
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

    bool frame_acquired = false;

    hr = duplication->AcquireNextFrame(100, &frame_info, &desktop_resource);

    if (hr == DXGI_ERROR_ACCESS_LOST)
        return capture_full_screen_windows_fallback();

    if (hr_failed(hr, "AcquireNextFrame"))
        return capture_full_screen_windows_fallback();

    frame_acquired = true;

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
        if (frame_acquired)
            duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    D3D11_TEXTURE2D_DESC desc{};
    desktop_texture->GetDesc(&desc);

    // Expect 4 bytes per pixel formats
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        if (frame_acquired)
            duplication->ReleaseFrame();
        debug("Unsupported DXGI format: {}", static_cast<unsigned>(desc.Format));
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
        if (frame_acquired)
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
        if (frame_acquired)
            duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    // Copy to RGBA buffer
    const uint8_t* src    = static_cast<const uint8_t*>(mapped.pData);
    const uint32_t width  = desc.Width;
    const uint32_t height = desc.Height;

    result.w = static_cast<int>(width);
    result.h = static_cast<int>(height);
    result.data.resize(static_cast<size_t>(width) * height * 4);

    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = src + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t*       out = result.data.data() + static_cast<size_t>(y) * width * 4;

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
            const uint8_t* row = src + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t*       out = result.data.data() + static_cast<size_t>(y) * width * 4;
            std::memcpy(out, row, static_cast<size_t>(width) * 4);
        }
    }

    context->Unmap(staging, 0);

    if (frame_acquired)
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
