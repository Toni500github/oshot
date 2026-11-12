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

#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "backends.hpp"
#include "fmt/format.h"
#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "stb_image.h"
#include "tiny-process-library/process.hpp"
#include "util.hpp"

using namespace spdlog;

// ------------------------------
// KDE Plasma
// ------------------------------

// Says how to get KWin's permission, once per process (the tray would repeat it on every capture)
static void warn_kwin_not_authorized()
{
    static std::atomic_flag warned = ATOMIC_FLAG_INIT;
    if (warned.test_and_set())
        return;

    // The AppImage's executable is in a mount point that changes on every start, so no desktop
    // file can name it
    if (std::getenv("APPIMAGE"))
    {
        spdlog::warn(
            "Before Plasma 6.8 KWin only gives screenshots to installed programs, so the AppImage uses "
            "a slower capture (Spectacle or the portal)");
        return;
    }

    // nixpkgs' KWin maps a wrapped program (bin/.oshot-wrapped) back to its wrapper
    fs::path          exe  = get_exe_path();
    const std::string name = exe.filename().string();
    if (name.size() > 9 && name.front() == '.' && name.ends_with("-wrapped"))
        exe.replace_filename(name.substr(1, name.size() - 9));

    spdlog::warn(
        "Before Plasma 6.8 KWin only gives screenshots to programs with a desktop file, so oshot uses a "
        "slower capture (Spectacle or the portal). To fix it, install oshot (make install, a package), "
        "or save oshot.desktop from the source's assets/linux folder as "
        "~/.local/share/applications/oshot.desktop with its first Exec line changed to \"Exec={}\"",
        exe.string());
}

bool kde_on_wayland()
{
    const char* type    = std::getenv("XDG_SESSION_TYPE");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return !(type && strcmp(type, "x11") == 0) && wayland && wayland[0] != '\0';
}

Result<capture_result_t> capture_full_screen_kde()
{
    // XGetImage needs neither KWin's permission nor spectacle
    if (!kde_on_wayland())
        return capture_full_screen_x11();

    Result<capture_result_t> res = capture_full_screen_kwin();
    if (res.ok())
        return res;
    spdlog::warn("{}", res.error_v());
    if (res.error_v().find("NoAuthorized") != std::string::npos)
        warn_kwin_not_authorized();

    // KWin before Plasma 6.8 refuses executables without a matching desktop file (AppImage, zip,
    // build folder), but trusts spectacle's
    if (which("spectacle") != UNKNOWN)
        return capture_full_screen_spectacle();
    return capture_full_screen_wayland();
}

// QImage::Format values KWin has used for screenshots (Format_RGBX8888 since Plasma 6.8,
// Format_ARGB32(_Premultiplied) before)
// NOLINTNEXTLINE
enum : guint32
{
    QIMAGE_RGB32                  = 4,
    QIMAGE_ARGB32                 = 5,
    QIMAGE_ARGB32_PREMULTIPLIED   = 6,
    QIMAGE_RGBX8888               = 16,
    QIMAGE_RGBA8888               = 17,
    QIMAGE_RGBA8888_PREMULTIPLIED = 18,
};

