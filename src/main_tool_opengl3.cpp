#ifndef __APPLE__
#  include "config.hpp"
#  include "imgui/imgui.h"
#  include "imgui/imgui_impl_glfw.h"
#  include "imgui/imgui_impl_opengl3.h"
#  include "screen_capture.hpp"
#  include "screenshot_tool.hpp"
#  include "util.hpp"
#  define GL_SILENCE_DEPRECATION
#  if defined(IMGUI_IMPL_OPENGL_ES2)
#    include <GLES2/gl2.h>
#  endif
#  include <GLFW/glfw3.h>  // Will drag system OpenGL headers

#  if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#  elif defined(__linux__)
#    include <X11/Xlib.h>
#  endif

GLFWwindow* window = nullptr;

void glfw_error_callback(int i_error, const char* description);
void glfw_drop_callback(GLFWwindow*, int count, const char** paths);
void extern_glfwTerminate()
{
    glfwTerminate();
}
void extern_glfwSwapInterval(int v)
{
    glfwSwapInterval(v);
}

// Returns the GLFW monitor that currently contains the cursor.
// Falls back to the primary monitor if the cursor position cannot be
// determined (e.g. on a pure Wayland session without XWayland).
static GLFWmonitor* get_monitor_at_cursor()
{
    int  cursor_x = 0, cursor_y = 0;
    bool cursor_ok = false;

#  if defined(_WIN32)
    POINT pt{};
    if (GetCursorPos(&pt))
    {
        cursor_x  = int(pt.x);
        cursor_y  = int(pt.y);
        cursor_ok = true;
    }
#  elif defined(__linux__)
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

void minimize_window()
{
    glfwIconifyWindow(window);
    glfwPollEvents();  // flush
}

void maximize_window()
{
    glfwRestoreWindow(window);
    glfwFocusWindow(window);
}

int run_main_tool(const std::string& imgui_ini_path)
{
    // Setup Screenshot Tool
    // Calling it before starting the window so that
    // we can capture at the exact moment we launch
    ScreenshotTool ss_tool;
    ss_tool.SetOnCancel([&]() {
        fmt::println(stderr, "Cancelled screenshot");
        glfwSwapInterval(0);  // Disable vsync
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });
    ss_tool.SetOnComplete([&](SavingOp op, const Result<capture_result_t>& result) {
        if (!result.ok())
        {
            error("Screenshot failed: {}", result.error());
            glfwSwapInterval(0);  // Disable vsync
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            return;
        }

        const Result<>& res = save_png(op, result.get());
        if (!res.ok())
            error("Failed to save as PNG: {}", res.error());

        glfwSwapInterval(0);  // Disable vsync
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });

    {
        const Result<>& res = ss_tool.Start();
        if (!res.ok())
        {
            error("Failed to start capture: {}", res.error());
            return EXIT_FAILURE;
        }
    }

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
#  elif defined(__APPLE__)
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
    // this because on windows it hanged in gdb and everytime had to restart the VM
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // Borderless
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);    // Always on top
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
#  endif

    GLFWmonitor*       monitor = get_monitor_at_cursor();
    const GLFWvidmode* mode    = glfwGetVideoMode(monitor);

    // Pass nullptr as the monitor to get a borderless windowed overlay
    // instead of exclusive fullscreen. This ensures GLFW_DECORATED,
    // GLFW_FLOATING, and GLFW_AUTO_ICONIFY hints actually take effect
    // (they are silently ignored for exclusive fullscreen windows), and
    // keeps mouse/keyboard events scoped to this monitor so they don't
    // bleed in from the other display on multi-monitor setups.
    window = glfwCreateWindow(
        mode->width, mode->height, "oshot", g_config->File.real_full_screen ? monitor : nullptr, nullptr);
    if (!window)
    {
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
    ImGui::StyleColorsDark();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = imgui_ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    ImFontConfig font_cfg;

    for (const std::string& font : g_config->File.fonts)
    {
        const fs::path& path = get_font_path(font);
        if (path.empty())
        {
            io.Fonts->AddFontDefault(&font_cfg);
        }
        else if (fs::exists(path))
        {
            io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f, &font_cfg);
        }
        else
        {
            if (!font.empty())
                warn("Font '{}' is not found", font);
            continue;
        }

        // this value is false by default, and we can't set it to true without adding atleast one font first.
        // so, after we add the first font, this will be true (and will stay true).
        // MergeMode fills the gap in previous fonts with glyphs from this font, for example, adding Arabic glyphs to a non-Arabic font.
        font_cfg.MergeMode = true;
    }

    apply_imgui_theme();

    // Start the overlay window
    {
        const Result<>& res = ss_tool.StartWindow();
        if (!res.ok())
        {
            error("Failed to start tool window: {}", res.error());
            if (!g_is_systray)
                glfwTerminate();
            return EXIT_FAILURE;
        }
    }

    while (!glfwWindowShouldClose(window) && ss_tool.IsActive())
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

        ss_tool.RenderOverlay();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);  // dark background
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
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
