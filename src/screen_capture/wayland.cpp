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

#include <gio/gio.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "backends.hpp"
#include "fmt/format.h"
#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "stb_image.h"
#include "tiny-process-library/process.hpp"
#include "util/image.hpp"
#include "util/result.hpp"

using namespace spdlog;

// ------------------------------
// Wayland and the xdg-desktop-portal
// ------------------------------

Result<capture_result_t> capture_full_screen_wayland()
{
    // grim reads the screen through the compositor's screencopy protocol (wlroots, Hyprland, niri),
    // in a third of the time the portal takes to save a PNG file for us to decode. GNOME has only the portal
    bool screencopy = false;
    if (wl_display* display = wl_display_connect(nullptr))
    {
        static constexpr wl_registry_listener listener = {
            .global =
                [](void* data, wl_registry*, uint32_t, const char* interface, uint32_t) {
                    if (strcmp(interface, "zwlr_screencopy_manager_v1") == 0 ||
                        strcmp(interface, "ext_image_copy_capture_manager_v1") == 0)
                        *static_cast<bool*>(data) = true;
                },
            .global_remove = [](void*, wl_registry*, uint32_t) {}
        };
        wl_registry* registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &listener, &screencopy);
        wl_display_roundtrip(display);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
    }

    if (!screencopy)
    {
        Result<capture_result_t> res = capture_full_screen_portal();
        if (res.ok())
            return res;
    }

    capture_result_t result;

    std::string             err;
    std::vector<uint8_t>    buf;
    TinyProcessLib::Process proc(
        { "grim", "-t", "ppm", "-" },
        "",  // cwd
        [&](const char* bytes, size_t n) {
            // stdout (binary)
            const auto* p = reinterpret_cast<const uint8_t*>(bytes);
            buf.insert(buf.end(), p, p + n);
        },
        [&](const char* p, size_t n) {
            // stderr
            err.append(p, n);
        });
    if (proc.get_exit_status() != 0)
    {
        // Not installed, or the compositor refused (e.g. Hyprland's permissions)
        if (screencopy)
        {
            spdlog::debug("Capture with grim failed: {}", err);
            return capture_full_screen_portal();
        }
        return Err("Capture with grim failed: {}", err);
    }

    // stbi_load_from_memory takes an int length
    if (buf.size() > INT_MAX)
        return Err("Screenshot too large to decode (buffer > INT_MAX).");

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load_from_memory(buf.data(), int(buf.size()), &w, &h, &comp, STBI_rgb_alpha);

    if (!rgba)
        return Err("Failed to read PPM data: {}", STBI_ERROR);

    result.w = w;
    result.h = h;
    result.data.assign(rgba, rgba + (size_t(w) * h * 4));
    stbi_image_free(rgba);

    return Ok(std::move(result));
}

struct portal_state_t
{
    GMainLoop* loop;
    guint      subscription_id;
    guint      timeout_id;  // tracked so we can cancel it before the stack frame is gone

    std::string      png_path;
    std::string      error_msg;
    capture_result_t cap;
};

static gboolean on_timeout(gpointer user_data)
{
    auto* st       = reinterpret_cast<portal_state_t*>(user_data);
    st->error_msg  = "Timed out waiting for portal response (is xdg-desktop-portal running?)";
    st->timeout_id = 0;  // GLib will remove the source; mark it gone so cleanup skips it
    g_main_loop_quit(st->loop);
    return G_SOURCE_REMOVE;
}

static Result<std::string> uri_to_path(const char* uri)
{
    if (!uri)
        return Err("URI path to screenshot is null");

    GFile* file = g_file_new_for_uri(uri);
    if (!file)
        return Err("g_file_new_for_uri returned null");

    char* path = g_file_get_path(file);
    g_object_unref(file);
    if (!path)
        return Err("URI is not a local file path (g_file_get_path returned nullptr)");

    std::string out(path);
    g_free(path);
    return Ok(std::move(out));
}

static void on_response(GDBusConnection* conn,
                        const gchar* /* sender_name */,
                        const gchar* /* object_path */,
                        const gchar* /* interface_name */,
                        const gchar* /* signal_name */,
                        GVariant* parameters,
                        gpointer  user_data)
{
    auto* st = reinterpret_cast<portal_state_t*>(user_data);

    guint32      response = 2;
    GVariant*    results  = nullptr;
    const gchar* uri      = nullptr;

    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    if (response != 0)
        st->error_msg = fmt::format("Canceled or failed (response={})", (unsigned)response);
    else if (!(g_variant_lookup(results, "uri", "&s", &uri) && uri))
        st->error_msg = "Screenshot portal succeeded but returned no file path";

    if (results)
        g_variant_unref(results);

    if (st->subscription_id)
    {
        g_dbus_connection_signal_unsubscribe(conn, st->subscription_id);
        st->subscription_id = 0;  // mark as removed so cleanup in capture_full_screen_portal skips it
    }

    const Result<std::string>& res = uri_to_path(uri);
    if (res.ok())
    {
        st->png_path  = res.get();
        st->error_msg = "";
    }
    else
    {
        st->png_path  = "";
        st->error_msg = res.error_v();
    }

    g_main_loop_quit(st->loop);
}

