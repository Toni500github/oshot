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

#ifndef _UTIL_DIALOGS_HPP_
#define _UTIL_DIALOGS_HPP_

// Message dialogs, and the log helpers that show one

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "fmt/color.h"
#include "fmt/format.h"
#include "i18n.hpp"
#include "nvdialog/nvdialog_dialog.h"
#include "nvdialog/nvdialog_types.h"
#include "platform.hpp"
#include "spdlog/spdlog.h"

#if OSHOT_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// ------------------------------
// Macros
// ------------------------------

/* fmt style for bold text in color `x` */
#define BOLD_COLOR(x) (fmt::emphasis::bold | fmt::fg(x))

namespace dialog
{
// ------------------------------
// Functions
// ------------------------------

/**
 * Set up nvdialog the first time a dialog or notification needs it.
 * On Linux this starts GTK, which takes tens of milliseconds and several MB, so an
 * overlay that never shows a dialog or notification never starts it. Thread-safe, but
 * call it on the main thread where possible, since that's where GTK gets set up.
 * @return false if dialogs are unavailable (init failed, or disable())
 **/
bool init() noexcept;

/* Never show dialogs again, e.g. in the oshotpm CLI, so messages only go to the log or terminal */
void disable() noexcept;

/**
 * Show a dialog and wait until it's closed. Does nothing if dialogs are unavailable
 * @param title The dialog's title
 * @param type The dialog's icon (NVD_DIALOG_SIMPLE, NVD_DIALOG_WARNING or NVD_DIALOG_ERROR)
 * @param str The message
 **/
void create(const char* title, const NvdDialogType type, const std::string& str) noexcept;

// ------------------------------
// Template functions
// ------------------------------

/**
 * fmt::format() with a runtime format string, which never throws.
 * Used by the noexcept helpers below
 * @param fmt The format string
 * @param args Arguments in the format
 * @return The formatted string, or `fmt` unformatted if formatting failed
 **/
template <typename... Args>
inline std::string format_noexcept(const std::string_view fmt, Args&&... args) noexcept
{
    try
    {
        return fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
    catch (...)
    {
        return std::string(fmt);
    }
}

/**
 * Log a critical error, show it in a dialog, then exit with code 1
 * @param fmt The format string
 * @param args Arguments in the format
 **/
template <typename... Args>
// NOLINTNEXTLINE(bugprone-exception-escape): only reachable through spdlog's own error handler
[[noreturn]] inline void die(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = format_noexcept(fmt, std::forward<Args>(args)...);

    spdlog::critical("{}", str);
    create(_("oshot Fatal Error"), NVD_DIALOG_ERROR, str);
    std::exit(1);
}

/**
 * Log an error and show it in a dialog
 * @param fmt The format string
 * @param args Arguments in the format
 **/
template <typename... Args>
// NOLINTNEXTLINE(bugprone-exception-escape): only reachable through spdlog's own error handler
inline void error(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = format_noexcept(fmt, std::forward<Args>(args)...);

    spdlog::error("{}", str);
    create(_("oshot Error"), NVD_DIALOG_ERROR, str);
}

/**
 * Log a warning and show it in a dialog
 * @param fmt The format string
 * @param args Arguments in the format
 **/
template <typename... Args>
// NOLINTNEXTLINE(bugprone-exception-escape): only reachable through spdlog's own error handler
inline void warn(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = format_noexcept(fmt, std::forward<Args>(args)...);

    spdlog::warn("{}", str);
    create(_("oshot Warning"), NVD_DIALOG_WARNING, str);
}

/**
 * Log a message and show it in a dialog
 * @param fmt The format string
 * @param args Arguments in the format
 **/
template <typename... Args>
// NOLINTNEXTLINE(bugprone-exception-escape): only reachable through spdlog's own error handler
inline void info(const std::string_view fmt, Args&&... args) noexcept
{
    const std::string& str = format_noexcept(fmt, std::forward<Args>(args)...);

    spdlog::info("{}", str);
    create(_("oshot Info"), NVD_DIALOG_SIMPLE, str);
}

/**
 * Ask the user a yes or no question.
 * Uses a dialog, or the terminal when dialogs are unavailable (e.g. in oshotpm).
 * @param def The answer when the user just presses Enter in the terminal
 * @param fmt The format string
 * @param args Arguments in the format
 * @return true for yes, false for no. EOF (CTRL-D) counts as no
 **/
template <typename... Args>
inline bool ask_user_yn(bool def, const std::string_view fmt, Args&&... args)
{
    const std::string& str = fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);
    if (!init())
    {
#if OSHOT_WINDOWS
        // oshot.exe has no console, but oshotpm.exe (and oshot run from one) has
        if (!GetConsoleWindow())
        {
            int result = MessageBox(NULL, str.c_str(), _("Confirmation"), MB_YESNO | MB_ICONQUESTION);
            return (result == IDYES);
        }
#endif
        const std::string_view inputs_str = def ? "[Y/n]" : "[y/N]";
        std::string            result;
        fmt::print("{} {}: ", str, inputs_str);

        while (std::getline(std::cin, result) && (result.length() > 1))
        {
            fmt::print(BOLD_COLOR(fmt::rgb(fmt::color::yellow)), "Please answear y or n\n");
            fmt::print("{} {}: ", str, inputs_str);
        }

        // CTRL-D or no input left counts as a "no", so callers can cancel and clean up
        if (std::cin.eof())
        {
            fmt::print("\n");
            return false;
        }

        if (result.empty())
            return def;

        if (def ? std::tolower(result[0]) != 'n' : std::tolower(result[0]) != 'y')
            return def;

        return !def;
    }
    else
    {
        NvdQuestionBox* question = nvd_dialog_question_new(_("Confirmation"), str.c_str(), NVD_YES_NO);
        if (!question)
            die("Couldn't create question dialog box");
        return nvd_get_reply(question) == NVD_REPLY_OK;
    }
}
}  // namespace dialog

#endif  // _UTIL_DIALOGS_HPP_
