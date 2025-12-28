#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdint>
#include <vector>

#include "fmt/format.h"
std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height)
{
    std::vector<uint8_t> rgba_data(width * height * 4);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            uint64_t pixel = XGetPixel(image, x, y);

            int i            = (y * width + x) * 4;
            rgba_data[i + 0] = (pixel >> 16) & 0xff;  // R
            rgba_data[i + 1] = (pixel >> 8) & 0xff;   // G
            rgba_data[i + 2] = (pixel) & 0xff;        // B
            rgba_data[i + 3] = 0xff;                  // A
        }
    }

    return rgba_data;
}

std::vector<uint8_t> ppm_to_rgba(uint8_t* ppm, int width, int height)
{
    std::vector<uint8_t> rgba_data(width * height * 4);

    for (int i = 0; i < width * height; ++i)
    {
        rgba_data[i * 4 + 0] = ppm[i * 3 + 0];  // R
        rgba_data[i * 4 + 1] = ppm[i * 3 + 1];  // G
        rgba_data[i * 4 + 2] = ppm[i * 3 + 2];  // B
        rgba_data[i * 4 + 3] = 0xff;            // A
    }

    return rgba_data;
}

std::vector<uint8_t> rgba_to_ppm(const std::vector<uint8_t>& rgba, int width, int height)
{
    std::string header = "P6\n" + fmt::to_string(width) + " " + fmt::to_string(height) + "\n255\n";

    std::vector<uint8_t> ppm_data;
    ppm_data.reserve(header.size() + (width * height * 3));
    ppm_data.insert(ppm_data.end(), header.begin(), header.end());

    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        ppm_data.push_back(rgba[i]);      // R
        ppm_data.push_back(rgba[i + 1]);  // G
        ppm_data.push_back(rgba[i + 2]);  // B
        // Skip rgba[i + 3] (alpha channel)
    }

    return ppm_data;
}
