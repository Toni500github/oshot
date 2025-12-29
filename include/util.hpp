#ifndef _UTIL_HPP_
#define _UTIL_HPP_

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdint>
#include <string>
#include <vector>

std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height);
std::vector<uint8_t> ppm_to_rgba(uint8_t* ppm, int width, int height);
std::vector<uint8_t> rgba_to_ppm(const std::vector<uint8_t>& rgba, int width, int height);
std::string replace_str(std::string str, const std::string_view from, const std::string_view to);

#endif  // !_UTIL_HPP_
