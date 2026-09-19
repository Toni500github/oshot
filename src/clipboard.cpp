/*
 * Copyright 2026 Toni500
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "clipboard.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "clip/clip.h"
#include "config.hpp"
#include "screen_capture.hpp"

#if OSHOT_LINUX
#  include <sys/wait.h>
#  include <unistd.h>

#  include <csignal>
#endif

// Starts wlcopy/xclip in the background, forgetting it.
// Sets wlcopy_pid/xclip_pid, and returns an stdin pipe on success.
Result<int> start_linux_copy(SessionType session, const std::string_view mime_type)
{
#if OSHOT_LINUX
    static pid_t clip_pid = -1;

    const char* tool = (session == SessionType::Wayland) ? "wl-copy" : "xclip";
    if (which(tool) == UNKNOWN)
        return Err("{} not found in PATH, please install it to enable clipboard support", tool);

    signal(SIGPIPE, SIG_IGN);

    // stop if already launched wlcopy
    if (clip_pid > 0)
    {
        kill(clip_pid, SIGINT);

        // we need to do this on Linux, because the process will be defunct otherwise.
        waitpid(clip_pid, nullptr, 0);

        clip_pid = -1;
    }

    std::array<int, 2> copy_pipe;
    if (pipe(copy_pipe.data()) == -1)
        return Err("Failed to open stdin pipe: {}", strerror(errno));

    clip_pid = fork();

    if (clip_pid == 0)
    {
        close(copy_pipe[1]);
        dup2(copy_pipe[0], STDIN_FILENO);
        close(copy_pipe[0]);

        if (session == SessionType::Wayland)
        {
            const char* args[] = { tool, "--foreground", "--type", mime_type.data(), nullptr };
            execvp(tool, const_cast<char* const*>(args));
        }
        else
        {
            const char* args[] = { tool, "-selection", "clipboard", "-t", mime_type.data(), "-i", nullptr };
            execvp(tool, const_cast<char* const*>(args));
        }

        exit(-1);
    }

    close(copy_pipe[0]);

    if (clip_pid < 0)
    {
        close(copy_pipe[1]);
        return Err("Failed to fork: {}", strerror(errno));
    }

    return Ok(copy_pipe[1]);
#else
    return Err("wl-copy/xclip scheme is not supported on non-linux systems!");
#endif
}

Result<> Clipboard::CopyText(const std::string& text)
{
    if (m_session == SessionType::Wayland || m_session == SessionType::X11)
    {
        const char*        mime = (m_session == SessionType::X11) ? "UTF8_STRING" : "text/plain;charset=utf-8";
        const Result<int>& res  = start_linux_copy(m_session, mime);
        TRY(res);

        const int fd = res.get();

        if (write(fd, text.c_str(), text.size()) == -1)
        {
            close(fd);
            if (errno == EPIPE)
                return Err("Failed to copy text: clipboard tool not found or exited unexpectedly");
            return Err("Failed to copy text: {}", strerror(errno));
        }

        close(fd);
        return Ok();
    }

    if (clip::set_text(text))
        return Ok();
    return Err("Failed to copy text into clipboard");
}

Result<> Clipboard::CopyImage(const capture_result_t& cap, ImageExt ext)
{
    if (cap.w <= 0 || cap.h <= 0)
        return Err("Image size is empty");

    if (m_session == SessionType::Wayland || m_session == SessionType::X11)
    {
        const Result<int>& res =
            start_linux_copy(m_session, "image/" + str_tolower(g_config->File.image_out_type.first));
        TRY(res);

        const std::vector<uint8_t>& png = encode_to_image(cap, ext);

        const int fd = res.get();
        if (write(fd, reinterpret_cast<const char*>(png.data()), png.size()) == -1)
        {
            close(fd);
            if (errno == EPIPE)
                return Err("Failed to copy image: clipboard tool not found or exited unexpectedly");
            return Err("Failed to write image to stdin: {}", strerror(errno));
        }

        close(fd);

        return Ok();
    }

    clip::image_spec spec;
    spec.width          = cap.w;
    spec.height         = cap.h;
    spec.bits_per_pixel = 32;
    spec.bytes_per_row  = size_t(cap.w) * 4;

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
