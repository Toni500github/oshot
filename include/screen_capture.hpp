#ifndef _SCREEN_CAPTURE_HPP_
#define _SCREEN_CAPTURE_HPP_

#include <cstdint>
#include <functional>
#include <span>
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
    std::vector<uint8_t> data;  // RGBA
    bool                 success = false;
    int                  w       = 0;
    int                  h       = 0;
    std::string          error_msg;

    std::span<const uint8_t> view() const { return data; }
    std::span<uint8_t>       view() { return data; }
};

enum class SessionType
{
    Wayland,
    X11,
    Windows,
    Unknown
};

using CaptureCallback = std::function<void(capture_result_t)>;

capture_result_t capture_full_screen_x11();
capture_result_t capture_full_screen_wayland();
capture_result_t capture_full_screen_windows();

SessionType get_session_type();

#endif  // !_SCREEN_CAPTURE_HPP_
