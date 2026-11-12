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

#ifndef _SCREENSHOT_TOOL_HELPERS_HPP_
#define _SCREENSHOT_TOOL_HELPERS_HPP_

// Shared by the src/screenshot_tool/*.cpp files

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"
#include "screenshot_tool.hpp"
#include "tinyfiledialogs.h"
#include "util.hpp"

#ifdef DISABLE_PLUGINS
#  define OSHOT_TEXT_OCR     "ocr_output"
#  define OSHOT_TEXT_BARCODE "barcode_output"
#endif

constexpr rgba_t::rgba_t(ImVec4 vec)
    : r(static_cast<uint8_t>(vec.x * 255.0f)),
      g(static_cast<uint8_t>(vec.y * 255.0f)),
      b(static_cast<uint8_t>(vec.z * 255.0f)),
      a(static_cast<uint8_t>(vec.w * 255.0f))
{}

constexpr ImVec4 rgba_t::to_imvec4() const
{
    return { float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, float(a) / 255.0f };
}

constexpr rgba_t::operator ImVec4() const
{
    return to_imvec4();
}

// ------------------------------
// Helpers
// ------------------------------

// https://github.com/pthom/imgui/blob/808272622f52d2f36124629c29994d2a5a7eb2f2/imgui_demo.cpp#L273
// A little (?) mark which shows a tooltip when hovered
inline void HelpMarker(const char* desc)
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

// A size in multiples of the font size, so windows fit the same text whatever the font
inline ImVec2 font_relative(const float w, const float h)
{
    return { ImGui::GetFontSize() * w, ImGui::GetFontSize() * h };
}

// The width of a button that fits the widest of the labels, for a row of buttons of the same size
inline float button_width(std::initializer_list<const char*> labels)
{
    float width = 0.0f;
    for (const char* label : labels)
        width = std::max(width, ImGui::CalcTextSize(label, nullptr, true).x);
    return width + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// Tools that draw a button in the toolbar
constexpr bool has_toolbar_button(ToolType t)
{
    return t != ToolType::kNone && t != ToolType::COUNT && t != ToolType::Logo;
}

template <size_t N>
inline void draw_input_text_path(const char*                      label,
                                 const char*                      input_id,
                                 const bool                       is_file,
                                 const std::array<const char*, N> filters,
                                 const std::function<void()>&     if_edited,
                                 std::string&                     path,
                                 ImGuiInputFlags                  flags,
                                 ImGuiInputTextCallback           callback  = nullptr,
                                 void*                            user_data = nullptr)
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
            start_path += DIR_SEP;  // tinyfd treats a trailing separator as "open here"
        }

        const char* dialog_path =
            !is_file ? tinyfd_selectFolderDialog(_("Open folder"), nullptr)
                     : tinyfd_openFileDialog(
                           _("Open file"), start_path.c_str(), int(filters.size()), filters.data(), nullptr, 0);

        if (hidden)
            show_window();

        if (dialog_path)
        {
            path.assign(dialog_path);
            if (if_edited)
                if_edited();
        }
    }
    handle_drop();
    ImGui::PopID();

    ImGui::SameLine(0, 3);
    ImGui::TextUnformatted(label);
}

template <size_t N>
inline void draw_input_text_file(const char*                      label,
                                 const char*                      input_id,
                                 const std::array<const char*, N> filters,
                                 const std::function<void()>&     if_edited,
                                 std::string&                     path,
                                 ImGuiInputFlags                  flags     = 0,
                                 ImGuiInputTextCallback           callback  = nullptr,
                                 void*                            user_data = nullptr)
{
    draw_input_text_path<N>(label, input_id, true, filters, if_edited, path, flags, callback, user_data);
}

inline void draw_input_text_folder(const char*                  label,
                                   const char*                  input_id,
                                   const std::function<void()>& if_edited,
                                   std::string&                 path,
                                   ImGuiInputFlags              flags = 0)
{
    draw_input_text_path<0>(label, input_id, false, {}, if_edited, path, flags);
}

inline bool create_timed_button(const std::string_view label1,
                                const std::string_view label2,
                                bool&                  armed,  // caller controls arming
                                const float            delay_secs = 1.5f)
{
    static double press_time = 0.0f;

    const double now = ImGui::GetTime();
    if (armed && now - press_time > delay_secs)
        armed = false;

    if (ImGui::Button(!armed ? label1.data() : label2.data()))
    {
        press_time = now;
        return true;  // caller decides whether to set armed=true
    }
    return false;
}

// ImGuiCond_Appearing, plus again whenever the display size changes (on Wayland
// the overlay only becomes fullscreen after its first frame has been drawn)
inline ImGuiCond center_cond(ImVec2& last_display)
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x == last_display.x && display.y == last_display.y)
        return ImGuiCond_Appearing;
    last_display = display;
    return ImGuiCond_Always;
}
#endif  // _SCREENSHOT_TOOL_HELPERS_HPP_
