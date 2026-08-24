#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include <deque>

#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

using namespace std::chrono_literals;

static std::deque<monitor_t> monitors;

static zxdg_output_manager_v1* s_xdg_output_manager = nullptr;

static void xdg_output_logical_position(void* data, zxdg_output_v1*, int32_t x, int32_t y)
{
    monitor_t* m = reinterpret_cast<monitor_t*>(data);
    m->geo.x     = x;
    m->geo.y     = y;
}

static void xdg_output_logical_size(void* data, zxdg_output_v1*, int32_t width, int32_t height)
{
    monitor_t* m  = reinterpret_cast<monitor_t*>(data);
    m->geo.width  = width;
    m->geo.height = height;
}

static void xdg_output_done(void*, zxdg_output_v1*)
{}
static void xdg_output_name(void*, zxdg_output_v1*, const char*)
{}
static void xdg_output_description(void*, zxdg_output_v1*, const char*)
{}

static const zxdg_output_v1_listener xdg_output_listener = { xdg_output_logical_position,
                                                             xdg_output_logical_size,
                                                             xdg_output_done,
                                                             xdg_output_name,
                                                             xdg_output_description };

static void output_geometry(void*       data,
                            wl_output*  wl_output,
                            int32_t     x,
                            int32_t     y,
                            int32_t     pw,
                            int32_t     ph,
                            int32_t     subpixel,
                            const char* make,
                            const char* model,
                            int32_t     transform)
{
    monitor_t* m = reinterpret_cast<monitor_t*>(data);
    m->geo.x     = x;
    m->geo.y     = y;
    m->transform = transform;
}

static void output_mode(void*      data,
                        wl_output* wl_output,
                        uint32_t   flags,
                        int32_t    width,
                        int32_t    height,
                        int32_t    refresh)
{
    if (flags & WL_OUTPUT_MODE_CURRENT)
    {
        monitor_t* m  = reinterpret_cast<monitor_t*>(data);
        m->geo.width  = width;
        m->geo.height = height;
    }
}

static void output_done(void* data, wl_output* wl_output)
{
    reinterpret_cast<monitor_t*>(data)->done = true;
}

static void output_scale(void* data, wl_output* wl_output, int32_t factor)
{}

static void output_name(void* data, wl_output* wl_output, const char* name)
{
    char* monitor_name = reinterpret_cast<monitor_t*>(data)->name;
    strncpy(monitor_name, name, 63);
}

static void output_description(void* data, wl_output* wl_output, const char* d)
{}

static const wl_output_listener output_listener = { output_geometry, output_mode, output_done,
                                                    output_scale,    output_name, output_description };

static void registry_global(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (strcmp(interface, wl_output_interface.name) == 0)
    {
        wl_output* output = reinterpret_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4));

        monitor_t& m = monitors.emplace_back();
        m.handle     = output;
        wl_output_add_listener(output, &output_listener, &m);
    }
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    {
        s_xdg_output_manager = reinterpret_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 3));
    }
}

static void registry_global_remove(void* data, wl_registry* registry, uint32_t name)
{}

static const wl_registry_listener registry_listener = { registry_global, registry_global_remove };

std::deque<monitor_t> wl_get_monitors()
{
    wl_display* display = wl_display_connect(nullptr);
    if (!display)
    {
        spdlog::error("No wayland display");
        return {};
    }

    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(display);  // discover wl_outputs + zxdg_output_manager_v1, flush base geometry/mode

    if (s_xdg_output_manager)
    {
        for (monitor_t& m : monitors)
        {
            zxdg_output_v1* xdg_output =
                zxdg_output_manager_v1_get_xdg_output(s_xdg_output_manager, reinterpret_cast<wl_output*>(m.handle));
            zxdg_output_v1_add_listener(xdg_output, &xdg_output_listener, &m);
        }
    }
    else
    {
        spdlog::warn("Compositor lacks xdg-output; rotated/scaled monitor geometry may be wrong");
    }

    wl_display_roundtrip(display);  // flush logical_position/logical_size, overwrites raw mode values

    wl_display_disconnect(display);
    return monitors;
}

#pragma GCC diagnostic pop
