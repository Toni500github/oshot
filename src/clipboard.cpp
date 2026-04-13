#include "clipboard.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "clip/clip.h"
#include "socket.hpp"

#ifdef __linux__
#  include <sys/wait.h>
#  include <unistd.h>
#endif

// Starts wlcopy in the background, forgetting it.
// Sets wlcopy_pid, and returns an stdin pipe on success.
Result<int> start_wlcopy(const std::string_view mime_type = "text/plain;charset=utf-8")
{
#ifdef __linux__
    static int wlcopy_pid = -1;

    // stop if already launched
    if (wlcopy_pid > 0)
    {
        kill(wlcopy_pid, SIGINT);

        // we need to do this on Linux, because the process will be defunct otherwise.
        waitpid(wlcopy_pid, NULL, 0);

        wlcopy_pid = -1;
    }

    int copy_pipe[2];
    if (pipe(copy_pipe) == -1)
        return Err("Failed to open stdin pipe: " + std::string(strerror(errno)));

    wlcopy_pid = fork();

    if (wlcopy_pid == 0)
    {
        close(copy_pipe[1]);
        dup2(copy_pipe[0], STDIN_FILENO);
        close(copy_pipe[0]);

        const char* args[] = { "wl-copy", "--foreground", "--type", mime_type.data(), nullptr };
        execvp("wl-copy", const_cast<char* const*>(args));

        exit(-1);
    }

    close(copy_pipe[0]);

    if (wlcopy_pid < 0)
    {
        close(copy_pipe[1]);
        return Err("Failed to fork: " + std::string(strerror(errno)));
    }

    return Ok(copy_pipe[1]);
#else
    return Err("wl-copy scheme is not supported on non-linux systems!");
#endif
}

Result<> Clipboard::CopyText(const std::string& text)
{
    if (m_session == SessionType::Wayland)
    {
        const Result<int>& res = start_wlcopy();
        if (!res.ok())
            return res.error();

        const int fd = res.get();

        if (write(fd, text.c_str(), text.size()) == -1)
        {
            close(fd);
            return Err("Failed to copy text: " + std::string(strerror(errno)));
        }

        close(fd);
        return Ok();
    }

    // Linux only, external client with systray already running
    if (!g_is_systray && !OSHOT_TOOL_ON_MAIN_THREAD && g_sender->IsConnected())
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

Result<> Clipboard::CopyImage(const capture_result_t& cap)
{
    if (cap.w <= 0 || cap.h <= 0)
        return Err("Image size is empty");

    if (m_session == SessionType::Wayland)
    {
        const Result<int>& res = start_wlcopy("image/png");
        if (!res.ok())
            return res.error();

        const std::vector<uint8_t>& png = encode_to_png(cap);

        const int fd = res.get();
        if (write(fd, reinterpret_cast<const char*>(png.data()), png.size()) == -1)
        {
            close(fd);
            return Err("Failed to write image to stdin: " + std::string(strerror(errno)));
        }

        close(fd);

        return Ok();
    }

    // Linux only, external client with systray already running
    if (!g_is_systray && !OSHOT_TOOL_ON_MAIN_THREAD && g_sender->IsConnected())
    {
        const size_t         size = static_cast<size_t>(cap.w) * cap.h * 4;
        std::vector<uint8_t> payload;
        payload.resize(8 + size);

        std::memcpy(payload.data() + 0, &cap.w, 4);
        std::memcpy(payload.data() + 4, &cap.h, 4);
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
