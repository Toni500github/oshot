#include "screen_capture.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fmt/format.h"
#include "platform.hpp"
#include "util.hpp"

#if CF_LINUX
#include <unistd.h>
// X11 fallback
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#elif CF_WINDOWS
#include <windows.h>
#elif CF_MACOS
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

SessionType get_session_type()
{
#if CF_WINDOWS
    return WINDOWS;
#elif CF_MACOS
    return MACOS;
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
#if CF_LINUX
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
#if CF_LINUX
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

    result.data          = ppm_to_rgba(ppm_data.data(), width, height);
    result.region.width  = width;
    result.region.height = height;
    result.success       = true;
#endif
    return result;
}

capture_result_t capture_full_screen_windows()
{
    capture_result_t result;
#if CF_WINDOWS
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
    // The DIB section gives us BGRA format in memory (why..?)
    const uint8_t* src = static_cast<const uint8_t*>(pBits);
    uint8_t*       dst = result.data.data();

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

capture_result_t capture_full_screen_macos()
{
    capture_result_t result;
#if CF_MACOS

    CGDirectDisplayID displayID = CGMainDisplayID();
    CGImageRef screenshot = CGDisplayCreateImage(displayID);

    if (!screenshot)
    {
        result.error_msg = "Failed to create screenshot";
        return result;
    }

    size_t width         = CGImageGetWidth(screenshot);
    size_t height        = CGImageGetHeight(screenshot);
    size_t bytesPerRow   = CGImageGetBytesPerRow(screenshot);
    result.region.width  = width;
    result.region.height = height;

    // Create bitmap context
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef    context    = CGBitmapContextCreate(
        nullptr, width, height, 8, bytesPerRow, colorSpace, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);

    if (!context)
    {
        result.error_msg = "Failed to create bitmap context";
        CGImageRelease(screenshot);
        CGColorSpaceRelease(colorSpace);
        return result;
    }

    // Draw the screenshot into the context
    CGRect rect = CGRectMake(0, 0, width, height);
    CGContextDrawImage(context, rect, screenshot);

    // Get the raw RGBA data
    uint8_t* data = static_cast<uint8_t*>(CGBitmapContextGetData(context));

    if (data)
    {
        size_t dataSize = height * bytesPerRow;
        result.data.assign(data, data + dataSize);
    }

    // Clean up
    CGContextRelease(context);
    CGImageRelease(screenshot);
    CGColorSpaceRelease(colorSpace);

    result.success = true;
#endif
    return result;
}
