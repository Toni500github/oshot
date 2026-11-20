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

#ifndef _UTIL_WINDOW_HPP_
#define _UTIL_WINDOW_HPP_

// The overlay window, for the code that runs while it's open

#include "platform.hpp"

struct SDL_Window;

// ------------------------------
// Functions
// ------------------------------

/**
 * Set the overlay's window, or nullptr once it's gone. The calling thread becomes the only one that may touch it
 * @param window Has a renderer, which ScreenshotTool draws with
 **/
void set_overlay_window(SDL_Window* window);

/* The overlay's window, nullptr off its thread or without an overlay */
SDL_Window* overlay_window();

/* Minimize the overlay window, if there's one */
void minimize_window();

/**
 * Hide the always-on-top overlay while a modal dialog is open, or the dialog opens behind it.
 * Does nothing off the overlay's thread
 * @return true if the overlay was on screen and is now hidden: call show_window() after the dialog
 **/
bool hide_window();

/* Show the overlay window again after hide_window() */
void show_window();

/**
 * The overlay's native window (HWND on Windows), to own a dialog so it stays above the overlay
 * @return nullptr off the overlay's thread, without an overlay, or where owning isn't supported
 **/
void* dialog_parent_window();

/* Make the overlay draw its next frame now instead of waiting for input. Any thread */
void wake_overlay();

#if OSHOT_MACOS
/**
 * Handle the system's quit request (shutdown, logout) ourselves, since SDL turns NSApp's into an
 * SDL_EVENT_QUIT and macOS aborts the shutdown. SDL_Init() replaces it, so install it after that
 * @param on_quit Called on the main thread when the request arrives
 **/
void install_quit_handler(void (*on_quit)());
#endif

#endif  // _UTIL_WINDOW_HPP_
