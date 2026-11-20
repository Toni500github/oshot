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

#ifndef _SCREENSHOT_TOOL_UI_HPP_
#define _SCREENSHOT_TOOL_UI_HPP_

// ImGui widgets and windows shared by the src/screenshot_tool/*.cpp files

#include <SDL3/SDL_dialog.h>

#include <cfloat>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

#include "imgui/imgui.h"
#include "util/color.hpp"

enum class SubWindow : std::uint8_t;
template <typename Enum>
struct GeneralContext;

namespace ui
{

// ------------------------------
// Types
// ------------------------------

/**
 * Pushes style vars and colors, and pops them at the end of its scope.
 * Declared before a window, it outlives the window's End()
 **/
class ScopedStyle
{
public:
    ScopedStyle() = default;
    ~ScopedStyle() { Pop(); }

    ScopedStyle(const ScopedStyle&)            = delete;
    ScopedStyle& operator=(const ScopedStyle&) = delete;

    /* Pops everything pushed so far, before the end of the scope */
    void Pop()
    {
        ImGui::PopStyleColor(m_colors);
        ImGui::PopStyleVar(m_vars);
        m_colors = m_vars = 0;
    }

    ScopedStyle& Var(const ImGuiStyleVar idx, const float val)
    {
        ImGui::PushStyleVar(idx, val);
        ++m_vars;
        return *this;
    }

    ScopedStyle& Var(const ImGuiStyleVar idx, const ImVec2& val)
    {
        ImGui::PushStyleVar(idx, val);
        ++m_vars;
        return *this;
    }

    ScopedStyle& Color(const ImGuiCol idx, const ImVec4& col)
    {
        ImGui::PushStyleColor(idx, col);
        ++m_colors;
        return *this;
    }

    ScopedStyle& Color(const ImGuiCol idx, const ImU32 col)
    {
        ImGui::PushStyleColor(idx, col);
        ++m_colors;
        return *this;
    }

private:
    int m_vars   = 0;
    int m_colors = 0;
};

// Flags of a ModalButtons() button
enum ModalButtonFlags_ : std::uint8_t
{
    ModalButtonFlags_None   = 0,
    ModalButtonFlags_Escape = 1 << 0,  // Esc presses it too
    ModalButtonFlags_Danger = 1 << 1,  // drawn as a DangerButton()
};
using ModalButtonFlags = std::uint8_t;

struct modal_button_t
{
    modal_button_t(const char* label_, const ModalButtonFlags flags_ = ModalButtonFlags_None)
        : label(label_), flags(flags_)
    {}

    const char*      label;
    ModalButtonFlags flags;
};

/**
 * A sub-window of the overlay, which the caller only creates while its flag is set in `shown`.
 * Begins when constructed. Ends and writes back whether it's still open when destroyed
 **/
class Window
{
public:
    /**
     * @param shown The flags of the shown sub-windows
     * @param id This window's flag
     * @param title The title, with a ### ID so a translation doesn't lose its size and place
     * @param size The first size in font sizes (FontRelative()). A 0 height fits the contents
     * @param flags Added to ImGuiWindowFlags_NoSavedSettings
     **/
    Window(GeneralContext<SubWindow>& shown, SubWindow id, const char* title, ImVec2 size, ImGuiWindowFlags flags = 0);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    /* False while collapsed, so there's nothing to draw */
    explicit operator bool() const { return m_visible; }

    /* Whether the window closes at the end of the scope, e.g. after a click on its [x] */
    [[nodiscard]] bool Closing() const { return !m_open; }

    void Close() { m_open = false; }
    void KeepOpen() { m_open = true; }

private:
    GeneralContext<SubWindow>& m_shown;
    SubWindow                  m_id;
    bool                       m_open = true;
    bool                       m_visible;
};

/* What every OverlayWindow is: no decorations, not movable, sized to its contents */
inline constexpr ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

/**
 * A window drawn on the overlay at a fixed place, e.g. a hint or a label.
 * Begins when constructed, ends when destroyed
 **/
class OverlayWindow
{
public:
    /**
     * @param name The window name, usually a `##` ID
     * @param pos Where the window goes, every frame
     * @param flags Added to kOverlayFlags
     * @param pivot The point of the window placed at `pos`, (0.5, 0.5) centers it
     **/
    OverlayWindow(const char*      name,
                  const ImVec2&    pos,
                  ImGuiWindowFlags flags = 0,
                  const ImVec2&    pivot = ImVec2(0.0f, 0.0f))
    {
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        m_visible = ImGui::Begin(name, nullptr, kOverlayFlags | flags);
    }
    ~OverlayWindow() { ImGui::End(); }

