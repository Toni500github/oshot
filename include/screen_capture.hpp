#ifndef _SCREEN_CAPTURE_HPP_
#define _SCREEN_CAPTURE_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct region_t
{
    int x{};
    int y{};
    int width{};
    int height{};
};

struct capture_result_t
{
    std::vector<uint8_t> data;
    bool                 success = false;
    region_t             region;
    std::string          error_msg;
};

enum SessionType
{
    WAYLAND,
    X11,
    UNKNOWN
};

using CaptureCallback = std::function<void(capture_result_t)>;

capture_result_t capture_full_screen_x11();
capture_result_t capture_full_screen_wayland();
SessionType      get_session_type();

#endif  // !_SCREEN_CAPTURE_HPP_
