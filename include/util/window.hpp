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

// The overlay window's functions, registered by the graphics backend

#include "platform.hpp"

// ------------------------------
// Functions
// ------------------------------

/**
 * Register the window functions of the graphics backend (main_tool_*), so oshot_common can call them
 * @param minimize_fn Minimizes the overlay, e.g. when a link opens in the browser
 * @param hide_fn Hides the overlay while a modal dialog is open, see hide_window()
 * @param show_fn Shows the overlay again after hide_fn
 * @param dialog_parent_fn Returns the overlay's native window, see dialog_parent_window(). May be nullptr
 **/
void register_window_callbacks(void (*minimize_fn)(),
                               bool (*hide_fn)(),
                               void (*show_fn)(),
                               void* (*dialog_parent_fn)());

/* Minimize the overlay window, if a backend registered one */
void minimize_window();

/**
 * Hide the overlay window while a modal dialog (file picker, message box) is open.
 * It's always on top, so the dialog would open behind it, and its frame loop is
 * blocked until the dialog closes. Does nothing off the overlay's thread
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

#if OSHOT_MACOS
/**
 * Handle the system's quit request (shutdown, logout) ourselves. NSApp's own
 * handler calls -terminate:, which SDL only turns into an SDL_EVENT_QUIT, so
 * macOS aborted the shutdown. Finishing the app launch in SDL_Init() replaces
 * the handler, so install it again after that
 * @param on_quit Called on the main thread when the request arrives
 **/
void install_quit_handler(void (*on_quit)());
#endif

#endif  // _UTIL_WINDOW_HPP_