    OverlayWindow(const OverlayWindow&)            = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    explicit operator bool() const { return m_visible; }

private:
    bool m_visible;
};

// ------------------------------
// Layout
// ------------------------------

/**
 * A size in multiples of the font size, so windows fit the same text whatever the font
 * @param w Width in font sizes
 * @param h Height in font sizes
 **/
inline ImVec2 FontRelative(const float w, const float h)
{
    return { ImGui::GetFontSize() * w, ImGui::GetFontSize() * h };
}

/**
 * The width of a button that fits the widest of the labels, for a row of buttons of the same size
 * @param labels The button labels, `##` suffixes ignored
 **/
float ButtonWidth(std::initializer_list<const char*> labels);

/* @param label The button label, `##` suffix ignored */
inline float ButtonWidth(const char* label)
{
    return ButtonWidth({ label });
}

/**
 * Right-aligns the next item
 * @param w The width of the item
 * @param same_line Puts it on the current line, else on a new one
 **/
inline void AlignRight(const float w, const bool same_line = true)
{
    const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w;
    if (same_line)
        ImGui::SameLine(x);
    else
        ImGui::SetCursorPosX(x);
}

// ------------------------------
// Widgets
// ------------------------------

/**
 * ImGui::TextWrapped() in another color
 * @param col The text color
 * @param fmt, ... printf-style format and arguments
 **/
void TextWrappedColored(const ImVec4& col, const char* fmt, ...) IM_FMTARGS(2);

/**
 * A search box for `filter`. Case-insensitive, "a,b" matches either and "-a" excludes
 * @param filter The filter it edits
 * @param id The box's ID
 * @param hint Shown while it's empty
 * @param width The box's width, -FLT_MIN for the rest of the line
 * @return true when the text changed
 **/
bool SearchBox(ImGuiTextFilter& filter, const char* id, const char* hint, float width = -FLT_MIN);

/**
 * Whether a record with several fields passes `filter`. The fields are matched as one text,
 * so "-a" excludes the record when any of them has it
 * @param filter The filter
 * @param fields The record's fields
 **/
bool PassFilter(const ImGuiTextFilter& filter, std::initializer_list<std::string_view> fields);

/**
 * A little (?) mark which shows a tooltip when hovered
 * @param desc The tooltip text
 **/
void HelpMarker(const char* desc);

/**
 * A button whose label changes to `label2` for a moment after a press, e.g. "Copy" to "Copied!"
 * @param label1 The normal label
 * @param label2 The label while armed
 * @param armed Set by the caller after a press it accepts, cleared here once `delay_secs` passed
 * @param size The button size
 * @param delay_secs How long `label2` stays
 * @return true when pressed
 **/
bool TimedButton(const char*   label1,
                 const char*   label2,
                 bool&         armed,
                 const ImVec2& size       = ImVec2(0, 0),
                 float         delay_secs = 1.5f);

/**
 * A button in the colors of a destructive action, e.g. Uninstall
 * @param label The label
 * @param size The button size
 * @return true when pressed
 **/
bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0));

/* The width Tag() gives `label` */
inline float TagWidth(const char* label)
{
    return ImGui::CalcTextSize(label).x + 12.0f;
}

/**
 * A small rounded label, e.g. a license or a status
 * @param label The text
 * @param col The background color
 * @param line_height When taller than the tag, centers it on a line of that height, e.g. one of buttons
 **/
void Tag(const char* label, rgba_t col, float line_height = 0.0f);

/**
 * A text field for a path with a "..." button that opens a file or folder dialog.
 * A path dropped on it replaces the text
 * @param label Shown after the button, if not empty
 * @param input_id The text field's ID
 * @param is_file Opens a file dialog if true, else a folder dialog
 * @param filter The file dialog's filter
 * @param if_edited Called after the path changed
 * @param path The path to edit
 * @param flags, callback, user_data Passed to ImGui::InputText()
 **/
void InputPath(const char*                  label,
               const char*                  input_id,
               bool                         is_file,
               SDL_DialogFileFilter         filter,
               const std::function<void()>& if_edited,
               std::string&                 path,
               ImGuiInputTextFlags          flags,
               ImGuiInputTextCallback       callback  = nullptr,
               void*                        user_data = nullptr);

/* InputPath() with a file dialog */
inline void InputFile(const char*                  label,
                      const char*                  input_id,
                      const SDL_DialogFileFilter   filter,
                      const std::function<void()>& if_edited,
                      std::string&                 path,
                      ImGuiInputTextFlags          flags     = 0,
                      ImGuiInputTextCallback       callback  = nullptr,
                      void*                        user_data = nullptr)
{
    InputPath(label, input_id, true, filter, if_edited, path, flags, callback, user_data);
}

/* InputPath() with a folder dialog */
inline void InputFolder(const char*                  label,
                        const char*                  input_id,
                        const std::function<void()>& if_edited,
                        std::string&                 path,
                        ImGuiInputTextFlags          flags = 0)
{
    InputPath(label, input_id, false, {}, if_edited, path, flags);
}

// ------------------------------
// Modals
// ------------------------------

/**
 * BeginPopupModal(), centered on the display and sized to its contents. The caller calls OpenPopup() outside any
 * PushID(), and EndPopup() if this returns true
 * @param id The popup's ID, the same as OpenPopup()'s
 * @param flags Added to ImGuiWindowFlags_AlwaysAutoResize
 **/
bool BeginModal(const char* id, ImGuiWindowFlags flags = 0);

/**
 * A row of buttons of the same width that close the current modal
 * @param buttons The buttons, in order
 * @return The index of the pressed button, or -1
 **/
int ModalButtons(std::initializer_list<modal_button_t> buttons);

/**
 * ModalButtons() without the braces, e.g. ModalButtons(_("Yes"), ModalEscape(_("No")))
 * @param buttons Labels, or buttons from ModalEscape(), ModalDanger() or modal_button_t
 * @return The index of the pressed button, or -1
 **/
template <typename... Buttons>
int ModalButtons(const Buttons&... buttons)
{
    static_assert(sizeof...(Buttons) > 0, "a modal needs a button to close it");
    return ModalButtons({ modal_button_t(buttons)... });
}

/* A ModalButtons() button that Esc presses too */
inline modal_button_t ModalEscape(const char* label)
{
    return { label, ModalButtonFlags_Escape };
}

/* A ModalButtons() button drawn as a DangerButton() */
inline modal_button_t ModalDanger(const char* label)
{
    return { label, ModalButtonFlags_Danger };
}

}  // namespace ui

#endif  // _SCREENSHOT_TOOL_UI_HPP_
