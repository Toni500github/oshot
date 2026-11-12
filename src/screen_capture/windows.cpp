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

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "backends.hpp"
#include "fmt/format.h"
#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "tiny-process-library/process.hpp"
#include "util/color.hpp"
#include "util/result.hpp"

// The order util.hpp used to include these in, before INITGUID
#define WIN32_LEAN_AND_MEAN
#include <combaseapi.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#define INITGUID
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <windows.h>

using namespace spdlog;

// ------------------------------
// Windows
// ------------------------------

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
    bmi.bmiHeader.biHeight      = -height;  // negative for a top-down DIB
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

    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);

    // origin_x/origin_y are the monitor's position in the virtual desktop
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

    // BGRA -> RGBA
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

    // The output of the monitor under the cursor, else the first one attached to the desktop
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
                found_exact = true;
                debug("DXGI capture: matched cursor monitor (adapter {}, output {})", ai, oi);
                break;
            }

            if (!found_fallback)
            {
                // Keep our own reference, because the loop's com_ptrs release theirs on the next iteration
                found_fallback       = true;
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
        adapter.ptr          = fallback_adapter.ptr;
        fallback_adapter.ptr = nullptr;
        output.ptr           = fallback_output.ptr;
        fallback_output.ptr  = nullptr;
    }

    com_ptr<IDXGIOutput1> output1;
    hr = output->QueryInterface(IID_PPV_ARGS(&output1));
    if (hr_failed(hr, "QueryInterface(IDXGIOutput1)"))
        return capture_full_screen_windows_fallback();

    // D3D11 device on the output's adapter
    com_ptr<ID3D11Device>        device;
    com_ptr<ID3D11DeviceContext> context;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);

#if DEBUG
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
#endif

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
                const uint8_t* bgra = row + size_t(x) * 4;
                uint8_t*       dst  = out + size_t(x) * 4;
                store_rgba(dst, rgba_t(bgra[2], bgra[1], bgra[0], 0xFF));
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
