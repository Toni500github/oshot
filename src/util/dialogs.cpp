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

#include "util/dialogs.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <system_error>

#include "nvdialog/nvdialog_core.h"
#include "nvdialog/nvdialog_error.h"
#include "platform.hpp"
#include "util/window.hpp"

// I'm not including <gtk/gtk.h> just for a couple of functions
extern "C" {
using gboolean = int32_t;
gboolean gtk_events_pending();
gboolean gtk_main_iteration();
}

namespace dialog
{
static std::atomic<bool> dialogs_disabled{ false };

bool init() noexcept
{
    static std::once_flag    once;
    static std::atomic<bool> ready{ false };

    if (dialogs_disabled)
        return false;

    try
    {
        std::call_once(once, [] {
            if (nvd_init() == 0)
                ready = true;
            else
                spdlog::warn("Failed to initialize nvdialog (error {}), dialogs are disabled",
                             static_cast<int>(nvd_get_error()));
        });
    }
    catch (const std::system_error&)
    {
        return false;
    }
    return ready;
}

void disable() noexcept
{
    dialogs_disabled = true;
}

void create(const char* title, const NvdDialogType type, const std::string& str) noexcept
{
    if (!init())
        return;

    // The overlay is always on top, so the dialog would open behind it.
    // On Windows, hiding it hands the foreground to another window, which then
    // stays in front of the dialog. Let the overlay own the dialog instead
#if OSHOT_WINDOWS
    nvd_set_parent(dialog_parent_window());
#else
    const bool hidden = hide_window();
#endif

    NvdDialogBox* dialog = nvd_dialog_box_new(title, str.c_str(), type);
    nvd_show_dialog(dialog);
    nvd_free_object(dialog);
#if OSHOT_FREEDESKTOP
    while (gtk_events_pending())
        gtk_main_iteration();
#endif

#if OSHOT_WINDOWS
    nvd_set_parent(nullptr);
#else
    if (hidden)
        show_window();
#endif
}
}  // namespace dialog
