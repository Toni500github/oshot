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

#ifndef _UTIL_FILE_DIALOGS_HPP_
#define _UTIL_FILE_DIALOGS_HPP_

// Native file dialogs (SDL's), which wait until they're closed

#include <SDL3/SDL_dialog.h>

#include <optional>
#include <span>
#include <string>

namespace dialog
{
/**
 * Show an open file dialog and wait until it's closed
 * @param title The dialog's title
 * @param default_path The folder to start in when it ends with a separator, else a file in it. May be empty
 * @param filters The file types to show, e.g. { N_("Images"), "png;jpg" }. Their names get translated
 * @return The chosen file, or nothing if the dialog was canceled
 **/
std::optional<std::string> open_file(const std::string&                    title,
                                     const std::string&                    default_path,
                                     std::span<const SDL_DialogFileFilter> filters);

/**
 * Show a save file dialog and wait until it's closed
 * @param title The dialog's title
 * @param default_path The file name to suggest, with its folder. May be empty
 * @param filters The file types to show, e.g. { N_("Images"), "png" }. Their names get translated
 * @return The chosen file, or nothing if the dialog was canceled
 **/
std::optional<std::string> save_file(const std::string&                    title,
                                     const std::string&                    default_path,
                                     std::span<const SDL_DialogFileFilter> filters);

/**
 * Show a folder picker and wait until it's closed
 * @param title The dialog's title
 * @return The chosen folder, or nothing if the dialog was canceled
 **/
std::optional<std::string> select_folder(const std::string& title);
}  // namespace dialog

#endif  // _UTIL_FILE_DIALOGS_HPP_
