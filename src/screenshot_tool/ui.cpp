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

#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <optional>
#include <string>
#include <string_view>

#include "i18n.hpp"
#include "imgui/imgui_stdlib.h"
#include "screenshot_tool.hpp"
#include "theme.hpp"
#include "util/file_dialogs.hpp"
#include "util/paths.hpp"
#include "util/window.hpp"

namespace ui
{

// ------------------------------
// Layout
// ------------------------------

float ButtonWidth(std::initializer_list<const char*> labels)
{
    float width = 0.0f;
    for (const char* label : labels)
        width = std::max(width, ImGui::CalcTextSize(label, nullptr, true).x);
    return width + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// ------------------------------
// Windows
// ------------------------------

Window::Window(GeneralContext<SubWindow>&   shown,
               GeneralContext<CaptureFlag>& capture,
               const SubWindow              id,
               const char*                  title,
               const ImVec2                 size,
               const ImGuiWindowFlags       flags)
    : m_shown(shown), m_id(id)
{
    ImGui::SetNextWindowSize(FontRelative(size.x, size.y), ImGuiCond_FirstUseEver);
    m_visible = ImGui::Begin(title, &m_open, flags | ImGuiWindowFlags_NoSavedSettings);
    if (capture.Has(CaptureFlag::Esc) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        capture.Clear(CaptureFlag::Esc);
        m_open = false;
    }
}

Window::~Window()
{
    ImGui::End();
    m_shown.Set(m_id, m_open);
}

// ------------------------------
// Widgets
// ------------------------------

// Variadic like ImGui::TextWrapped(), so it takes the same printf-style translated formats
void TextWrappedColored(const ImVec4& col, const char* fmt, ...)  // NOLINT(modernize-avoid-variadic-functions)
{
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

bool SearchBox(ImGuiTextFilter& filter, const char* id, const char* hint, const float width)
{
    ImGui::SetNextItemWidth(width);
    return filter.DrawWithHint(id, hint);
}

bool PassFilter(const ImGuiTextFilter& filter, std::initializer_list<std::string_view> fields)
{
    if (!filter.IsActive())
        return true;

    // A newline between them, so a search can't match across two fields
    std::string text;
    for (const std::string_view field : fields)
    {
        text.append(field);
        text += '\n';
    }
    return filter.PassFilter(text.data(), text.data() + text.size());
}

// https://github.com/pthom/imgui/blob/808272622f52d2f36124629c29994d2a5a7eb2f2/imgui_demo.cpp#L273
void HelpMarker(const char* desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool TimedButton(const char* label1, const char* label2, bool& armed, const ImVec2& size, const float delay_secs)
{
    // Per button, so pressing another one doesn't restart this one's countdown
    ImGui::PushID(label1);
    const ImGuiID key = ImGui::GetID("##press_time");
    ImGui::PopID();
    ImGuiStorage* storage = ImGui::GetStateStorage();

    const auto now = float(ImGui::GetTime());
    if (armed && now - storage->GetFloat(key) > delay_secs)
        armed = false;

    if (ImGui::Button(!armed ? label1 : label2, size))
    {
        storage->SetFloat(key, now);
        return true;  // caller decides whether to set armed=true
    }
    return false;
}

bool DangerButton(const char* label, const ImVec2& size)
{
    ScopedStyle style;
    style.Color(ImGuiCol_Button, theme::color(AppCol::Danger).to_abgr())
        .Color(ImGuiCol_ButtonHovered, theme::color(AppCol::DangerHovered).to_abgr())
        .Color(ImGuiCol_ButtonActive, theme::color(AppCol::DangerActive).to_abgr());
    return ImGui::Button(label, size);
}

void Tag(const char* label, const rgba_t col, const float line_height)
{
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 pad(6.0f, 2.0f);
    const ImVec2 size(text_size.x + pad.x * 2, text_size.y + pad.y * 2);
    const float  offset = std::round(std::max(line_height - size.y, 0.0f) * 0.5f);
    const ImVec2 p0(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + offset);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(p0, p0 + size, col.to_abgr(), 4.0f);
    draw_list->AddText(p0 + pad, Colors::WHITE.to_abgr(), label);

    ImGui::Dummy(ImVec2(size.x, std::max(size.y, line_height)));
}

void InputPath(const char*                  label,
               const char*                  input_id,
               const bool                   is_file,
               const SDL_DialogFileFilter   filter,
               const std::function<void()>& if_edited,
               std::string&                 path,
               ImGuiInputTextFlags          flags,
               ImGuiInputTextCallback       callback,
               void*                        user_data)
{
    auto handle_drop = [&]() {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !g_dropped_paths.empty())
        {
            path = g_dropped_paths.back();
            g_dropped_paths.clear();
            if (if_edited)
                if_edited();
        }
    };

    const float button_size = ImGui::GetFrameHeight();
    ImGui::PushItemWidth(ImGui::CalcItemWidth() - button_size);
    if (ImGui::InputText(input_id, &path, flags, callback, user_data) && if_edited)
        if_edited();
    ImGui::PopItemWidth();
    handle_drop();

    ImGui::PushID(input_id);
    ImGui::SameLine(0, 0);
    if (ImGui::Button("...", ImVec2(button_size, button_size)))
    {
        const bool hidden = hide_window();

        const fs::path default_path = path;

        std::string start_path;
        if (is_file && default_path.has_parent_path())
        {
            start_path = default_path.parent_path().string();
            start_path += DIR_SEP;  // a trailing separator opens the folder itself
        }

        const std::optional<std::string>& dialog_path =
            !is_file ? dialog::select_folder(_("Open folder")) : dialog::open_file(_("Open file"), start_path, filter);

        if (hidden)
            show_window();

        if (dialog_path)
        {
            path = *dialog_path;
            if (if_edited)
                if_edited();
        }
    }
    handle_drop();
    ImGui::PopID();

    if (*label)
    {
        ImGui::SameLine(0, 3);
        ImGui::TextUnformatted(label);
    }
}

// ------------------------------
// Modals
// ------------------------------

bool BeginModal(const char* id, const ImGuiWindowFlags flags)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    return ImGui::BeginPopupModal(id, nullptr, flags | ImGuiWindowFlags_AlwaysAutoResize);
}

int ModalButtons(std::initializer_list<modal_button_t> buttons)
{
    ImGui::Spacing();

    float width = ImGui::GetFontSize() * 6.0f;
    for (const modal_button_t& b : buttons)
        width = std::max(width, ButtonWidth(b.label));

    // Not on the frame it opens, whose Esc may be the one that opened it
    const bool esc = !ImGui::IsWindowAppearing() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    int pressed = -1;
    int i       = 0;
    for (const modal_button_t& b : buttons)
    {
        if (i > 0)
            ImGui::SameLine();
        const bool clicked = (b.flags & ModalButtonFlags_Danger) ? DangerButton(b.label, ImVec2(width, 0.0f))
                                                                 : ImGui::Button(b.label, ImVec2(width, 0.0f));
        if (pressed < 0 && (clicked || (esc && (b.flags & ModalButtonFlags_Escape))))
            pressed = i;
        ++i;
    }

    if (pressed >= 0)
        ImGui::CloseCurrentPopup();
    return pressed;
}

}  // namespace ui
