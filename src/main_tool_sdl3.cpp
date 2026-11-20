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

#include <SDL3/SDL.h>

#include "app.hpp"
#include "config.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl3.h"
#include "imgui/imgui_impl_sdlrenderer3.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/spdlog.h"
#include "tray.hpp"
#include "util.hpp"
#include "util/dialogs.hpp"
#include "util/fonts.hpp"
#include "util/image.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
#include "util/window.hpp"

#if OSHOT_FREEDESKTOP
#  include <fontconfig/fontconfig.h>
#elif OSHOT_MACOS
#  include <objc/message.h>
#  include <objc/runtime.h>
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

void apply_imgui_theme();
void reload_theme_if_changed();

// The thread run_main_tool() runs on, the only one that may touch the window
static std::thread::id window_thread;

static bool on_overlay_thread()
{
    return g_ss_tool.GetWindow() && std::this_thread::get_id() == window_thread;
}

static bool on_wayland()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && strcmp(driver, "wayland") == 0;
}

static void minimize_window_()
{
    if (on_overlay_thread())
        SDL_MinimizeWindow(g_ss_tool.GetWindow());
}

static bool hide_window_()
{
    SDL_Window* window = g_ss_tool.GetWindow();
    if (!on_overlay_thread() || (SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)))
        return false;

    // Unmap it rather than minimize it, since SDL doesn't restore a minimized fullscreen window on Windows,
    // sway ignores minimize requests, and macOS's minimize animation never finishes under the modal loop
    SDL_HideWindow(window);
    SDL_PumpEvents();
    return true;
}

static void show_window_()
{
    if (!on_overlay_thread())
        return;

    SDL_Window* window = g_ss_tool.GetWindow();
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    g_ss_tool.KeepDrawing();  // repaint right away, the dialog's input wasn't ours
}

