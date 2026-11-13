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

#include "util/file_dialogs.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#include <atomic>
#include <shared_mutex>
#include <vector>

#include "i18n.hpp"
#include "platform.hpp"
#include "util/dialogs.hpp"

namespace dialog
{
std::shared_mutex& file_dialog_mutex()
{
    static std::shared_mutex mutex;
    return mutex;
}

struct dialog_state_t
{
    std::atomic<bool>          done{ false };
    std::optional<std::string> path;   // written before `done`
    std::string                error;  // why no dialog could be shown, written before `done`
};

// May run on another thread (Windows, zenity), or inside SDL_PumpEvents() (the portal)
static void SDLCALL on_dialog_closed(void* userdata, const char* const* files, int)
{
    auto* state = static_cast<dialog_state_t*>(userdata);
    if (!files)
    {
        // Shown by show_dialog(), on the thread that asked for the dialog
        const char* error = SDL_GetError();
        state->error      = error[0] != '\0' ? error : "unknown error";
    }
    else if (files[0] && files[0][0] != '\0')  // SDL 3.4's zenity backend reports a cancel as ""
        state->path = files[0];
    state->done = true;
}

static std::optional<std::string> show_dialog(const SDL_FileDialogType              type,
                                              const std::string&                    title,
                                              const std::string&                    default_path,
                                              std::span<const SDL_DialogFileFilter> filters)
{
    std::vector<SDL_DialogFileFilter> translated(filters.begin(), filters.end());
    for (SDL_DialogFileFilter& f : translated)
        f.name = i18n::tr(f.name);

    const std::shared_lock lock(file_dialog_mutex());

    // SDL copies the strings, so the properties can go once the dialog is shown
    dialog_state_t         state;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    if (!default_path.empty())
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, default_path.c_str());
    if (!translated.empty())
    {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, translated.data());
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, Sint64(translated.size()));
    }
    SDL_ShowFileDialogWithProperties(type, on_dialog_closed, &state, props);
    SDL_DestroyProperties(props);

    // The portal answers through SDL's D-Bus connection, which SDL_PumpEvents() reads
    while (!state.done)
    {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    // Otherwise it would look like a cancel, and Save or Open would seem to do nothing
    if (!state.error.empty())
    {
#if OSHOT_FREEDESKTOP
        dialog::error(
            "Couldn't show a file dialog: {}\nInstall zenity, or a desktop portal with a file chooser "
            "(xdg-desktop-portal-gtk or xdg-desktop-portal-kde).",
            state.error);
#else
        dialog::error("Couldn't show a file dialog: {}", state.error);
#endif
    }
    return std::move(state.path);
}

std::optional<std::string> open_file(const std::string&                    title,
                                     const std::string&                    default_path,
                                     std::span<const SDL_DialogFileFilter> filters)
{
    return show_dialog(SDL_FILEDIALOG_OPENFILE, title, default_path, filters);
}

std::optional<std::string> save_file(const std::string&                    title,
                                     const std::string&                    default_path,
                                     std::span<const SDL_DialogFileFilter> filters)
{
    return show_dialog(SDL_FILEDIALOG_SAVEFILE, title, default_path, filters);
}

std::optional<std::string> select_folder(const std::string& title)
{
    return show_dialog(SDL_FILEDIALOG_OPENFOLDER, title, {}, {});
}
}  // namespace dialog
