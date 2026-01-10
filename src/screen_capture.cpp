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
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#endif

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

capture_result_t capture_full_screen_windows()
{
    capture_result_t result;

#ifdef _WIN32
    HRESULT hr;

    // Create D3D11 device
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;

    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);

    if (FAILED(hr))
    {
        result.error_msg = "D3D11CreateDevice failed";
        return result;
    }

    // Get primary output (monitor)
    IDXGIDevice* dxgiDevice = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);

    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);

    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);  // primary monitor

    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

    // Create duplication
    IDXGIOutputDuplication* duplication = nullptr;
    hr                                  = output1->DuplicateOutput(device, &duplication);

    if (FAILED(hr))
    {
        result.error_msg = "DuplicateOutput failed";
        goto cleanup;
    }

    // Acquire frame
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    IDXGIResource*          resource = nullptr;

    hr = duplication->AcquireNextFrame(1000, &frameInfo, &resource);
    if (FAILED(hr))
    {
        result.error_msg = "AcquireNextFrame failed";
        goto cleanup;
    }

    ID3D11Texture2D* gpuTex = nullptr;
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

    ID3D11Texture2D* staging = nullptr;
    hr                       = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr))
    {
        result.error_msg = "CreateTexture2D (staging) failed";
        duplication->ReleaseFrame();
        goto cleanup;
    }

    context->CopyResource(staging, gpuTex);

    // Map and convert BGRA -> RGBA
    D3D11_MAPPED_SUBRESOURCE map{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr))
    {
        result.error_msg = "Map failed";
        duplication->ReleaseFrame();
        goto cleanup;
    }

    std::span<const uint8_t> src(static_cast<const uint8_t*>(map.pData));
    std::span<uint8_t>       dst(result.data);

    for (int y = 0; y < height; ++y)
    {
        std::span<const uint8_t> row(src + y * map.RowPitch);
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

    // clang-format off
cleanup:
    if (staging)     staging->Release();
    if (gpuTex)      gpuTex->Release();
    if (resource)    resource->Release();
    if (duplication) duplication->Release();
    if (output1)     output1->Release();
    if (output)      output->Release();
    if (adapter)     adapter->Release();
    if (dxgiDevice)  dxgiDevice->Release();
    if (context)     context->Release();
    if (device)      device->Release();

#endif

    return result;
}
