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

#ifndef _APP_HPP_
#define _APP_HPP_

// ------------------------------
// Captures and the tray (app.cpp)
// ------------------------------

/* Whether a quit was requested, by the tray menu, a signal or the system */
bool app_quitting();

/**
 * Quit from the tray menu, a signal or the system (tray or main thread).
 * A capture in progress sees app_quitting() and closes itself
 **/
void request_quit();

/**
 * Handler for SIGINT/SIGTERM. Async-signal-safe, so it only raises the quit flag.
 * Once the tray is up, Tray::SetQuitSignalCallback() takes over
 * @param sig The signal number (unused)
 **/
void on_quit_signal(int sig);

/* atexit() handler. Quits the tray and SDL unless a capture is running */
void exit_handler_nc();

/**
 * Open one overlay on this thread (screenshot or Runtime.source_file), then release its memory
 * @return The exit code of run_main_tool(), or EXIT_SUCCESS if another overlay is already open
 **/
int run_capture();

/**
 * Run as the tray (or capture once if another process owns it), until quit.
 * Takes the tray lock, starts the first capture unless --tray, and runs the tray loop on this thread
 * @return The process exit code
 **/
int run_tray();

// ------------------------------
// Window backend (main_tool_sdl3.cpp)
// ------------------------------

/**
 * Create the overlay window and run its frame loop until the capture ends
 * @return The process exit code
 **/
int run_main_tool();

#endif  // _APP_HPP_
