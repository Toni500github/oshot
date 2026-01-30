#include "clipboard.hpp"

#include <cstdint>

#include "clip/clip.h"
#include "tiny-process-library/process.hpp"

#define SVPNG_LINKAGE inline
#define SVPNG_OUTPUT  std::vector<uint8_t>* output
#define SVPNG_PUT(u)  output->push_back(static_cast<uint8_t>(u))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#include "svpng.h"
#pragma GCC diagnostic pop

Result<> Clipboard::CopyText(const std::string& text)
{
    // Fuck you, fuck your software monopoly
    // and fuck your stupid standards that nobody wants to follow
    if (m_session != SessionType::Wayland)
    {
        if (clip::set_text(text))
            return Ok();
        return Err("Failed to copy text into clipboard");
    }

    std::string err;

    // Use foreground mode so the process doesn't fork away from our pipes.
    TinyProcessLib::Process proc(
        { "wl-copy", "--foreground", text }, "", nullptr, [&](const char* b, size_t n) { err.append(b, n); });
    if (proc.get_exit_status() == 0)
        return Ok();
    return Err("Failed to copy text into clipboard: " + err);
}

Result<> Clipboard::CopyImage(const capture_result_t& cap)
{
    if (cap.w <= 0 || cap.h <= 0)
        return Err("Image size is 0");

    std::string err;
    if (m_session == SessionType::Wayland)
    {
        std::vector<uint8_t> png;
        png.reserve(static_cast<size_t>(cap.w) * cap.h * 4);

        svpng(&png, cap.w, cap.h, cap.view().data(), 1);

        // Use foreground mode so the process doesn't fork away from our pipes.
        TinyProcessLib::Process proc(
            { "wl-copy", "--foreground", "--type", "image/png" }, "", nullptr, [&](const char* b, size_t n) {
                err.append(b, n);
            });

        if (!proc.write(reinterpret_cast<const char*>(png.data()), png.size()))
            return Err("Failed to write image to stdin");

        proc.close_stdin();
        if (proc.get_exit_status() == 0)
            return Ok();
        return Err("Failed copy image into clipboard");
    }

    clip::image_spec spec;
    spec.width          = cap.w;
    spec.height         = cap.h;
    spec.bits_per_pixel = 32;
    spec.bytes_per_row  = cap.w * 4;

    spec.red_mask    = 0xff;
    spec.green_mask  = 0xff00;
    spec.blue_mask   = 0xff0000;
    spec.alpha_mask  = 0xff000000;
    spec.red_shift   = 0;
    spec.green_shift = 8;
    spec.blue_shift  = 16;
    spec.alpha_shift = 24;

    clip::image img(cap.view().data(), spec);
    if (clip::set_image(img))
        return Ok();
    return Err("Failed copy image into clipboard");
}
