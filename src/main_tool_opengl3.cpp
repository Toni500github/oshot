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

#include "platform.hpp"

#if !OSHOT_MACOS
#  include "config.hpp"
#  include "imgui/imgui.h"
#  include "imgui/imgui_impl_glfw.h"
#  include "imgui/imgui_impl_opengl3.h"
#  include "screen_capture.hpp"
#  include "screenshot_tool.hpp"
#  include "util.hpp"
#  if defined(IMGUI_IMPL_OPENGL_ES2)
#    include <GLES2/gl2.h>
#  endif

// clang-format off
#  include <GL/gl.h>
#  include <GLFW/glfw3.h>  // Will drag system OpenGL headers
// clang-format on

#  if OSHOT_WINDOWS
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#  elif OSHOT_FREEDESKTOP
#    include <X11/Xlib.h>
#  endif

#  include <algorithm>
#  include <thread>

using namespace std::chrono_literals;

GLFWwindow* window = nullptr;

void apply_imgui_theme();
void glfw_error_callback(int i_error, const char* description);
void glfw_drop_callback(GLFWwindow*, int count, const char** paths);

// The GLFW monitor under the cursor, or the primary one if the cursor can't
// be found (e.g. on Wayland without XWayland)
static GLFWmonitor* get_monitor_at_cursor()
{
    int  cursor_x = 0, cursor_y = 0;
    bool cursor_ok = false;

#  if OSHOT_WINDOWS
    POINT pt{};
    if (GetCursorPos(&pt))
    {
        cursor_x  = int(pt.x);
        cursor_y  = int(pt.y);
        cursor_ok = true;
    }
#  elif OSHOT_FREEDESKTOP
    // X11 or XWayland
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy)
    {
        Window   root  = DefaultRootWindow(dpy);
        Window   child = None;
        int      win_x = 0, win_y = 0;
        unsigned mask = 0;
        if (XQueryPointer(dpy, root, &root, &child, &cursor_x, &cursor_y, &win_x, &win_y, &mask))
            cursor_ok = true;
        XCloseDisplay(dpy);
    }
#  endif

    if (!cursor_ok)
        return glfwGetPrimaryMonitor();

    int           monitor_count = 0;
    GLFWmonitor** monitors      = glfwGetMonitors(&monitor_count);

    for (int i = 0; i < monitor_count; ++i)
    {
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);

        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (!mode)
            continue;

        // Found focused monitor
        if (cursor_x >= mx && cursor_x < mx + mode->width && cursor_y >= my && cursor_y < my + mode->height)
            return monitors[i];
    }

    return glfwGetPrimaryMonitor();
}

// The thread running the frame loop, the only one that may touch the window
static std::thread::id window_thread;

static void minimize_window_()
{
    glfwIconifyWindow(window);
    glfwPollEvents();  // flush
}

static bool on_wayland()
{
#  if OSHOT_FREEDESKTOP && defined(GLFW_PLATFORM_WAYLAND)  // GLFW 3.4+
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#  else
    return false;
#  endif
}

// The monitor the overlay was fullscreen on before hide_window_() unmapped it
static GLFWmonitor* hidden_monitor = nullptr;

static bool hide_window_()
{
    if (!window || std::this_thread::get_id() != window_thread || !glfwGetWindowAttrib(window, GLFW_VISIBLE) ||
        glfwGetWindowAttrib(window, GLFW_ICONIFIED))
        return false;

    // Wayland compositors may ignore minimize requests (sway does), which would
    // leave the dialog behind the fullscreen overlay: unmap it instead.
    // glfwHideWindow() and glfwShowWindow() ignore fullscreen windows
    if (on_wayland())
    {
        hidden_monitor = glfwGetWindowMonitor(window);
        if (hidden_monitor)
            glfwSetWindowMonitor(window, nullptr, 0, 0, g_scr_w, g_scr_h, GLFW_DONT_CARE);
        glfwHideWindow(window);
    }
    else
    {
        glfwIconifyWindow(window);
    }
    glfwPollEvents();  // flush
    return true;
}

