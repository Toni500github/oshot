#include "screen_capture.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fmt/format.h"
#include "util.hpp"

#ifdef __linux__
// X11 fallback
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

SessionType get_session_type()
{
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

    return result;
}
