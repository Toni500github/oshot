#ifndef _UTIL_HPP_
#define _UTIL_HPP_

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdint>
#include <vector>

std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height);
std::vector<uint8_t> ppm_to_rgba(uint8_t* ppm, int width, int height);
std::vector<uint8_t> rgba_to_ppm(const std::vector<uint8_t>& rgba, int width, int height);


#endif // !_UTIL_HPP_ 
