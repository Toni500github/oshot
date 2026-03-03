#include "clipboard.hpp"

#ifndef _WIN32
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "clip/clip.h"
#include "socket.hpp"

#define SVPNG_LINKAGE inline
#define SVPNG_OUTPUT  std::vector<uint8_t>* output
#define SVPNG_PUT(u)  output->push_back(static_cast<uint8_t>(u))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#include "svpng.h"
#pragma GCC diagnostic pop

// used to track the wl-copy process
static int wlcopy_pid = -1;

// Starts wlcopy in the background, forgetting it.
// Sets wlcopy_pid, and returns an stdin pipe on success.
Result<int> Start_wlcopy(const std::string& mime_type = "text/plain;charset=utf-8")
{
    // stop if already launched
    if (wlcopy_pid > 0)
    {
        kill(wlcopy_pid, SIGINT);

#ifndef _WIN32
        // we need to do this on Linux, because the process will be defunct otherwise.
        waitpid(wlcopy_pid, NULL, 0);
#endif

        wlcopy_pid = -1;
    }

    int copy_pipe[2];
    if (pipe(copy_pipe) == -1)
    {
        return Err("Failed to open stdin pipe: " + std::string(strerror(errno)));
    }

    wlcopy_pid = fork();

    if (wlcopy_pid == 0)
    {
        close(copy_pipe[1]);
        dup2(copy_pipe[0], STDIN_FILENO);
        close(copy_pipe[0]);
        execlp("wl-copy", "wl-copy", "--foreground", "--type", mime_type.c_str(), NULL);

        exit(-1);
    }

    close(copy_pipe[0]);

    if (wlcopy_pid < 0)
    {
        close(copy_pipe[1]);
        return Err("Failed to fork: " + std::string(strerror(errno)));
    }

    return Ok(copy_pipe[1]);
}

Result<> Clipboard::CopyText(const std::string& text)
{
    // Fuck you, fuck your software monopoly
    // and fuck your stupid standards that nobody wants to follow
    if (m_session != SessionType::Wayland)
    {
        if (!g_is_systray)
        {
            const Result<>& res = g_sender->Send(text);
            if (res.ok())
                return Ok();
            return Err("Failed to send text to copy: " + res.error_v());
        }

        if (clip::set_text(text))
            return Ok();
        return Err("Failed to copy text into clipboard");
    }

    Result<int> res = Start_wlcopy();
    if (!res.ok())
    {
        return res.error();
    }

    int fd = res.get();

    if (write(fd, text.c_str(), text.size()) == -1)
    {
        return Err("Failed to copy text: " + std::string(strerror(errno)));
    }

    close(fd);

    return Ok();
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

        Result<int> res = Start_wlcopy("image/png");

        if (!res.ok())
        {
            return res.error();
        }

        int fd = res.get();

        if (write(fd, reinterpret_cast<const char*>(png.data()), png.size()) == -1)
            return Err("Failed to write image to stdin: " + std::string(strerror(errno)));

        close(fd);

        return Ok();
    }

    if (!g_is_systray)
    {
        const uint32_t w_be = htonl(static_cast<uint32_t>(cap.w));
        const uint32_t h_be = htonl(static_cast<uint32_t>(cap.h));

        const size_t         size = static_cast<size_t>(cap.w) * cap.h * 4;
        std::vector<uint8_t> payload;
        payload.resize(8 + size);

        std::memcpy(payload.data() + 0, &w_be, 4);
        std::memcpy(payload.data() + 4, &h_be, 4);
        std::memcpy(payload.data() + 8, cap.view().data(), size);

        const Result<>& res = g_sender->Send(SendMsg::Image, payload.data(), payload.size());
        if (res.ok())
            return Ok();
        return Err("Failed to send image to copy: " + res.error_v());
    }

    clip::image_spec spec;
    spec.width          = cap.w;
    spec.height         = cap.h;
    spec.bits_per_pixel = 32;
    spec.bytes_per_row  = cap.w * 4;

    spec.red_mask    = 0x000000ff;
    spec.green_mask  = 0x0000ff00;
    spec.blue_mask   = 0x00ff0000;
    spec.alpha_mask  = 0xff000000;
    spec.red_shift   = 0;
    spec.green_shift = 8;
    spec.blue_shift  = 16;
    spec.alpha_shift = 24;

    clip::image img(cap.view().data(), spec);
    if (clip::set_image(img))
        return Ok();
    return Err("Failed to copy image into clipboard");
}