Result<capture_result_t> capture_full_screen_portal()
{
    portal_state_t st{};
    spdlog::warn("Fallback to portal capture");

    GError*          error = nullptr;
    GDBusConnection* bus   = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus)
    {
        st.error_msg = "Failed to connect to session bus: " + std::string(error->message ? error->message : "Unknown");
        g_error_free(error);
        return Err(st.error_msg);
    }

    // Call Screenshot portal synchronously so we can fail loudly if the service isn't there.
    // Screenshot returns a "request handle" object path (type 'o').
    GVariant* reply = g_dbus_connection_call_sync(bus,
                                                  "org.freedesktop.portal.Desktop",
                                                  "/org/freedesktop/portal/desktop",
                                                  "org.freedesktop.portal.Screenshot",
                                                  "Screenshot",
                                                  g_variant_new("(sa{sv})", "", nullptr),
                                                  G_VARIANT_TYPE("(o)"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  3000,  // 3s timeout for the method call itself
                                                  nullptr,
                                                  &error);

    if (!reply)
    {
        st.error_msg = "Portal call failed: " + std::string(error->message ? error->message : "Unknown");
        g_error_free(error);
        g_object_unref(bus);
        return Err(st.error_msg);
    }

    const char* request_path = nullptr;
    g_variant_get(reply, "(&o)", &request_path);

    if (!request_path || request_path[0] == '\0')
    {
        g_variant_unref(reply);
        g_object_unref(bus);
        return Err("Portal returned an empty request handle");
    }

    st.loop            = g_main_loop_new(nullptr, FALSE);
    st.subscription_id = 0;
    st.timeout_id      = 0;

    // The request object's Response signal
    st.subscription_id = g_dbus_connection_signal_subscribe(bus,
                                                            "org.freedesktop.portal.Desktop",
                                                            "org.freedesktop.portal.Request",
                                                            "Response",
                                                            request_path,
                                                            nullptr,
                                                            G_DBUS_SIGNAL_FLAGS_NONE,
                                                            on_response,
                                                            &st,
                                                            nullptr);

    // Give up if nothing responds. The source ID is kept to remove it if on_response fires
    // first, or the timeout would fire after this stack frame is gone and crash on
    // g_main_loop_quit with a dangling portal_state_t*
    st.timeout_id = g_timeout_add_seconds(5, on_timeout, &st);

    g_main_loop_run(st.loop);

    // Remove whichever of the timeout and the subscription didn't fire, before st goes
    // out of scope. on_timeout and on_response zero their own ID
    if (st.timeout_id)
        g_source_remove(st.timeout_id);
    if (st.subscription_id)
        g_dbus_connection_signal_unsubscribe(bus, st.subscription_id);

    g_variant_unref(reply);
    g_main_loop_unref(st.loop);
    g_object_unref(bus);

    if (st.png_path.empty())
    {
        if (st.error_msg.empty())
            return Err("Failed to retrieve URI path to screenshot PNG");
        return Err(st.error_msg);
    }

    int      w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load(st.png_path.c_str(), &w, &h, &comp, STBI_rgb_alpha);

    if (!rgba)
    {
        unlink(st.png_path.c_str());
        return Err("Failed to read PNG data: {}", STBI_ERROR);
    }

    st.cap.w = w;
    st.cap.h = h;
    st.cap.data.assign(rgba, rgba + (size_t(w) * h * 4));
    stbi_image_free(rgba);

    // The portal backend (on KDE mostly) writes a permanent file to ~/Pictures named "Screenshot_*.png".
    // Delete it now that we have the pixels in memory.
    unlink(st.png_path.c_str());

    // The portal always captures the full virtual desktop on multi-monitor
    // setups. Crop down to the monitor that contains the cursor.
    // XRandR works on native X11 and on KDE/GNOME Wayland via XWayland.
    std::optional<region_t> op_mon = get_cursor_monitor_xrandr(nullptr);
    if (op_mon && (st.cap.w > op_mon->w || st.cap.h > op_mon->h))
    {
        region_t m = *op_mon;
        debug("Portal: got monitor with xrandr at cursor position");
        debug("Portal: cropping {}x{} capture to monitor {}x{}+{}+{}", st.cap.w, st.cap.h, m.w, m.h, m.x, m.y);

        const int x0    = std::max(0, m.x);
        const int y0    = std::max(0, m.y);
        const int x1    = std::min(st.cap.w, m.x + m.w);
        const int y1    = std::min(st.cap.h, m.y + m.h);
        const int new_w = x1 - x0;
        const int new_h = y1 - y0;

        if (new_w > 0 && new_h > 0)
        {
            const int            src_stride = st.cap.w;
            std::vector<uint8_t> cropped(size_t(new_w) * new_h * 4);
            for (int row = 0; row < new_h; ++row)
            {
                const uint8_t* src = st.cap.data.data() + (size_t(y0 + row) * src_stride + x0) * 4;
                uint8_t*       dst = cropped.data() + size_t(row) * new_w * 4;
                std::memcpy(dst, src, size_t(new_w) * 4);
            }
            st.cap.data = std::move(cropped);
            st.cap.w    = new_w;
            st.cap.h    = new_h;
        }
    }

    return Ok(std::move(st.cap));
}
