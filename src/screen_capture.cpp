#include "screen_capture.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fmt/format.h"
#include "util.hpp"

#ifdef __linux__
#include <unistd.h>

// X11 fallback
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <windows.h>
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#endif

// Windows only
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

SessionType get_session_type()
{
#ifdef _WIN32
    return OS_WINDOWS;
#endif

    const char* xdg     = std::getenv("XDG_SESSION_TYPE");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* x11     = std::getenv("DISPLAY");

    if (xdg && strncmp(xdg, "wayland", 8) == 0)
        return WAYLAND;
    if (wayland && wayland[0] != '\0')
        return WAYLAND;

    if (x11 && x11[0] != '\0')
        return X11;
    if (xdg && strncmp(xdg, "x11", 4) == 0)
        return X11;

    return UNKNOWN;
}

capture_result_t capture_full_screen_x11()
{
    capture_result_t result;
#ifdef __linux__
    Display* display = XOpenDisplay(nullptr);
    if (!display)
    {
        result.error_msg = "Failed to open X display";
        return result;
    }

    Window            root = DefaultRootWindow(display);
    XWindowAttributes attrs;
    XGetWindowAttributes(display, root, &attrs);

    XImage* image = XGetImage(display, root, 0, 0, attrs.width, attrs.height, AllPlanes, ZPixmap);
    if (!image)
    {
        result.error_msg = "Failed to capture screen image";
        XCloseDisplay(display);
        return result;
    }

    result.data          = ximage_to_rgba(image, attrs.width, attrs.height);
    result.region.width  = attrs.width;
    result.region.height = attrs.height;
    result.success       = true;

    XDestroyImage(image);
    XCloseDisplay(display);
#endif
    return result;
}

capture_result_t capture_full_screen_wayland()
{
    capture_result_t result;
#ifdef __linux__
    std::FILE* pipe = popen("grim -t ppm -", "r");
    if (!pipe)
    {
        result.error_msg = "Failed to execute grim";
        return result;
    }

    char magic[3];
    int  width{}, height{}, maxval{};
    if (fscanf(pipe, "%2s %d %d %d", magic, &width, &height, &maxval) != 4 || magic[0] != 'P' || magic[1] != '6')
    {
        pclose(pipe);
        result.error_msg = "Invalid PPM format from grim";
        return result;
    }
    fgetc(pipe);  // skip newline

    std::vector<uint8_t> ppm_data(width * height * 3);
    if (fread(ppm_data.data(), 1, ppm_data.size(), pipe) != ppm_data.size())
    {
        pclose(pipe);
        result.error_msg = fmt::format("Failed to read PPM data: {}", strerror(errno));
        return result;
    }
    pclose(pipe);

    result.data          = ppm_to_rgba(ppm_data, width, height);
    result.region.width  = width;
    result.region.height = height;
    result.success       = true;
#endif
    return result;
}

capture_result_t capture_full_screen_windows_fallback()
{
    capture_result_t result;
#ifdef _WIN32
    int width  = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

    result.region.width  = width;
    result.region.height = height;
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
        result.error_msg = "Failed to create DIB section";
        return result;
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
    std::span<const uint8_t> src(static_cast<const uint8_t*>(pBits), width * height * 4);
    std::span<uint8_t>       dst(result.data);

    // Convert BGRA to RGBA
    for (int i = 0; i < width * height; ++i)
    {
        dst[i * 4 + 0] = src[i * 4 + 2];  // R <- B
        dst[i * 4 + 1] = src[i * 4 + 1];  // G <- G
        dst[i * 4 + 2] = src[i * 4 + 0];  // B <- R
        dst[i * 4 + 3] = 0xff;
    }

    // Cleanup
    SelectObject(hMemoryDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

    result.success = true;
#endif
    return result;
}

#ifdef _WIN32
static bool hr_failed(HRESULT hr, capture_result_t& result, const char* what)
{
    if (FAILED(hr))
    {
        result.error_msg = fmt::format("{} failed: 0x{:08X}", what, (unsigned)hr);
        return true;
    }
    return false;
}
#endif

capture_result_t capture_full_screen_windows()
{
    capture_result_t result;

#ifdef _WIN32
    // DXGI factory
    com_ptr<IDXGIFactory1> factory;
    HRESULT                hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (hr_failed(hr, result, "CreateDXGIFactory1"))
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
        result.error_msg = "No desktop-attached DXGI output found";
        return capture_full_screen_windows_fallback();
    }

    com_ptr<IDXGIOutput1> output1;
    hr = output->QueryInterface(IID_PPV_ARGS(&output1));
    if (hr_failed(hr, result, "QueryInterface(IDXGIOutput1)"))
        return capture_full_screen_windows_fallback();

    // D3D11 device bound to output adapter
    com_ptr<ID3D11Device>        device;
    com_ptr<ID3D11DeviceContext> context;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);

    if (hr_failed(hr, result, "D3D11CreateDevice"))
        return capture_full_screen_windows_fallback();

    // Output duplication
    com_ptr<IDXGIOutputDuplication> duplication;
    hr = output1->DuplicateOutput(device, &duplication);
    if (hr_failed(hr, result, "DuplicateOutput"))
        return capture_full_screen_windows_fallback();

    // Acquire frame
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    com_ptr<IDXGIResource>  desktop_resource;

    bool frame_acquired = false;

    hr = duplication->AcquireNextFrame(100, &frame_info, &desktop_resource);

    if (hr == DXGI_ERROR_ACCESS_LOST)
        return capture_full_screen_windows_fallback();

    if (hr_failed(hr, result, "AcquireNextFrame"))
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
    if (hr_failed(hr, result, "QueryInterface(ID3D11Texture2D)"))
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
        result.error_msg = fmt::format("Unsupported DXGI format: {}", (unsigned)desc.Format);
        return capture_full_screen_windows_fallback();
    }

    // Staging texture
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags      = 0;

    com_ptr<ID3D11Texture2D> staging;
    hr = device->CreateTexture2D(&desc, nullptr, &staging);
    if (hr_failed(hr, result, "CreateTexture2D(staging)"))
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
    if (hr_failed(hr, result, "Map"))
    {
        if (frame_acquired)
            duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    // Black-frame detection
    const uint8_t* src       = static_cast<const uint8_t*>(mapped.pData);
    bool           all_black = true;
    for (int i = 0; i < 64; ++i)
    {
        if (src[i] != 0)
        {
            all_black = false;
            break;
        }
    }

    if (all_black)
    {
        context->Unmap(staging, 0);
        if (frame_acquired)
            duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    // Copy to RGBA buffer
    const uint32_t width  = desc.Width;
    const uint32_t height = desc.Height;

    result.region.width  = (int)width;
    result.region.height = (int)height;
    result.data.resize((size_t)width * (size_t)height * 4);

    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = src + (size_t)y * mapped.RowPitch;
            uint8_t*       out = result.data.data() + (size_t)y * (size_t)width * 4;

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
            std::memcpy(result.data.data() + (size_t)y * (size_t)width * 4,
                        src + (size_t)y * mapped.RowPitch,
                        (size_t)width * 4);
        }
    }

    context->Unmap(staging, 0);

    if (frame_acquired)
        duplication->ReleaseFrame();

    result.success = true;
#endif

    return result;
}