static void* dialog_parent_()
{
#if OSHOT_WINDOWS
    if (on_overlay_thread())
        return SDL_GetPointerProperty(
            SDL_GetWindowProperties(g_ss_tool.GetWindow()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    return nullptr;
}

int run_main_tool()
{
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool          close    = false;  // set once the overlay is canceled or done
    bool          copied   = false;  // the overlay closed after copying the image

    window_thread = std::this_thread::get_id();
    register_window_callbacks(minimize_window_, hide_window_, show_window_, dialog_parent_);

    g_ss_tool.SetOnCancel([&]() {
        spdlog::info("Canceled screenshot");
        SDL_SetRenderVSync(renderer, false);
        close = true;
    });
    g_ss_tool.SetReadPixels([&](const region_t& region) -> capture_result_t {
        // region is in screenshot pixels and the origin in screen units. On a scaled
        // output the render target has GetImageScale() pixels per screen unit
        const ImVec2   o = g_ss_tool.GetImageOrigin();
        const float    k = g_ss_tool.GetImageScale();
        const SDL_Rect rect{ .x = region.x + int(std::lround(o.x * k)),
                             .y = region.y + int(std::lround(o.y * k)),
                             .w = region.w,
                             .h = region.h };

        SDL_Surface* raw = SDL_RenderReadPixels(renderer, &rect);
        if (!raw)
        {
            spdlog::error("SDL_RenderReadPixels() failed: {}", SDL_GetError());
            return g_ss_tool.GetFinalImage();
        }

        // The target's format depends on the driver (ARGB8888 on most of them).
        // RGBA32 is RGBA in memory on any endianness
        SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (!rgba)
        {
            spdlog::error("SDL_ConvertSurface() failed: {}", SDL_GetError());
            return g_ss_tool.GetFinalImage();
        }

        // Rows can be padded (pitch), so copy them one by one
        const size_t               row = size_t(rgba->w) * 4;
        std::vector<unsigned char> pixels(row * rgba->h);
        for (int y = 0; y < rgba->h; ++y)
            std::memcpy(pixels.data() + (size_t(y) * row),
                        static_cast<const unsigned char*>(rgba->pixels) + (size_t(y) * rgba->pitch),
                        row);

        capture_result_t out{ .data = std::move(pixels), .w = rgba->w, .h = rgba->h };
        SDL_DestroySurface(rgba);
        return out;
    });
    g_ss_tool.SetOnComplete([&](SavingOp op, const capture_result_t& img, ImageExt ext) {
        const Result<bool>& saved = save_image(op, img, ext);
        if (!saved.ok())
        {
            // The overlay stays open, and dialog::create() keeps the error in front of it
            if (op == SavingOp::Clipboard)
                dialog::error("Failed to copy the image: {}", saved.error_v());
            else
                dialog::error("Failed to save as {}: {}", g_config->File.image_out_type.first, saved.error_v());
            return false;
        }
        if (!saved.get())  // the save dialog was canceled, so go back to the overlay
            return false;

        SDL_SetRenderVSync(renderer, false);
        close  = true;
        copied = op == SavingOp::Clipboard;
        return true;
    });

    // oshot's own signal handlers (main.cpp) also quit the tray
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // The Wayland app ID and X11 class, matching oshot.desktop for window rules and icons
    SDL_SetHint(SDL_HINT_APP_ID, "oshot");
    // Losing focus to a dialog or another monitor must not minimize the fullscreen overlay
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    // Without a tray, SDL_Init() starts the app on macOS. Keep it an LSUIElement one (no Dock
    // icon, no menu bar), and go fullscreen in place instead of in a new Space with an animation
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
#if OSHOT_FREEDESKTOP
    // SDL picks XWayland when the compositor lacks wp_fifo_v1 (GNOME before 48), where the overlay isn't
    // forced fullscreen and is blurry on scaled outputs. SDL_VIDEO_DRIVER still takes precedence
    if (std::getenv("WAYLAND_DISPLAY"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
    // The overlay has no decorations, and libdecor (used where the compositor doesn't draw them,
    // e.g. GNOME) would only slow SDL_Init() down loading its GTK plugin
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");
#endif

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        dialog::error("Failed to initialize SDL: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
#if OSHOT_MACOS
    // SDL_Init() finishing the app launch replaces the quit handler
    install_quit_handler(request_quit);

    // A bare executable (not oshot.app with its LSUIElement) starts with a prohibited activation policy
    // and never gets the keyboard, so make it an accessory app (no Dock icon or menu bar)
    {
        using send_id_t     = id (*)(id, SEL);
        using send_policy_t = BOOL (*)(id, SEL, long);
        const id app = reinterpret_cast<send_id_t>(objc_msgSend)(reinterpret_cast<id>(objc_getClass("NSApplication")),
                                                                 sel_registerName("sharedApplication"));
        constexpr long NSApplicationActivationPolicyAccessory = 1;
        reinterpret_cast<send_policy_t>(objc_msgSend)(
            app, sel_registerName("setActivationPolicy:"), NSApplicationActivationPolicyAccessory);
    }
#endif

#if OSHOT_MACOS
    // A destroyed window leaks its backing surfaces (16 MiB per capture), so the tray reuses one
    struct kept_window_t
    {
        SDL_Window*   window   = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_DisplayID display  = 0;
        SDL_Rect      bounds{};
    };
    static kept_window_t kept;
#endif

    // Undoes the setup so far, then shows the error
    const auto fail = [&](const dialog::located_fmt_t fmt, const std::string_view error) {
        g_ss_tool.SetWindow(nullptr);
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);  // before the dialog, which would try to hide it
#if OSHOT_MACOS
        kept = {};
#endif
        dialog::error(fmt, error);
        if (!g_is_systray)
            SDL_Quit();
        return EXIT_FAILURE;
    };

#if OSHOT_FREEDESKTOP
    // fontconfig's configuration is slow to load, so load it while the window is set up
    std::future<void> fontconfig = std::async(std::launch::async, [] { FcInit(); });
#endif

    // The screenshot has to be taken before the window shows up. Wayland shows a window only with its
    // first frame, so there the capture runs while the window and its renderer are set up
    std::future<Result<>> capture;
    if (on_wayland())
    {
        capture = std::async(std::launch::async, [] { return g_ss_tool.Start(); });
    }
    else
    {
        const Result<>& started = g_ss_tool.Start();
        if (!started.ok())
            return fail("Failed to start capture: {}", started.error_v());
    }

    // Window sizes are in screen units (points on macOS and scaled Wayland outputs)
    SDL_Rect            bounds{};
    const SDL_DisplayID cursor_display = get_display_at_cursor();
    if (!SDL_GetDisplayBounds(cursor_display, &bounds))
        return fail("Failed to get the display's size: {}", SDL_GetError());

    // ImGui needs no depth or stencil buffer, and SDL's default depth buffer is as big as the screen
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    // SDL's borderless fullscreen (no video mode change), since a monitor-sized borderless window lands below
    // xfwm4's panel, gets tiled, or can't be placed on Wayland. The fullscreen option turns it off on X11 and Windows
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if DEBUG
    // A plain window in debug builds on X11 and Windows, where a fullscreen one on top of
    // everything hung the debugger's session
    if (on_wayland() || OSHOT_MACOS)
        flags |= SDL_WINDOW_FULLSCREEN;
#else
    if (g_config->File.fullscreen || on_wayland() || OSHOT_MACOS)
        flags |= SDL_WINDOW_FULLSCREEN;
    flags |= SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP;
#endif

#if OSHOT_MACOS
    if (kept.window && (kept.display != cursor_display || !SDL_RectsEqual(&kept.bounds, &bounds)))
    {
        SDL_DestroyRenderer(kept.renderer);
        SDL_DestroyWindow(kept.window);
        kept = {};
    }
    if (kept.window)
    {
        window   = kept.window;
        renderer = kept.renderer;
        SDL_SetRenderVSync(renderer, true);  // closing the last overlay turned it off
    }
    else
#endif
    {
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "oshot");
        // With a position SDL asks Wayland for that output, which there is only the primary one.
        // Without one the compositor picks (the focused output), and the event loop crops to it
        if (!on_wayland())
        {
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, bounds.x);
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, bounds.y);
        }
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, bounds.w);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, bounds.h);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, Sint64(flags));
        window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        if (!window)
            return fail("Failed to create the overlay window: {}", SDL_GetError());

        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer)
            return fail("Failed to create the overlay's renderer: {}", SDL_GetError());
        SDL_SetRenderVSync(renderer, true);
        spdlog::debug("SDL video driver: {}, renderer: {}", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer));
