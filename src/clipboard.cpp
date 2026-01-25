#include "clipboard.hpp"

#include <cstdint>

#include "clip.h"
#include "process.hpp"

#define SVPNG_LINKAGE inline
#define SVPNG_OUTPUT  std::vector<uint8_t>* output
#define SVPNG_PUT(u)  output->push_back(static_cast<uint8_t>(u))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#include "svpng.h"
#pragma GCC diagnostic pop

bool Clipboard::CopyText(const std::string& text)
{
    // Fuck you, fuck your software monopoly
    // and fuck your stupid standards that nobody wants to follow
    if (m_session != SessionType::Wayland)
        return clip::set_text(text);

    // Use foreground mode so the process doesn't fork away from our pipes.
    TinyProcessLib::Process proc({ "wl-copy", "--foreground", text }, "");
    return proc.get_exit_status() == 0;
}

bool Clipboard::CopyImage(const capture_result_t& cap)
{
    if (cap.region.width <= 0 || cap.region.height <= 0)
        return false;

    if (m_session == SessionType::Wayland)
    {
        std::vector<uint8_t> png;
        png.reserve(static_cast<size_t>(cap.region.width) * cap.region.height);

        svpng(&png, cap.region.width, cap.region.height, cap.view().data(), 1);

        // Use foreground mode so the process doesn't fork away from our pipes.
        TinyProcessLib::Process proc({ "wl-copy", "--foreground", "--type", "image/png" }, "");

        if (!proc.write(reinterpret_cast<const char*>(png.data()), png.size()))
            return false;

        proc.close_stdin();
        return proc.get_exit_status() == 0;
    }

    clip::image_spec spec;
    spec.width          = cap.region.width;
    spec.height         = cap.region.height;
    spec.bits_per_pixel = 32;
    spec.bytes_per_row  = cap.region.width * 4;

    spec.red_mask    = 0xff;
    spec.green_mask  = 0xff00;
    spec.blue_mask   = 0xff0000;
    spec.alpha_mask  = 0xff000000;
    spec.red_shift   = 0;
    spec.green_shift = 8;
    spec.blue_shift  = 16;
    spec.alpha_shift = 24;

    clip::image img(cap.view().data(), spec);
    return clip::set_image(img);
}