// Reads exactly buf.size() bytes, or less if the writer closes the pipe early
static size_t read_full(const int fd, std::vector<uint8_t>& buf)
{
    size_t got = 0;
    while (got < buf.size())
    {
        const ssize_t n = read(fd, buf.data() + got, buf.size() - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += size_t(n);
    }
    return got;
}

Result<capture_result_t> capture_full_screen_kwin()
{
    GError*          error = nullptr;
    GDBusConnection* bus   = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus)
    {
        const std::string msg = error->message ? error->message : "Unknown";
        g_error_free(error);
        return Err("Failed to connect to session bus: {}", msg);
    }

    std::array<int, 2> fds{ -1, -1 };
    if (pipe2(fds.data(), O_CLOEXEC) != 0)
    {
        const int errnum = errno;
        g_object_unref(bus);
        return Err("pipe2() failed for the KWin screenshot: {}", strerror(errnum));
    }

    // The list keeps its own duplicate of the write end
    GUnixFDList* fd_list = g_unix_fd_list_new();
    const int    handle  = g_unix_fd_list_append(fd_list, fds[1], &error);
    close(fds[1]);
    if (handle < 0)
    {
        const std::string msg = error->message ? error->message : "Unknown";
        g_error_free(error);
        g_object_unref(fd_list);
        g_object_unref(bus);
        close(fds[0]);
        return Err("Failed to pass the pipe to KWin: {}", msg);
    }

    // The same options spectacle sends by default. KWin leaves out the caller's windows unless
    // hide-caller-windows is false, so an overlay from this process doesn't get in the shot.
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "native-resolution", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&opts, "{sv}", "include-cursor", g_variant_new_boolean(FALSE));

    // KWin sends the reply before it starts writing the pixels, so waiting for the reply
    // can't deadlock on a full pipe
    GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(bus,
                                                                    "org.kde.KWin.ScreenShot2",
                                                                    "/org/kde/KWin/ScreenShot2",
                                                                    "org.kde.KWin.ScreenShot2",
                                                                    "CaptureActiveScreen",
                                                                    g_variant_new("(a{sv}h)", &opts, handle),
                                                                    G_VARIANT_TYPE("(a{sv})"),
                                                                    G_DBUS_CALL_FLAGS_NONE,
                                                                    4000,  // what spectacle waits
                                                                    fd_list,
                                                                    nullptr,
                                                                    nullptr,
                                                                    &error);

    // Closes the last copy of the write end besides KWin's, or read() would never see EOF
    g_object_unref(fd_list);
    g_object_unref(bus);

    if (!reply)
    {
        close(fds[0]);
        // Error.NoAuthorized before Plasma 6.8, ServiceUnknown when the compositor isn't KWin
        // and UnknownMethod before Plasma 5.24
        char* remote = g_dbus_error_get_remote_error(error);
        g_dbus_error_strip_remote_error(error);
        const std::string msg = fmt::format("KWin screenshot failed ({}): {}",
                                            remote ? remote : "no D-Bus error name",
                                            error->message ? error->message : "Unknown");
        g_free(remote);
        g_error_free(error);
        return Err(msg);
    }

    GVariant*   results = g_variant_get_child_value(reply, 0);
    const char* type    = nullptr;
    const char* screen  = nullptr;
    guint32     w = 0, h = 0, stride = 0, format = 0;
    const bool have_all = g_variant_lookup(results, "type", "&s", &type) &&
                          g_variant_lookup(results, "width", "u", &w) && g_variant_lookup(results, "height", "u", &h) &&
                          g_variant_lookup(results, "stride", "u", &stride) &&
                          g_variant_lookup(results, "format", "u", &format);
    const bool is_raw   = have_all && strcmp(type, "raw") == 0;
    if (is_raw && g_variant_lookup(results, "screen", "&s", &screen))
        debug("KWin: captured {}x{} of screen {} (format {})", w, h, screen, format);
    g_variant_unref(results);
    g_variant_unref(reply);

    if (!is_raw || w == 0 || h == 0 || w > INT_MAX / 4 || h > INT_MAX || stride < w * 4)
    {
        close(fds[0]);
        return Err("Unexpected KWin screenshot metadata: {}x{}, stride {}, format {}", w, h, stride, format);
    }

    const bool native_argb = format == QIMAGE_RGB32 || format == QIMAGE_ARGB32 || format == QIMAGE_ARGB32_PREMULTIPLIED;

    if (!native_argb && format != QIMAGE_RGBX8888 && format != QIMAGE_RGBA8888 &&
        format != QIMAGE_RGBA8888_PREMULTIPLIED)
    {
        close(fds[0]);
        return Err("Unsupported KWin screenshot format {}", format);
    }

    std::vector<uint8_t> raw(size_t(stride) * h);
    const size_t         got = read_full(fds[0], raw);
    close(fds[0]);
    if (got != raw.size())
        return Err("The KWin screenshot pipe ended after {} of {} bytes", got, raw.size());

    // A screen is opaque, so alpha is 255 in every format (and premultiplying changes nothing)
    capture_result_t out{ .data = std::vector<uint8_t>(size_t(w) * h * 4), .w = int(w), .h = int(h) };
    for (guint32 y = 0; y < h; ++y)
    {
        const uint8_t* src = raw.data() + size_t(y) * stride;
        uint8_t*       dst = out.data.data() + size_t(y) * w * 4;
        for (guint32 x = 0; x < w; ++x, src += 4, dst += 4)
        {
            rgba_t c;
            if (native_argb)
            {
                // 0xAARRGGBB in the machine's byte order
                uint32_t px;
                std::memcpy(&px, src, sizeof(px));
                c = rgba_t::from_argb(px);
            }
            else
            {
                c = load_rgba(src);
            }
            c.a = 0xFF;
            store_rgba(dst, c);
        }
    }

    return Ok(std::move(out));
}

// Capture the monitor under the cursor with KDE's `spectacle -m` (--screen),
// which the portal doesn't guarantee when it runs non-interactively on
// several monitors
Result<capture_result_t> capture_full_screen_spectacle()
{
    capture_result_t result;

    const std::string tmppath = create_temp_png();
    if (tmppath.empty())
        return Err("Failed to create temp png");

    // -b  run in background (no GUI window)
    // -n  suppress the "screenshot saved" desktop notification
    // -m  capture the monitor containing the mouse pointer
    // -o  write to the given path instead of the default Pictures folder
    TinyProcessLib::Process proc({ "spectacle", "-b", "-n", "-m", "-o", tmppath }, "");

    const int exit_code = proc.get_exit_status();
    if (exit_code != 0)
    {
        unlink(tmppath.c_str());
        spdlog::warn("spectacle exited with code {}. Trying wayland capture...", exit_code);
        return capture_full_screen_wayland();
    }

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load(tmppath.c_str(), &w, &h, &comp, STBI_rgb_alpha);
    unlink(tmppath.c_str());

    if (!rgba)
        return Err("Failed to read PNG: {}", STBI_ERROR);

    result.w = w;
    result.h = h;
    result.data.assign(rgba, rgba + size_t(w) * h * 4);
    stbi_image_free(rgba);

    return Ok(std::move(result));
}