#if OSHOT_MACOS
        if (g_is_systray)
            kept = { window, renderer, cursor_display, bounds };
#endif
    }

    if (capture.valid())
    {
        const Result<>& started = capture.get();
        if (!started.ok())
            return fail("Failed to start capture: {}", started.error_v());
    }

    g_ss_tool.SetWindow(window);
    const auto use_display = [](const SDL_DisplayID id, const bool keep_desktop) {
        if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(id))
        {
            g_scr_w = int(std::lround(float(m->w) * m->pixel_density));
            g_scr_h = int(std::lround(float(m->h) * m->pixel_density));
        }
        MUST_OK(g_ss_tool.CropAndUseDisplay(id, keep_desktop), spdlog::warn("{}", _r.error_v()));
    };
    // SDL sends a display change only from this display on
    const SDL_DisplayID first_display = SDL_GetDisplayForWindow(window);
    use_display(first_display, on_wayland());
    bool placed = !on_wayland();

    // macOS shows the window black until the first frame is presented, so show it after that one
    bool shown = !OSHOT_MACOS;
    if (shown)
    {
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
    }
    else
    {
        SDL_SetRenderVSync(renderer, false);  // a hidden window never gets the vblank
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    const std::string ini = g_config->GetConfigDirPath() + DIR_SEP_STR "imgui.ini";
    ImGuiIO&          io  = ImGui::GetIO();
    io.IniFilename        = ini.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

#if OSHOT_FREEDESKTOP
    fontconfig.wait();
#endif
    build_font_atlas(io);
    // The tray process loaded the theme when it started, and it may have changed since
    reload_theme_if_changed();
    apply_imgui_theme();

    int ret = EXIT_SUCCESS;

    // Start the overlay window
    const Result<>& started = g_ss_tool.StartWindow();
    if (!started.ok())
    {
        dialog::error("Failed to start tool window: {}", started.error_v());
        ret   = EXIT_FAILURE;
        close = true;
    }

    // Wakes the frame loop from SDL_WaitEventTimeout(), from any thread
    register_wake_overlay([] {
        SDL_Event event{};
        event.type = SDL_EVENT_USER;
        SDL_PushEvent(&event);
    });

    bool clean_frame = false;
    // app_quitting() covers tray Quit and signals received while capturing
    while (!close && g_ss_tool.IsActive() && !app_quitting())
    {
        // An untouched overlay waits for input instead of redrawing at the refresh rate
        SDL_Event    event;
        const double timeout = g_ss_tool.GetIdleTimeout();
        bool         has_event =
            timeout > 0.0 ? SDL_WaitEventTimeout(&event, Sint32(std::ceil(timeout * 1000.0))) : SDL_PollEvent(&event);
        for (; has_event; has_event = SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                close = true;
            else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                g_dropped_paths.emplace_back(event.drop.data);
            else if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
            {
                placed = true;
                use_display(SDL_DisplayID(event.window.data1), false);
            }
            // A click after no display change means the compositor kept the first display. Not a key,
            // since SDL reports the modifiers still held from the shortcut when the overlay gets the keyboard
            else if (!placed && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            {
                placed = true;
                use_display(first_display, false);
            }
        }
        // SDL's event pump only dispatches the tray's events on Windows and macOS
        Tray::Pump();
        if (close)
            break;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        reload_theme_if_changed();

        // Start the Dear ImGui frame
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        g_ss_tool.RenderOverlay();

        // Rendering
        ImGui::Render();
        // ImGui's SDL_Renderer backend scales only the clip rects, so let SDL scale everything, or a scaled
        // output gets the overlay in its top-left corner
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

        // Save/copy or scan requested: render one more frame with only the screenshot and
        // the annotations, read it back with SDL_RenderReadPixels(), and never show it
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

        SDL_RenderPresent(renderer);
        if (!std::exchange(shown, true))
        {
            SDL_ShowWindow(window);
            SDL_RaiseWindow(window);
            SDL_SetRenderVSync(renderer, true);
        }
    }

    register_wake_overlay(nullptr);

    // Cleanup
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    // On X11 xclip dies with BadWindow if SDL's window is gone before it answers SDL's format request, and
    // the copy goes with it. The tray keeps the window, elsewhere wait for the answer before SDL_Quit()
    if (OSHOT_FREEDESKTOP && copied && !g_is_systray && !on_wayland())
    {
        const Uint64 deadline = SDL_GetTicks() + 500;
        SDL_Event    event;
        while (SDL_GetTicks() < deadline)
            if (SDL_WaitEventTimeout(&event, 50) && event.type == SDL_EVENT_CLIPBOARD_UPDATE)
                break;
    }

    // The textures belong to the renderer
    g_ss_tool.ReleaseTextures();
    g_ss_tool.SetWindow(nullptr);
#if OSHOT_MACOS
    if (kept.window == window)
    {
        SDL_HideWindow(window);
    }
    else
#endif
    {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
    }

    // The tray keeps running, so let the window system unmap the window now
    if (g_is_systray)
        SDL_PumpEvents();
    else
        SDL_Quit();

    return ret;
}
