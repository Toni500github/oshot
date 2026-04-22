#include "clipboard.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "clip/clip.h"
#include "screen_capture.hpp"

#ifdef __linux__
#  include <sys/wait.h>
#  include <unistd.h>
#endif

// Starts wlcopy/xclip in the background, forgetting it.
// Sets wlcopy_pid/xclip_pid, and returns an stdin pipe on success.
Result<int> start_linux_copy(SessionType session, const std::string_view mime_type = "text/plain;charset=utf-8")
{
#ifdef __linux__
    static pid_t clip_pid = -1;

    // stop if already launched wlcopy
    if (clip_pid > 0)
    {
        kill(clip_pid, SIGINT);

        // we need to do this on Linux, because the process will be defunct otherwise.
        waitpid(clip_pid, NULL, 0);

        clip_pid = -1;
    }

    int copy_pipe[2];
    if (pipe(copy_pipe) == -1)
        return Err("Failed to open stdin pipe: " + std::string(strerror(errno)));

    clip_pid = fork();

    if (clip_pid == 0)
    {
        close(copy_pipe[1]);
        dup2(copy_pipe[0], STDIN_FILENO);
        close(copy_pipe[0]);

        if (session == SessionType::Wayland)
        {
            const char* args[] = { "wl-copy", "--foreground", "--type", mime_type.data(), nullptr };
            execvp("wl-copy", const_cast<char* const*>(args));
        }
        else
        {
            const char* args[] = { "xclip", "-selection", "clipboard", "-t", mime_type.data(), "-i", nullptr };
            execvp("xclip", const_cast<char* const*>(args));
        }

        exit(-1);
    }

    close(copy_pipe[0]);

    if (clip_pid < 0)
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
    if (m_session == SessionType::Wayland || m_session == SessionType::X11)
    {
        const Result<int>& res = start_linux_copy(m_session);
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

    if (clip::set_text(text))
        return Ok();
    return Err("Failed to copy text into clipboard");
}

Result<> Clipboard::CopyImage(const capture_result_t& cap)
{
    if (cap.w <= 0 || cap.h <= 0)
        return Err("Image size is empty");

    if (m_session == SessionType::Wayland || m_session == SessionType::X11)
    {
        const Result<int>& res = start_linux_copy(m_session, "image/png");
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
