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

#include "util/window.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <atomic>
#include <thread>

#include "platform.hpp"

#if OSHOT_MACOS
// AssertMacros.h (pulled in by CoreServices) defines check()/verify()/require() otherwise
#  define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#  include <CoreServices/CoreServices.h>
#endif

static std::atomic<SDL_Window*>     overlay{ nullptr };
static std::atomic<std::thread::id> overlay_thread;

void set_overlay_window(SDL_Window* window)
{
    overlay_thread = std::this_thread::get_id();
    overlay        = window;
}

SDL_Window* overlay_window()
{
    SDL_Window* window = overlay;
    return window && std::this_thread::get_id() == overlay_thread.load() ? window : nullptr;
}

void minimize_window()
{
    if (SDL_Window* window = overlay_window())
        SDL_MinimizeWindow(window);
}

bool hide_window()
{
    SDL_Window* window = overlay_window();
    if (!window || (SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)))
        return false;

    // Unmap it rather than minimize it, since SDL doesn't restore a minimized fullscreen window on Windows,
    // sway ignores minimize requests, and macOS's minimize animation never finishes under the modal loop
    SDL_HideWindow(window);
    SDL_PumpEvents();
    return true;
}

void show_window()
{
    if (SDL_Window* window = overlay_window())
    {
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
        // A dialog from OnCleanFrame() (the save dialog) returns in a frame that isn't presented
        wake_overlay();
    }
}

void* dialog_parent_window()
{
#if OSHOT_WINDOWS
    if (SDL_Window* window = overlay_window())
        return SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    return nullptr;
}

void wake_overlay()
{
    // Wakes the frame loop from SDL_WaitEventTimeout(). Without an overlay nothing waits, and SDL may be quit
    if (!overlay.load())
        return;
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
}

#if OSHOT_MACOS
static void (*quit_handler_fn)() = nullptr;

static OSErr on_quit_event(const AppleEvent*, AppleEvent*, SRefCon)
{
    if (quit_handler_fn)
        quit_handler_fn();
    return noErr;
}

void install_quit_handler(void (*on_quit)())
{
    static const AEEventHandlerUPP upp = NewAEEventHandlerUPP(on_quit_event);
    quit_handler_fn                    = on_quit;
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, upp, 0, false);
}
#endif
