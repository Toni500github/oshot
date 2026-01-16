#include "screen_capture.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "tiny-process-library/process.hpp"
#include "util.hpp"

#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include "fmt/format.h"
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
    std::vector<uint8_t>    buf;
    TinyProcessLib::Process proc(
        { "grim", "-t", "ppm", "-" },
        "",  // cwd
        [&](const char* bytes, size_t n) {
            // stdout (binary)
            buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(bytes), reinterpret_cast<const uint8_t*>(bytes) + n);
        },
        [&](const char* bytes, size_t n) {
            // stderr (text)
            result.error_msg.assign(bytes, n);
        });

    int      w, h, comp;
    uint8_t* rgba = stbi_load_from_memory(buf.data(), buf.size(), &w, &h, &comp, STBI_rgb_alpha);
    if (!rgba)
    {
        result.error_msg =
            "Failed to read PPM data: " + std::string(stbi_failure_reason() ? stbi_failure_reason() : "Unknown");
        return result;
    }

    result.region.width  = w;
    result.region.height = h;
    result.data.assign(rgba, rgba + (w * h * 4));
    stbi_image_free(rgba);
    result.success = true;
#endif
    return result;
}

capture_result_t capture_full_screen_windows()
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

capture_result_t capture_full_screen_windows_2()
{
    capture_result_t result;
#if 0
    HRESULT hr;

    // Create D3D11 device
    com_ptr<ID3D11Device>        device;
    com_ptr<ID3D11DeviceContext> context;

    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr))
    {
        result.error_msg = "D3D11CreateDevice failed";
        return capture_full_screen_windows_fallback();
    }

    // Get primary output (monitor)
    com_ptr<IDXGIDevice> dxgiDevice;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);

    com_ptr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);

    com_ptr<IDXGIOutput> output;
    adapter->EnumOutputs(0, &output);  // primary monitor

    com_ptr<IDXGIOutput1> output1;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

    // Create duplication
    com_ptr<IDXGIOutputDuplication> duplication;

    hr = output1->DuplicateOutput(device, &duplication);
    if (FAILED(hr))
    {
        result.error_msg = "DuplicateOutput failed";
        return capture_full_screen_windows_fallback();
    }

    // Acquire frame
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    com_ptr<IDXGIResource>  resource;

    hr = duplication->AcquireNextFrame(16, &frameInfo, &resource);
    if (FAILED(hr))
    {
        result.error_msg = "AcquireNextFrame failed";
        return capture_full_screen_windows_fallback();
    }

    com_ptr<ID3D11Texture2D> gpuTex;
    resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&gpuTex);

    D3D11_TEXTURE2D_DESC desc{};
    gpuTex->GetDesc(&desc);

    const int width  = static_cast<int>(desc.Width);
    const int height = static_cast<int>(desc.Height);

    result.region.width  = width;
    result.region.height = height;
    result.data.resize(width * height * 4);

    // Create staging texture (CPU readable)
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage                = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags            = 0;
    stagingDesc.CPUAccessFlags       = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags            = 0;

    com_ptr<ID3D11Texture2D> staging;
    hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr))
    {
        result.error_msg = "CreateTexture2D (staging) failed";
        duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    context->CopyResource(staging, gpuTex);

    // Map and convert BGRA -> RGBA
    D3D11_MAPPED_SUBRESOURCE map{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr))
    {
        result.error_msg = "Map failed";
        duplication->ReleaseFrame();
        return capture_full_screen_windows_fallback();
    }

    const uint8_t* src = static_cast<const uint8_t*>(map.pData);
    uint8_t*       dst = result.data.data();

    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = src + y * map.RowPitch;
        for (int x = 0; x < width; ++x)
        {
            const int si = x * 4;
            const int di = (y * width + x) * 4;

            dst[di + 0] = row[si + 2];  // R
            dst[di + 1] = row[si + 1];  // G
            dst[di + 2] = row[si + 0];  // B
            dst[di + 3] = 0xff;         // A
        }
    }

    context->Unmap(staging, 0);
    duplication->ReleaseFrame();

    result.success = true;
#endif
    return result;
}