static void show_window_()
{
    if (on_wayland())
    {
        glfwShowWindow(window);
        if (hidden_monitor)
            glfwSetWindowMonitor(window, hidden_monitor, 0, 0, g_scr_w, g_scr_h, GLFW_DONT_CARE);
        hidden_monitor = nullptr;
    }
    else
    {
        glfwRestoreWindow(window);
    }
    glfwFocusWindow(window);
}

static void* dialog_parent_()
{
#  if OSHOT_WINDOWS
    if (window && std::this_thread::get_id() == window_thread)
        return glfwGetWin32Window(window);
#  endif
    return nullptr;
}

bool app_quitting();  // main.cpp

int run_main_tool()
{
    int display_w, display_h;

    window_thread = std::this_thread::get_id();
    register_window_callbacks(
        minimize_window_, hide_window_, show_window_, dialog_parent_, glfwTerminate, glfwSwapInterval);

    // Before the window exists, so the screenshot is taken right away and without the overlay
    g_ss_tool.SetOnCancel([&]() {
        spdlog::info("Canceled screenshot");
        glfwSwapInterval(0);  // Disable vsync
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });
    g_ss_tool.SetReadPixels([&](const region_t& region) {
        // region is in screenshot pixels, the origin in screen units: on a scaled
        // output the framebuffer has GetImageScale() pixels per screen unit
        const point_t o = g_ss_tool.GetImageOrigin();
        const float   k = g_ss_tool.GetImageScale();

        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        const int rx = region.x + int(std::lround(o.x * k));
        const int ry = fb_h - (region.y + int(std::lround(o.y * k))) - region.h;

        const size_t               row = size_t(region.w) * 4;
        std::vector<unsigned char> pixels(row * region.h);
        glReadPixels(rx, ry, region.w, region.h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // glReadPixels() returns the rows bottom to top. Flip them here rather than
        // with stbi_flip_vertically_on_write(): the clipboard on Windows takes the
        // raw pixels and never goes through stb
        for (int y = 0; y < region.h / 2; ++y)
        {
            unsigned char* top = pixels.data() + (size_t(y) * row);
            std::swap_ranges(top, top + row, pixels.data() + (size_t(region.h - 1 - y) * row));
        }

        return capture_result_t{ .data = std::move(pixels), .w = region.w, .h = region.h };
    });
    g_ss_tool.SetOnComplete([&](SavingOp op, const capture_result_t& img, ImageExt ext) {
        MUST_OK(save_image(op, img, ext), {
            // The overlay closes anyway: hide it now so the error isn't opened behind it.
            // On Windows the overlay owns the dialog instead, see create_dialog()
            if constexpr (!OSHOT_WINDOWS)
                hide_window_();
            error("Failed to save as {}: {}", g_config->File.image_out_type.first, _r.error_v());
        });

        glfwSwapInterval(0);  // Disable vsync
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });

    MUST_OK(g_ss_tool.Start(), {
        error("Failed to start capture: {}", _r.error_v());
        return EXIT_FAILURE;
    });

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return EXIT_FAILURE;

    // Decide GL+GLSL versions
#  if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100 (WebGL 1.0)
    const char* glsl_version = "#version 100";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#  elif defined(IMGUI_IMPL_OPENGL_ES3)
    // GL ES 3.0 + GLSL 300 es (WebGL 2.0)
    const char* glsl_version = "#version 300 es";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#  elif OSHOT_MACOS
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#  else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 330 core";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
#  endif

#  if !DEBUG
    // Don't make the window actually fullscreen if debug build
    // this because on Windows it hanged in gdb and every time I had to restart the VM
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // Borderless
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);    // Always on top
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
#  endif

    // On Wayland a window can't place or size itself, so a tiling compositor
    // would tile the overlay like any other window. Fullscreen is the only way
    // to cover the monitor there, and unlike X11 it never changes the video mode.
    bool fullscreen = g_config->File.real_full_screen;
#  if OSHOT_FREEDESKTOP && defined(GLFW_PLATFORM_WAYLAND)  // GLFW 3.4+
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
    {
        fullscreen = true;
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, "oshot");  // matches oshot.desktop, for window rules and icons
        // On a scaled output (e.g. 1.5x) the framebuffer has more pixels than the
        // window has units: draw the screenshot at its full resolution there
        g_ss_tool.SetFullResolutionImage(true);
    }
#  endif

    GLFWmonitor*       monitor = get_monitor_at_cursor();
    const GLFWvidmode* mode    = glfwGetVideoMode(monitor);

    // Only log GLFW errors while creating the window: a failed first try isn't an
    // error yet, and a real failure gets one dialog below instead of one per try
    GLFWerrorfun error_callback = glfwSetErrorCallback(
        [](int i_error, const char* description) { spdlog::warn("GLFW Error {}: {}", i_error, description); });

    // No monitor: a borderless window instead of exclusive fullscreen, which
    // ignores the window hints and gets input from the other monitors too
    window = glfwCreateWindow(mode->width, mode->height, "oshot", fullscreen ? monitor : nullptr, nullptr);
#  if !OSHOT_MACOS && !defined(IMGUI_IMPL_OPENGL_ES2) && !defined(IMGUI_IMPL_OPENGL_ES3)
    if (!window)
    {
        // Some drivers stop below 3.3, e.g. Mesa's on the Raspberry Pi 4 and 5 (3.1).
        // The ImGui renderer works with 3.0 and GLSL 130 too
        spdlog::warn("Couldn't create an OpenGL 3.3 context, trying OpenGL 3.0");
        glsl_version = "#version 130";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);  // profiles only exist from 3.2
        window = glfwCreateWindow(mode->width, mode->height, "oshot", fullscreen ? monitor : nullptr, nullptr);
    }
#  endif
    glfwSetErrorCallback(error_callback);
    if (!window)
    {
        const char* desc = nullptr;
        glfwGetError(&desc);
        error("Failed to create the overlay window: {}\noshot needs OpenGL 3.0 or newer.",
              desc ? desc : "unknown error");
        if (!g_is_systray)
            glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glfwSetDropCallback(window, glfw_drop_callback);
    glfwSwapInterval(1);  // Enable vsync

    g_scr_w = mode->width;
    g_scr_h = mode->height;

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    const std::string ini = g_config->GetConfigDirPath() + DIR_SEP_STR "imgui.ini";
    ImGuiIO&          io  = ImGui::GetIO();
    io.IniFilename        = ini.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    build_font_atlas(io);
    apply_imgui_theme();

    // Start the overlay window
    MUST_OK(g_ss_tool.StartWindow(), {
        error("Failed to start tool window: {}", _r.error_v());
        if (!g_is_systray)
            glfwTerminate();
        return EXIT_FAILURE;
    });

    bool clean_frame    = false;
    bool dialogs_inited = false;
    // app_quitting(): tray Quit or a signal while capturing
    while (!glfwWindowShouldClose(window) && g_ss_tool.IsActive() && !app_quitting())
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your
        // inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or
        // clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or
        // clear/overwrite your copy of the keyboard data. Generally you may always pass all inputs to dear imgui, and
        // hide them from your application based on those two flags.
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        g_ss_tool.RenderOverlay();

        // Rendering
        ImGui::Render();
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Save/copy or scan requested: render one more frame with only the screenshot
        // and the annotations, read it back with glReadPixels(), and never show it
        if (clean_frame)
        {
            clean_frame = false;
            g_ss_tool.OnCleanFrame();
            continue;
        }
        if (g_ss_tool.WantsCleanFrame())
        {
            clean_frame = true;
            continue;
        }

        glfwSwapBuffers(window);

        // Set up dialogs only once the overlay is on screen: on Linux this
        // starts GTK, which would otherwise delay the capture and the overlay
        // by tens of milliseconds. An error before this sets them up itself.
        if (!dialogs_inited)
        {
            init_dialogs();
            dialogs_inited = true;
        }
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    window = nullptr;

    if (!g_is_systray)
        glfwTerminate();

    return EXIT_SUCCESS;
}
#endif
