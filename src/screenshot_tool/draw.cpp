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

#include <tesseract/publictypes.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "cache.hpp"
#include "clipboard.hpp"
#include "config.hpp"
#include "fmt/format.h"
#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#include "platform.hpp"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "text_extraction.hpp"
#include "theme.hpp"
#include "util.hpp"
#include "util/color.hpp"
#include "util/file_dialogs.hpp"
#include "util/fonts.hpp"
#include "util/image.hpp"
#include "util/result.hpp"
#include "util/strings.hpp"
#include "util/window.hpp"
#ifndef DISABLE_PLUGINS
#  include "plugin.hpp"
#endif
#include "helpers.hpp"

using namespace std::chrono_literals;

// ------------------------------
// Drawing
// ------------------------------

static constexpr bool is_annotation_tool(ToolType t)
{
    return idx(t) > idx(ToolType::kNone) && idx(t) <= idx(ToolType::Pencil);
};

static const char* tool_name(ToolType t)
{
    switch (t)
    {
        case ToolType::Arrow:           return _("Arrow");
        case ToolType::Line:            return _("Line");
        case ToolType::Rectangle:       return _("Rectangle");
        case ToolType::RectangleFilled: return _("Filled rectangle");
        case ToolType::Circle:          return _("Circle");
        case ToolType::CircleFilled:    return _("Filled circle");
        case ToolType::CounterBubble:   return _("Counter");
        case ToolType::Text:            return _("Text");
        case ToolType::Pencil:          return _("Pencil");
        case ToolType::ToggleTextTools: return _("Text tools");
        case ToolType::CopyImage:       return _("Copy image");
        case ToolType::SaveImage:       return _("Save image");
        case ToolType::kNone:
        case ToolType::Logo:
        case ToolType::COUNT:           break;
    }
    return "";
}

// A setting's label, with its tooltip, then the cell for its widget
static void form_row(const char* label, const char* tooltip)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SetItemTooltip("%s", tooltip);
    ImGui::TableNextColumn();
}

// The annotation color picker's look, from the Preferences
static ImGuiColorEditFlags ann_color_picker_flags()
{
    ImGuiColorEditFlags flags =
        ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoLabel;
    flags |= g_config->File.color_picker == 0 ? ImGuiColorEditFlags_PickerHueBar : ImGuiColorEditFlags_PickerHueWheel;
    switch (ColorPickerAlpha(g_config->File.cpa_mode))
    {
        case ColorPickerAlpha::Disabled: flags |= ImGuiColorEditFlags_NoAlpha; break;
        case ColorPickerAlpha::Bar:      flags |= ImGuiColorEditFlags_AlphaBar; break;
        case ColorPickerAlpha::Inline:   break;
    }
    return flags;
}

// Where a popup of `size` for the toolbar button `btn` goes: lined up with the button, outside `avoid` (the
// selection and its toolbar) on the button's `side` or else the first other side it fits on. If none, over the
// selection next to the button
static ImVec2 toolbar_popup_pos(const ImRect& btn, const ImRect& avoid, SelectionBorder side, const ImVec2& size)
{
    static constexpr std::array<std::array<ImGuiDir, 4>, idx(SelectionBorder::COUNT)> order{ {
        { ImGuiDir_Down, ImGuiDir_Right, ImGuiDir_Left, ImGuiDir_Up },
        { ImGuiDir_Right, ImGuiDir_Down, ImGuiDir_Up, ImGuiDir_Left },
        { ImGuiDir_Up, ImGuiDir_Right, ImGuiDir_Left, ImGuiDir_Down },
        { ImGuiDir_Left, ImGuiDir_Down, ImGuiDir_Up, ImGuiDir_Right },
    } };

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float  gap     = ImGui::GetStyle().ItemSpacing.y;

    auto outside = [&](ImGuiDir dir, const ImRect& r) {
        switch (dir)
        {
            case ImGuiDir_Down:  return ImVec2(btn.Min.x, r.Max.y + gap);
            case ImGuiDir_Up:    return ImVec2(btn.Min.x, r.Min.y - gap - size.y);
            case ImGuiDir_Right: return ImVec2(r.Max.x + gap, btn.Min.y);
            default:             return ImVec2(r.Min.x - gap - size.x, btn.Min.y);
        }
    };
    auto fits = [&](ImGuiDir dir, const ImVec2& p) {
        if (p.x < 0.0f || p.y < 0.0f)
            return false;
        return dir == ImGuiDir_Up || dir == ImGuiDir_Down ? p.y + size.y <= display.y : p.x + size.x <= display.x;
    };

    const std::array<ImGuiDir, 4>& dirs = order[idx(side)];
    ImVec2                         pos  = outside(dirs.back(), btn);
    for (const ImGuiDir dir : dirs)
    {
        if (fits(dir, outside(dir, avoid)))
        {
            pos = outside(dir, avoid);
            break;
        }
    }

    pos.x = std::clamp(pos.x, 0.0f, std::max(0.0f, display.x - size.x));
    pos.y = std::clamp(pos.y, 0.0f, std::max(0.0f, display.y - size.y));
    return pos;
}

// The top-left corner of a toolbar row `length` px long, centered on `border` of `sel` and `out` px away from it
static ImVec2 toolbar_row_pos(const selection_rect_t& sel,
                              SelectionBorder         border,
                              const ImVec2&           out,
                              const float             length,
                              const ImVec2&           btn)
{
    const float centered_x = sel.get_x() + (sel.get_width() - length) * 0.5f;
    const float centered_y = sel.get_y() + (sel.get_height() - length) * 0.5f;
    switch (border)
    {
        case SelectionBorder::Bottom: return { centered_x, sel.get_y() + sel.get_height() + out.y };
        case SelectionBorder::Top:    return { centered_x, sel.get_y() - out.y - btn.y };
        case SelectionBorder::Right:  return { sel.get_x() + sel.get_width() + out.x, centered_y };
        default:                      return { sel.get_x() - out.x - btn.x, centered_y };
    }
}

// The borderless, auto-sized window a toolbar row is drawn in
static void begin_toolbar_window(const char* name, const ImVec2& pos)
{
    ImGui::SetNextWindowPos(pos);
    // The overlay leaves no window padding, so the border would clip the buttons' outer pixel and rounding
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(name,
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
}

// A box of `size` in the color of a text field, with `hint` centered in it, for an output that's empty
static void draw_output_hint(const char* hint, const ImVec2& size)
{
    const ImGuiStyle& style     = ImGui::GetStyle();
    ImFont*           font      = ImGui::GetFont();
    const float       font_size = ImGui::GetFontSize();
    const float       wrap      = std::max(size.x - style.FramePadding.x * 4.0f, 1.0f);
    const char*       end       = hint + std::strlen(hint);

    std::vector<std::string_view> lines;
    for (const char* p = hint; p < end;)
    {
        const char* line_end = font->CalcWordWrapPosition(font_size, p, end, wrap);
        if (line_end == p)  // a word longer than the line
            line_end += ImTextCountUtf8BytesFromChar(p, end);
        lines.emplace_back(p, line_end);
        p = ImTextCalcWordWrapNextLineStart(line_end, end);
    }

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImU32  col = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        min, ImVec2(min.x + size.x, min.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);

    float y = std::round(min.y + (size.y - font_size * float(lines.size())) * 0.5f);
    for (const std::string_view line : lines)
    {
        const float w = ImGui::CalcTextSize(line.data(), line.data() + line.size()).x;
        draw_list->AddText(
            ImVec2(std::round(min.x + (size.x - w) * 0.5f), y), col, line.data(), line.data() + line.size());
        y += font_size;
    }
    ImGui::Dummy(size);
}

// An output's text box, or `hint` in its place while it's empty and can't be edited
static void draw_output_text(const char*  id,
                             std::string& text,
                             const bool   editable,
                             const bool   wrap,
                             const char*  hint,
                             const float  height)
{
    if (text.empty() && !editable)
    {
        draw_output_hint(hint, ImVec2(ImGui::GetContentRegionAvail().x, height));
        return;
    }

    ImGuiInputTextFlags flags = editable ? 0 : ImGuiInputTextFlags_ReadOnly;
    if (wrap)
        flags |= ImGuiInputTextFlags_WordWrap;
    ImGui::InputTextMultiline(id, &text, ImVec2(-FLT_MIN, height), flags);
}

// The height of a text box that shows `lines` lines
static float text_box_height(const float lines)
{
    return ImGui::GetTextLineHeight() * lines + ImGui::GetStyle().FramePadding.y * 2.0f;
}

// Buttons in the colors of the text fields, for the ones next to a main action. A `pressed` one is drawn pushed in.
// Pops with ImGui::PopStyleColor(4)
static void push_secondary_button_colors(const bool pressed = false)
{
    const ImVec4* colors = ImGui::GetStyle().Colors;
    ImGui::PushStyleColor(ImGuiCol_Button, colors[pressed ? ImGuiCol_HeaderActive : ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_FrameBgActive]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_HeaderActive]);
    ImGui::PushStyleColor(ImGuiCol_Text, colors[pressed ? ImGuiCol_CheckMark : ImGuiCol_Text]);
}

// The non-empty parts separated by dots, e.g. "6 lines · 48 words · 420 ms"
static std::string join_stats(std::initializer_list<std::string> parts)
{
    std::string line;
    for (const std::string& part : parts)
    {
        if (part.empty())
            continue;
        if (!line.empty())
            line += " · ";
        line += part;
    }
    return line;
}

static void draw_stats(const std::string& stats)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", stats.c_str());
    ImGui::PopStyleColor();
}

// The height draw_stats() takes, with the spacing under it
static float stats_height(const std::string& stats)
{
    return ImGui::CalcTextSize(stats.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x).y +
           ImGui::GetStyle().ItemSpacing.y;
}

// How long a scan took, in ms under a second
static std::string duration_str(const float seconds)
{
    if (seconds >= 1.0f)
        return fmt::format("{:.2f} s", seconds);
    return seconds < 0.001f ? "<1 ms" : fmt::format("{:.0f} ms", seconds * 1000.0f);
}

void ScreenshotTool::DrawASelectionBorder(selection_info_t& sel,
                                          const float       sel_x,
                                          const float       sel_y,
                                          const float       sel_w,
                                          const float       sel_h)
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    // Draw selection border
    draw_list->AddRect(ImVec2(sel_x, sel_y),
                       ImVec2(sel_x + sel_w, sel_y + sel_h),
                       theme::color(AppCol::SelectionBorder).to_abgr(),
                       0.0f,
                       1.0f,
                       ImDrawFlags_None);

    // Draw handles
    constexpr float handle_draw = 2.0f;
    auto            draw_handle = [&](ImVec2 pos, HandleHovered type) {
        if (!g_config->Runtime.enable_handles)
            return;

        ImVec2 min = ImVec2(pos.x - handle_draw, pos.y - handle_draw);
        ImVec2 max = ImVec2(pos.x + handle_draw, pos.y + handle_draw);

        const bool   hovered = sel.handle_hover == type || sel.dragging_handle == type;
        const rgba_t color   = theme::color(hovered ? AppCol::SelectionHandleHovered : AppCol::SelectionHandle);

        draw_list->AddRectFilled(min, max, color.to_abgr());
        draw_list->AddRect(min, max, theme::color(AppCol::SelectionHandle).to_abgr(), 0.0f, 2.0f, ImDrawFlags_None);
    };

    // Corner handles
    draw_handle(ImVec2(sel_x, sel_y), HandleHovered::TopLeft);
    draw_handle(ImVec2(sel_x + sel_w, sel_y), HandleHovered::TopRight);
    draw_handle(ImVec2(sel_x, sel_y + sel_h), HandleHovered::BottomLeft);
    draw_handle(ImVec2(sel_x + sel_w, sel_y + sel_h), HandleHovered::BottomRight);

    // Edge handles
    draw_handle(ImVec2(sel_x + sel_w / 2, sel_y), HandleHovered::Top);
    draw_handle(ImVec2(sel_x + sel_w / 2, sel_y + sel_h), HandleHovered::Bottom);
    draw_handle(ImVec2(sel_x, sel_y + sel_h / 2), HandleHovered::Left);
    draw_handle(ImVec2(sel_x + sel_w, sel_y + sel_h / 2), HandleHovered::Right);

    if (sel.is_ann)
        return;

    // Selection size, shown while the selection changes and for 2 seconds after
    static double last_change_time = ImGui::GetTime();
    if (m_input_owner == InputOwner::Selection)
        last_change_time = ImGui::GetTime();

    if ((ImGui::GetTime() - last_change_time) < 2.f)
    {
        // Drawn every frame, and fmt's buffer keeps a label this short on the stack
        fmt::memory_buffer str;
        fmt::format_to(std::back_inserter(str), "{:.0f}x{:.0f}+{:.0f}+{:.0f}", sel_w, sel_h, sel_x, sel_y);
        if (g_config->File.image_out_size_fmt != "off")
        {
            static size_t           estimated_size = 0;
            static selection_rect_t tracked_selection{};    // last geometry observed, any source
            static selection_rect_t committed_selection{};  // geometry estimated_size currently reflects
            static std::chrono::steady_clock::time_point last_change_ts{};

            auto same_geo = [](const selection_rect_t& a, const selection_rect_t& b) {
                return a.get_x() == b.get_x() && a.get_y() == b.get_y() && a.get_width() == b.get_width() &&
                       a.get_height() == b.get_height();
            };

            if (!same_geo(sel.selection, tracked_selection))
            {
                tracked_selection = sel.selection;
                last_change_ts    = std::chrono::steady_clock::now();
            }

            if (m_size_estimate.valid() && m_size_estimate.wait_for(0s) == std::future_status::ready)
                estimated_size = m_size_estimate.get();

            if (!m_size_estimate.valid() && !same_geo(tracked_selection, committed_selection) &&
                std::chrono::steady_clock::now() - last_change_ts > 150ms)
            {
                committed_selection = tracked_selection;

                capture_result_t snapshot = GetFinalImage();  // render thread only, cheap crop
                ImageExt         ext      = g_config->File.image_out_type.second;

                m_size_estimate = std::async(std::launch::async, [snapshot = std::move(snapshot), ext] {
                    return encode_to_image(snapshot, ext).size();
                });
            }
            byte_units_t byte_units = (g_config->File.image_out_size_fmt == "auto")
                                          ? auto_divide_bytes(double(estimated_size), ByteUnit::Kibibyte)
                                          : divide_bytes(double(estimated_size), g_config->File.image_out_size_fmt);

            fmt::format_to(std::back_inserter(str),
                           "\n{}: {:.2f}{}",
                           g_config->File.image_out_type.first,
                           byte_units.num_bytes,
                           byte_units.unit);
        }
        const ImVec2 str_size = ImGui::CalcTextSize(str.begin(), str.end());

        constexpr float kPaddingX = 6.0f;
        constexpr float kPaddingY = 4.5f;
        constexpr float kMargin   = 3.0f;  // gap between selection corner and the label

        const ImVec2 win_size(str_size.x + kPaddingX * 2.0f, str_size.y + kPaddingY * 2.0f);
        const ImVec2 win_pos((sel_x + sel_w) - win_size.x - kMargin, (sel_y + sel_h) - win_size.y - kMargin);

        ImGui::SetNextWindowPos(win_pos);
        ImGui::SetNextWindowSize(win_size);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPaddingX, kPaddingY));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color(AppCol::SizeLabelBg));
        ImGui::PushStyleColor(ImGuiCol_Border, theme::color(AppCol::SizeLabelBorder));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color(AppCol::SizeLabelText));

        if (ImGui::Begin("##selection_size_window",
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize))
            ImGui::TextUnformatted(str.begin(), str.end());
        ImGui::End();

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
    }
}

void ScreenshotTool::CreateCopyTextButton(const std::string&     text_copy,
                                          const std::string_view label1,
                                          const ImVec2&          size,
                                          const bool             show_error)
{
    ErrorContext<GeneralError>& ectx = m_general_errors;

    // Each button says "Copied!" on its own, e.g. the ones in the rows of a table
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGui::PushID("##copy_armed");
    const ImGuiID key = ImGui::GetID(label1.data(), label1.data() + label1.size());
    ImGui::PopID();
    bool armed = storage->GetBool(key);
    if (create_timed_button(label1, _("Copied!"), armed, size))
    {
        const Result<>& res = g_clipboard.CopyText(text_copy);
        if (res.ok())
        {
            ectx.Clear(GeneralError::FailedToCopyText);
            armed = true;
        }
        else
        {
            ectx.Set(GeneralError::FailedToCopyText, res.error_v());
        }
    }
    storage->SetBool(key, armed);

    if (show_error && ectx.Has(GeneralError::FailedToCopyText))
    {
        ImGui::SameLine();
        ImGui::TextColored(theme::color(AppCol::Error),
                           _("Failed to copy text: %s"),
                           ectx.Get(GeneralError::FailedToCopyText).c_str());
    }
}

void ScreenshotTool::DrawDarkOverlay()
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    const float sel_x = m_main_sel.selection.get_x();
    const float sel_y = m_main_sel.selection.get_y();
    const float sel_w = m_main_sel.selection.get_width();
    const float sel_h = m_main_sel.selection.get_height();

    const ImU32 dark_color = theme::color(AppCol::SelectionDim).to_abgr();

    // Above, below, left and right of the selection
    draw_list->AddRectFilled(m_image_origin, ImVec2(m_image_end.x, sel_y), dark_color);
    draw_list->AddRectFilled(ImVec2(m_image_origin.x, sel_y + sel_h), m_image_end, dark_color);
    draw_list->AddRectFilled(ImVec2(m_image_origin.x, sel_y), ImVec2(sel_x, sel_y + sel_h), dark_color);
    draw_list->AddRectFilled(ImVec2(sel_x + sel_w, sel_y), ImVec2(m_image_end.x, sel_y + sel_h), dark_color);
}

void ScreenshotTool::DrawAnnotations()
{
    static_assert(sizeof(point_t) == sizeof(ImVec2) && alignof(point_t) == alignof(ImVec2),
                  "point_t and ImVec2 layout mismatch");

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const float dpi       = ImGui::GetIO().DisplayFramebufferScale.x;

    auto draw_line = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        draw_list->AddLine(p1, p2, ann.color.to_abgr(), t);
    };

    auto draw_text = [&](const annotation_t& ann, const ImVec2& p1) {
        const float font_size = ann.thickness > 8.0f ? ann.thickness : ImGui::GetFontSize();
        ImFont*     font      = CacheAndGetFont(m_inputs.resolved_ann_font_path, font_size);
        draw_list->AddText(font, font_size, p1, ann.color.to_abgr(), ann.text.c_str());
    };

    auto draw_rectangle_or_filled =
        [&](const bool filled, const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
            ImVec2 min(std::min(p1.x, p2.x), std::min(p1.y, p2.y));
            ImVec2 max(std::max(p1.x, p2.x), std::max(p1.y, p2.y));
            filled ? draw_list->AddRectFilled(min, max, ann.color.to_abgr(), 0.0f, ImDrawFlags_None)
                   : draw_list->AddRect(min, max, ann.color.to_abgr(), 0.0f, t, ImDrawFlags_None);
        };

    auto draw_circle_or_filled =
        [&](const bool filled, const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
            float dx     = p2.x - p1.x;
            float dy     = p2.y - p1.y;
            float radius = std::sqrt(dx * dx + dy * dy);
            filled ? draw_list->AddCircleFilled(p1, radius, ann.color.to_abgr(), 0)
                   : draw_list->AddCircle(p1, radius, ann.color.to_abgr(), 0, t);
        };

    auto draw_counter_bubble = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        draw_circle_or_filled(false, ann, p1, p2, t);

        const std::string& label = fmt::to_string(ann.count);

        const float dx     = p2.x - p1.x;
        const float dy     = p2.y - p1.y;
        const float radius = std::sqrt(dx * dx + dy * dy);
        // ~70% of the diameter
        const float font_size = std::max(8.0f, radius * 1.4f);
        ImFont*     font      = CacheAndGetFont(m_inputs.resolved_ann_font_path, font_size);

        const ImVec2 text_size =
            font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.c_str()) : ImGui::CalcTextSize(label.c_str());

        const ImVec2 text_pos(p1.x - text_size.x * 0.5f, p1.y - text_size.y * 0.5f);
        draw_list->AddText(font, font_size, text_pos, ann.color.to_abgr(), label.c_str());
    };

    auto draw_pencil = [&](const annotation_t& ann, const float t) {
        if (ann.points.empty())
            return;

        const ImU32 col     = ann.color.to_abgr();
        const float r       = std::max(t * 0.5f, 0.5f);
        const float step    = std::max(r * 0.6f, 1.0f);  // spacing between stamps
        const float step_sq = step * step;
        const int   segs    = r < 4.0f ? 8 : 0;  // 0 = ImGui picks based on radius

        ImVec2 last(ann.points[0].x, ann.points[0].y);
        draw_list->AddCircleFilled(last, r, col, segs);

        for (size_t i = 1; i < ann.points.size(); ++i)
        {
            const float dx   = ann.points[i].x - last.x;
            const float dy   = ann.points[i].y - last.y;
            const float d_sq = dx * dx + dy * dy;
            if (d_sq < step_sq)
                continue;  // too close to the last stamp, skip

            const float d  = std::sqrt(d_sq);
            const int   n  = int(d / step);
            const float sx = dx / d * step;
            const float sy = dy / d * step;

            for (int k = 0; k < n; ++k)
            {
                last.x += sx;
                last.y += sy;
                draw_list->AddCircleFilled(last, r, col, segs);
            }
        }

        // make sure the stroke ends exactly where the mouse ended
        const ImVec2 end(ann.points.back().x, ann.points.back().y);
        if (end.x != last.x || end.y != last.y)
            draw_list->AddCircleFilled(end, r, col, segs);
    };

    auto draw_arrow = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        ImVec2 v(p2.x - p1.x, p2.y - p1.y);
        float  len = std::sqrt(v.x * v.x + v.y * v.y);
        if (len < 1.0f)
            return;

        ImVec2 dir(v.x / len, v.y / len);
        ImVec2 perp(-dir.y, dir.x);  // unit perpendicular

        float head_len = 6.0f * t;
        float head_w   = 4.0f * t;

        head_len = std::min(head_len, len * 0.6f);

        ImVec2 base(p2.x - dir.x * head_len, p2.y - dir.y * head_len);
        ImVec2 left(base.x + perp.x * (head_w * 0.5f), base.y + perp.y * (head_w * 0.5f));
        ImVec2 right(base.x - perp.x * (head_w * 0.5f), base.y - perp.y * (head_w * 0.5f));

        // shaft
        draw_list->AddLine(p1, base, ann.color.to_abgr(), t);

        // head
        draw_list->AddTriangleFilled(p2, left, right, ann.color.to_abgr());
    };

    auto draw_annotation = [&](const annotation_t& ann) {
        const ImVec2 p1(ann.start.x, ann.start.y);
        const ImVec2 p2(ann.end.x, ann.end.y);
        const float  t = ann.thickness * dpi;

        switch (ann.type)
        {
            case ToolType::Line:            draw_line(ann, p1, p2, t); break;
            case ToolType::Arrow:           draw_arrow(ann, p1, p2, t); break;
            case ToolType::Rectangle:       draw_rectangle_or_filled(false, ann, p1, p2, t); break;
            case ToolType::RectangleFilled: draw_rectangle_or_filled(true, ann, p1, p2, t); break;
            case ToolType::Circle:          draw_circle_or_filled(false, ann, p1, p2, t); break;
            case ToolType::CircleFilled:    draw_circle_or_filled(true, ann, p1, p2, t); break;
            case ToolType::CounterBubble:   draw_counter_bubble(ann, p1, p2, t); break;
            case ToolType::Text:            draw_text(ann, p1); break;
            case ToolType::Pencil:          draw_pencil(ann, t); break;

            case ToolType::kNone:
            case ToolType::ToggleTextTools:
            case ToolType::CopyImage:
            case ToolType::SaveImage:
            case ToolType::Logo:
            case ToolType::COUNT:           break;
        }
    };

    for (const annotation_t& ann : m_annotations)
        draw_annotation(ann);

    // The one being drawn, on top
    if (m_current_actions.Has(CurrentAction::IsDrawing))
        draw_annotation(m_current_annotation);
}

void ScreenshotTool::DrawSelectionBorder()
{
    const float sel_x = m_main_sel.selection.get_x();
    const float sel_y = m_main_sel.selection.get_y();
    const float sel_w = m_main_sel.selection.get_width();
    const float sel_h = m_main_sel.selection.get_height();

    DrawASelectionBorder(m_main_sel, sel_x, sel_y, sel_w, sel_h);
}

void ScreenshotTool::DrawAnnotationResizeBorder()
{
    const annotation_t* ann = m_current_annotation_resize;
    if (!ann)
        return;

    constexpr float PIXEL = 1.0f;

    const float padding = ann->thickness - PIXEL;
    if (padding == 0.0f)
        return;

    const ImRect box = GetAnnotationHandleBox(*ann);
    DrawASelectionBorder(m_anno_sel, box.Min.x, box.Min.y, box.GetWidth() - PIXEL, box.GetHeight() - PIXEL);
}

void ScreenshotTool::DrawAnnotationSettingsPopup(const ToolType tool, const SelectionBorder side)
{
    const ImGuiStyle& style = ImGui::GetStyle();

    // ImGui would open it at the cursor, over the toolbar and the selection
    if (ImGui::IsPopupOpen("##ann_settings"))
    {
        const ImVec2 btn     = GetToolButtonSize();
        const bool   rings   = m_fill_tools_complete && !g_config->File.old_toolbar_pos;
        const ImVec2 out     = GetToolbarRingOffset(rings ? m_toolbar_rows.back().ring : 0);
        const ImVec2 display = ImGui::GetIO().DisplaySize;

        // The selection and every ring of its toolbar
        const selection_rect_t& sel = m_main_sel.selection;
        const ImRect            avoid(sel.get_x() - out.x - btn.x,
                                      sel.get_y() - out.y - btn.y,
                                      sel.get_x() + sel.get_width() + out.x + btn.x,
                                      sel.get_y() + sel.get_height() + out.y + btn.y);

        // Off screen while its size isn't known yet, or it would show in the wrong place for a frame
        ImVec2 pos(-display.x - m_ann_popup_size.x, -display.y - m_ann_popup_size.y);
        if (m_ann_popup_placed)
            pos = toolbar_popup_pos(
                ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), avoid, side, m_ann_popup_size);
        else
            RequestRedraw();
        ImGui::SetNextWindowPos(pos);
    }

    // The overlay's zero padding would put the contents on the popup's border
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.ItemSpacing.x, style.ItemSpacing.x));
    const bool open = ImGui::BeginPopupContextItem("##ann_settings");
    ImGui::PopStyleVar();
    if (!open)
        return;

    const ImVec2 popup_size = ImGui::GetWindowSize();
    m_ann_popup_placed      = popup_size.x == m_ann_popup_size.x && popup_size.y == m_ann_popup_size.y;
    m_ann_popup_size        = popup_size;

    float&     thickness = m_tool_thickness[idx(tool)];
    const bool is_text   = tool == ToolType::Text;
    thickness            = is_text ? std::clamp(thickness, 8.0f, 144.0f) : std::clamp(thickness, 1.0f, 10.0f);

    const ImGuiColorEditFlags flags = ann_color_picker_flags();
    const float               width = ImGui::GetFontSize() * 16.0f;
    ImGui::SeparatorText(tool_name(tool));

    if (ImGui::BeginTable("##ann_settings_form", 2, ImGuiTableFlags_SizingFixedFit, ImVec2(width, 0)))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        if (is_text)
        {
            form_row(_("Size"), _("The font size of the new text"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat("##font_size", &thickness, 2.0f, 8.0f, "%.0f px");

            form_row(_("Font"),
                     _("A font file name or path for the new text. Empty uses the interface font. Supports "
                       "drag-and-drop"));
            constexpr SDL_DialogFileFilter filter{ .name = N_("Fonts"), .pattern = "ttf;otf;ttc" };
            ImGui::PushItemWidth(-FLT_MIN);
            draw_input_text_file(
                "",
                "##font_path_ann_settings",
                filter,
                [&] { m_inputs.resolved_ann_font_path = get_font_path(m_inputs.ann_font).string(); },
                m_inputs.ann_font);
            ImGui::PopItemWidth();
        }
        // A filled shape has no outline to size
        else if (tool != ToolType::RectangleFilled && tool != ToolType::CircleFilled)
        {
            form_row(_("Thickness"), _("The width of the lines"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##thickness", &thickness, 1.0f, 10.0f, "%.0f px");
        }

        form_row(_("Color"), _("Shared by all the tools"));
        const float frame_h = ImGui::GetFrameHeight();
        ImGui::ColorButton("##current_color",
                           m_current_color,
                           flags & ImGuiColorEditFlags_AlphaMask_,
                           ImVec2(frame_h * 2.0f, frame_h));
        ImGui::SameLine();
        if (ImGui::Button(_("Pick from screen")))
        {
            m_current_actions.Set(CurrentAction::IsColorPicking);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("%s", _("Click anywhere on the image to pick a color"));

        ImGui::EndTable();
    }

    ImVec4 picker = m_current_color;
    ImGui::SetNextItemWidth(width);
    if (ImGui::ColorPicker4("##color", reinterpret_cast<float*>(&picker), flags))
    {
        m_current_color = rgba_t(picker);
        g_cache->SetValue(CacheEntry::AnnColor, m_current_color.to_rgba());
    }
    ImGui::EndPopup();
}

void ScreenshotTool::DrawAnnotationToolbar()
{
    // `side` is the selection border the button is on, which its settings popup opens away from
    auto draw_button = [&](ToolType tool, SelectionBorder side, bool same_line) {
        // Shown only while its window is closed
        if (tool == ToolType::ToggleTextTools && m_show_window.Has(SubWindow::MainTextTools))
            return;

        const bool is_tool  = is_annotation_tool(tool);
        const bool selected = is_tool && m_current_tool == tool;

        ImGui::PushID(int(tool));
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, theme::color(AppCol::ToolSelected));
        const bool clicked = ImGui::ImageButton("##btn", m_tool_textures[idx(tool)].GetTexID(), TOOL_BUTTON_SIZE2);
        if (selected)
            ImGui::PopStyleColor();

        const char* name = tool_name(tool);
        switch (tool)
        {
            case ToolType::SaveImage: ImGui::SetItemTooltip("%s (%s+S)", name, SHORTCUT_MOD); break;
            case ToolType::CopyImage:
                ImGui::SetItemTooltip(
                    "%s (%s+%sC)", name, SHORTCUT_MOD, g_config->File.ctrl_c_copy_img ? "" : "SHIFT+");
                break;
            default:
                if (is_tool)
                    ImGui::SetItemTooltip("%s\n%s", name, _("Right-click for its color and size"));
                else
                    ImGui::SetItemTooltip("%s", name);
        }

        if (is_tool)
        {
            if (clicked)
                m_current_tool = selected ? ToolType::kNone : tool;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                m_current_tool = tool;
            DrawAnnotationSettingsPopup(tool, side);
        }
        else if (clicked && tool == ToolType::ToggleTextTools)
        {
            m_show_window.Set(SubWindow::MainTextTools);
        }
        else if (clicked)
        {
            RequestComplete(tool == ToolType::SaveImage ? SavingOp::File : SavingOp::Clipboard);
        }
        ImGui::PopID();

        if (same_line)
            ImGui::SameLine();
    };

    const ImGuiStyle&       style = ImGui::GetStyle();
    const ImVec2            btn   = GetToolButtonSize();
    const selection_rect_t& sel   = m_main_sel.selection;

    // Laid out again when the Text tools window opens or closes, which hides or shows its button
    auto has_text_tools_button = [](const toolbar_row_t& row) {
        return std::ranges::find(row.tools, ToolType::ToggleTextTools) != row.tools.end();
    };
    if (m_fill_tools_complete &&
        std::ranges::any_of(m_toolbar_rows, has_text_tools_button) == m_show_window.Has(SubWindow::MainTextTools))
        UpdateAnnsToolbarPos();

    const size_t n_plugins = CountPluginToolbarButtons();

    if (m_fill_tools_complete && !g_config->File.old_toolbar_pos)
    {
        for (size_t i = 0; i < m_toolbar_rows.size(); ++i)
        {
            const toolbar_row_t& row      = m_toolbar_rows[i];
            const bool           has_save = row.tools.back() == ToolType::SaveImage;
            const bool horizontal         = row.border == SelectionBorder::Bottom || row.border == SelectionBorder::Top;

            // Centered with the plugins' buttons, which come after SaveImage
            const auto   n      = float(row.tools.size() + (has_save ? n_plugins : 0));
            const float  length = horizontal ? n * btn.x + (n - 1.0f) * style.ItemSpacing.x
                                             : n * btn.y + (n - 1.0f) * style.ItemSpacing.y;
            const ImVec2 pos    = toolbar_row_pos(sel, row.border, GetToolbarRingOffset(row.ring), length, btn);

            // Kept on the stack since it's drawn every frame and is too long for std::string's inline buffer
            std::array<char, 32> name{};
            fmt::format_to_n(name.data(), name.size() - 1, "##annotation_toolbar_n{}", i);
            begin_toolbar_window(name.data(), ImVec2(pos.x - style.WindowPadding.x, pos.y - style.WindowPadding.y));

            for (const ToolType tool : row.tools)
                draw_button(tool, row.border, horizontal);
#ifndef DISABLE_PLUGINS
            if (has_save)
                DrawPluginToolbarButtons(horizontal);
#endif

            ImGui::End();
        }
        return;
    }

    // One row of buttons below the selection, else above it, else on screen wherever it fits
    const std::vector<ToolType> tools   = GetToolbarButtons();
    const ImVec2                display = ImGui::GetIO().DisplaySize;
    const auto                  n       = float(tools.size() + n_plugins);
    const float toolbar_w = n * (btn.x + style.ItemSpacing.x) - style.ItemSpacing.x + style.WindowPadding.x * 2.0f;
    const float toolbar_h = btn.y + style.WindowPadding.y * 2.0f;

    const float below_y   = sel.get_y() + sel.get_height() + TOOLBAR_OFFSET;
    const float above_y   = sel.get_y() - TOOLBAR_OFFSET - toolbar_h;
    float       toolbar_y = below_y;
    if (below_y + toolbar_h > display.y)
        toolbar_y = above_y >= 0.0f ? above_y : std::clamp(below_y, 0.0f, display.y - toolbar_h);
    const float toolbar_x = std::clamp(sel.get_x(), 0.0f, std::max(0.0f, display.x - toolbar_w));

    begin_toolbar_window("##annotation_toolbar", ImVec2(toolbar_x, toolbar_y));
    const SelectionBorder side = toolbar_y < sel.get_y() ? SelectionBorder::Top : SelectionBorder::Bottom;
    for (const ToolType tool : tools)
        draw_button(tool, side, true);
#ifndef DISABLE_PLUGINS
    DrawPluginToolbarButtons(true);
#endif
    ImGui::End();
}

void ScreenshotTool::DrawMenuItems()
{
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu(_("File")))
        {
            if (ImGui::MenuItem(_("Open Image...")))
            {
                const bool hidden = hide_window();

                constexpr SDL_DialogFileFilter    filter{ .name = N_("Images"), .pattern = "png;jpeg;jpg;bmp" };
                const std::optional<std::string>& open_path = dialog::open_file(_("Open Image"), "", filter);

                if (hidden)
                    show_window();

                if (open_path)
                    OpenImage(*open_path);
            }

            ImGui::Separator();

            if (ImGui::MenuItem(_("Save Image"), SHORTCUT_MOD "+S"))
                RequestComplete(SavingOp::File);

            if (ImGui::MenuItem(_("Copy Image"),
                                g_config->File.ctrl_c_copy_img ? SHORTCUT_MOD "+C" : SHORTCUT_MOD "+SHIFT+C"))
                RequestComplete(SavingOp::Clipboard);

            ImGui::Separator();

            if (ImGui::MenuItem(_("Quit"), "ESC"))
                Cancel();

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(_("Edit")))
        {
            ImGui::MenuItem(_("View Handles"), SHORTCUT_MOD "+G", &g_config->Runtime.enable_handles);

            ImGui::Separator();
            if (ImGui::MenuItem(_("Preferences...")))
                m_show_window.Set(SubWindow::Preferences);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(_("Tools")))
        {
            if (ImGui::MenuItem(_("Download OCR model")))
                m_show_window.Set(SubWindow::OcrDownload);
            ImGui::Separator();

#ifndef DISABLE_PLUGINS
            if (ImGui::MenuItem(_("Browse Plugins...")))
                m_show_window.Set(SubWindow::BrowsePlugins);
            if (ImGui::MenuItem(_("Manage Plugins")))
                m_show_window.Set(SubWindow::ManagePlugins);
            if (ImGui::MenuItem(_("Install Plugins...")))
                m_show_window.Set(SubWindow::InstallPlugins);
            ImGui::Separator();
#endif

            if (ImGui::MenuItem(_("View Logs")))
                m_show_window.Set(SubWindow::Logs);
            ImGui::EndMenu();
        }

#ifndef DISABLE_PLUGINS
        DrawPluginActionsMenu();
#endif

        if (ImGui::BeginMenu(_("Help")))
        {
            if (ImGui::MenuItem(_("About")))
                m_show_window.Set(SubWindow::About);
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }
}

void ScreenshotTool::DrawOutputButtons(const std::string& text,
                                       const std::string& original,
                                       bool&              show_original,
                                       const char*        copy_label,
                                       const bool         show_wrap)
{
    if (original.empty())
        show_original = false;

    const float spacing    = ImGui::GetStyle().ItemSpacing.x;
    const char* wrap_label = _("Wrap");
    const char* orig_label = _("Original");
    const float copy_w     = button_width({ copy_label, _("Copied!") });

    float width = copy_w;
    if (show_wrap)
        width += button_width({ wrap_label }) + spacing;
    if (!original.empty())
        width += button_width({ orig_label }) + spacing;
    align_right(width);

    auto toggle = [](const char* label, bool& on, const char* tooltip) {
        push_secondary_button_colors(on);
        if (ImGui::Button(label))
            on = !on;
        ImGui::PopStyleColor(4);
        ImGui::SetItemTooltip("%s", tooltip);
        ImGui::SameLine();
    };
    if (show_wrap)
        toggle(wrap_label, m_wrap_text, _("Wrap the lines that are wider than the text box"));
    if (!original.empty())
        toggle(orig_label,
               show_original,
               _("Show the text as it was read, before the plugins' text transformers changed it"));

    const std::string& shown = show_original ? original : text;
    ImGui::BeginDisabled(shown.empty());
    push_secondary_button_colors();
    CreateCopyTextButton(shown, copy_label, ImVec2(copy_w, 0.0f), false);
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();

    if (m_general_errors.Has(GeneralError::FailedToCopyText))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color(AppCol::Error));
        ImGui::TextWrapped(_("Failed to copy text: %s"), m_general_errors.Get(GeneralError::FailedToCopyText).c_str());
        ImGui::PopStyleColor();
    }
}

void ScreenshotTool::DrawOcrTools()
{
    ErrorContext<OcrError>& ectx = m_ocr_errors;

    const bool need_to_scan  = ectx.Has(OcrError::NeedToScanDir);
    const bool invalid_path  = ectx.Has(OcrError::InvalidPath);
    const bool invalid_model = ectx.Has(OcrError::InvalidModel);
    const bool models_ok     = !invalid_path && !need_to_scan;

    ImGui::PushID("OcrTools");

    if (ImGui::BeginTable("##ocr_settings", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        // --- Path input ---
        form_row(_("Folder"), _("The folder with the OCR models (.traineddata). Supports drag-and-drop"));
        const char* load_label = _("Load");
        if (invalid_path || need_to_scan)
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color(AppCol::Error));
        ImGui::PushItemWidth(need_to_scan ? -(button_width({ load_label }) + ImGui::GetStyle().ItemSpacing.x)
                                          : -FLT_MIN);
        draw_input_text_folder("", "##ocr_path", [&] { ectx.Set(OcrError::NeedToScanDir); }, m_inputs.ocr_path);
        ImGui::PopItemWidth();
        if (invalid_path || need_to_scan)
            ImGui::PopStyleColor();
        if (need_to_scan)
        {
            ImGui::SameLine();
            if (ImGui::Button(load_label))
                RefreshOcrModels();
            ImGui::SetItemTooltip("%s", _("Look for the models in this folder"));
        }

        if (models_ok)
        {
            // --- Model combo ---
            form_row(_("Model"), _("The model of the language the text is in"));
            std::string& ocr_model = m_inputs.ocr_model;
            if (invalid_model)
                ImGui::PushStyleColor(ImGuiCol_Text, theme::color(AppCol::Error));
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool combo_open = ImGui::BeginCombo("##model", ocr_model.c_str(), ImGuiComboFlags_HeightLarge);
            // Don't color the whole list red
            if (invalid_model)
            {
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s", _("Not in the models folder, pick another one"));
            }

            if (combo_open)
            {
                static ImGuiTextFilter filter;
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere();
                    filter.Clear();
                }

                ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F);
                filter.Draw("##Filter", -FLT_MIN);
                // Esc would only leave the filter, so one press closes the list
                if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape))
                    ImGui::CloseCurrentPopup();

                for (const std::string& model : m_ocr_models_list)
                {
                    if (filter.PassFilter(model.c_str()) && ImGui::Selectable(model.c_str(), model == ocr_model))
                    {
                        ocr_model = model;
                        ectx.Clear(OcrError::InvalidModel);
                    }
                }
                ImGui::EndCombo();
            }

            // --- Layout combo ---
            // The layouts Tesseract can assume, as in Edit > Optimize OCR for...
            form_row(_("Layout"), _("How the text is laid out. Automatic tries a few and keeps the best"));
            const auto  layouts = std::to_array<std::pair<int, const char*>>({
                { 0, _("Automatic") },
                { tesseract::PSM_SINGLE_WORD, _("Single Word") },
                { tesseract::PSM_SINGLE_LINE, _("Single Line") },
                { tesseract::PSM_SINGLE_BLOCK, _("Paragraph") },
                { tesseract::PSM_AUTO, _("Automatic (Layout)") },
            });
            int&        psm     = g_config->Runtime.preferred_psm;
            const char* preview = layouts[0].second;
            for (const auto& [value, label] : layouts)
                if (value == psm)
                    preview = label;

            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##psm", preview))
            {
                for (const auto& [value, label] : layouts)
                    if (ImGui::Selectable(label, value == psm))
                        psm = value;
                ImGui::EndCombo();
            }
        }
        ImGui::EndTable();
    }

    // --- Suggestions ---
    if (need_to_scan)
    {
        ImGui::TextWrapped("%s", _("Press Load to look for the OCR models in this folder."));
    }
    else if (invalid_path)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color(AppCol::Error));
        ImGui::TextWrapped("%s", _("No OCR models (.traineddata) in this folder"));
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::TextDisabled("%s", _("Suggestions:"));

        if (g_is_nix)
        {
            constexpr const char* cmd = "nix build --no-link --print-out-paths nixpkgs#tesseract";
            ImGui::TextWrapped(
                "%s",
                _("Run the following command in your terminal, then update the OCR path in the Preferences window:"));
            ImGui::TextColored(theme::color(AppCol::Warning), "%s", cmd);
            CreateCopyTextButton(cmd, _("Copy command"));
            ImGui::Spacing();
        }

#if !OSHOT_WINDOWS
        if (!g_is_flatpak)
        {
            ImGui::Bullet();
            ImGui::TextWrapped("%s",
                               _("Install one of the tesseract language packages (e.g. tesseract-data-eng on Arch, "
                                 "tesseract-ocr-eng on Debian/Ubuntu, tesseract-langpack-eng on Fedora)."));
        }
#endif
        ImGui::Bullet();
        ImGui::TextWrapped("%s", _("Download a tesseract language model via 'Tools -> Download OCR model'."));
        ImGui::Spacing();
    }

    // --- Extract button and output ---
    ImGui::BeginDisabled(!models_ok || invalid_model);
    if (ImGui::Button(_("Extract Text")))
        RequestScan(ScanType::Ocr);
    ImGui::EndDisabled();

    DrawOutputButtons(m_inputs.ocr_results.data, m_inputs.ocr_original, m_show_ocr_original, _("Copy"), true);
    ShowIfError(ectx, OcrError::FailedToOCR);

    const ocr_result_t& res      = m_inputs.ocr_results;
    const bool          scanned  = res.confidence >= 0;
    const bool          original = m_show_ocr_original;
    std::string&        text     = original ? m_inputs.ocr_original : m_inputs.ocr_results.data;

    std::string stats;
    if (scanned)
    {
        // Lines with text, words and characters (not bytes)
        size_t lines = 0, words = 0, chars = 0;
        bool   in_word = false, in_line = false;
        for (const char c : text)
        {
            const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
            chars += (uint8_t(c) & 0xC0) != 0x80;
            words += !space && !in_word;
            lines += !space && !in_line;
            in_word = !space;
            in_line = c != '\n' && (in_line || !space);
        }
        stats = join_stats({ i18n::tr_n_fmt("{} line", "{} lines", long(lines), lines),
                             i18n::tr_n_fmt("{} word", "{} words", long(words), words),
                             i18n::tr_n_fmt("{} character", "{} characters", long(chars), chars),
                             duration_str(m_inputs.ocr_seconds),
                             res.psm_str });
    }

    // The text box fills the tab, leaving room for the confidence bar and the stats
    const float below = scanned ? ImGui::GetFrameHeightWithSpacing() + stats_height(stats) : 0.0f;
    const float height =
        std::max(text_box_height(6), ImGui::GetContentRegionAvail().y - below - ImGui::GetStyle().ItemSpacing.y);
    draw_output_text(original ? "##ocr_original" : "##" OSHOT_TEXT_OCR,
                     text,
                     !original && g_config->File.allow_out_edit,
                     m_wrap_text,
                     scanned ? _("No text found. Try another model or layout")
                             : _("Select an area and press Extract Text to read its text"),
                     height);

    if (scanned)
    {
        const rgba_t col = theme::color(res.confidence <= 45   ? AppCol::ConfidenceLow
                                        : res.confidence <= 70 ? AppCol::ConfidenceMid
                                                               : AppCol::ConfidenceHigh);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(float(res.confidence) / 100.0f,
                           ImVec2(-FLT_MIN, 0.0f),
                           i18n::tr_fmt("Confidence {}%", res.confidence).c_str());
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(
            "%s", _("How sure the OCR is of the words it read. If the text seems off, try another model or layout"));

        draw_stats(stats);
    }

    ImGui::PopID();
}

void ScreenshotTool::DrawBarcodeTable(float height)
{
    const ImGuiStyle&             style = ImGui::GetStyle();
    const std::vector<barcode_t>& codes = m_inputs.barcode_scan_result.codes;

    if (m_code_rows.empty())
    {
        m_selected_code = -1;
        draw_output_hint(m_inputs.barcode_scanned
                             ? _("No QR codes or barcodes found")
                             : _("Select an area and press Decode to read its QR codes and barcodes"),
                         ImVec2(ImGui::GetContentRegionAvail().x, height));
        return;
    }
    if (std::cmp_greater_equal(m_selected_code, m_code_rows.size()))
        m_selected_code = -1;

    // --- Filter, once the list gets long ---
    const bool filtering = m_code_rows.size() > 10;
    if (filtering)
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##code_filter",
                                     _("Filter the codes by their text or type"),
                                     m_code_filter.InputBuf,
                                     IM_ARRAYSIZE(m_code_filter.InputBuf)))
            m_code_filter.Build();
        height -= ImGui::GetFrameHeightWithSpacing();
    }

    std::vector<size_t> shown;
    for (size_t i = 0; i < m_code_rows.size(); ++i)
    {
        const barcode_t& code = codes[m_code_rows[i].code];
        if (!filtering || m_code_filter.PassFilter(code.data.c_str()) || m_code_filter.PassFilter(code.type.c_str()))
            shown.push_back(i);
    }

    // The clicked code's whole text goes under the table, up to 6 lines before it scrolls
    const barcode_t* selected = m_selected_code >= 0 ? &codes[m_code_rows[size_t(m_selected_code)].code] : nullptr;
    float            full_h   = 0.0f;
    if (selected)
    {
        const float wrap = ImGui::GetContentRegionAvail().x - style.FramePadding.x * 2.0f - style.ScrollbarSize;
        const float text = ImGui::CalcTextSize(selected->data.c_str(), nullptr, false, wrap).y;
        full_h           = std::min(text, ImGui::GetTextLineHeight() * 6.0f) + style.FramePadding.y * 2.0f;
    }

    // The table, or the hint that stands in for it, takes the rest
    const float table_h =
        std::max(ImGui::GetFrameHeight() * 3.0f, height - (selected ? full_h + style.ItemSpacing.y : 0.0f));

    if (shown.empty())
    {
        draw_output_hint(_("No code matches the filter"), ImVec2(ImGui::GetContentRegionAvail().x, table_h));
    }
    else
    {
        const char* copy_label = _("Copy");
        const float copy_w     = button_width({ copy_label, _("Copied!") });

        // Each row's data, wrapped to the Data column and cut to 3 lines, again only when the column's width changes
        auto wrap_rows = [&](const float width) {
            if (width == m_code_rows_width)
                return;
            m_code_rows_width = width;

            ImFont*     font       = ImGui::GetFont();
            const float font_size  = ImGui::GetFontSize();
            const float ellipsis_w = ImGui::CalcTextSize("…").x;
            for (code_row_t& row : m_code_rows)
            {
                const std::string& data = codes[row.code].data;
                const char*        p    = data.data();
                const char*        end  = p + data.size();

                row.text.clear();
                for (row.lines = 0; p < end && row.lines < 3; ++row.lines)
                {
                    const char* para_end = std::find(p, end, '\n');
                    const char* line_end = font->CalcWordWrapPosition(font_size, p, para_end, width);
                    if (line_end == p && p < para_end)  // not even one character fits
                        line_end += ImTextCountUtf8BytesFromChar(p, para_end);

                    if (row.lines > 0)
                        row.text += '\n';
                    row.text.append(p, line_end);

                    if (line_end < para_end)
                        p = ImTextCalcWordWrapNextLineStart(line_end, para_end);
                    else
                        p = para_end < end ? para_end + 1 : end;
                }

                // Some was left out, so the last line ends with an ellipsis
                if (p < end)
                {
                    const size_t last = row.text.rfind('\n') + 1;  // npos + 1 is 0
                    while (row.text.size() > last &&
                           ImGui::CalcTextSize(row.text.c_str() + last).x + ellipsis_w > width)
                    {
                        while (row.text.size() > last && (uint8_t(row.text.back()) & 0xC0) == 0x80)
                            row.text.pop_back();
                        if (row.text.size() > last)
                            row.text.pop_back();
                    }
                    row.text += "…";
                }
            }
        };

        // Before the table exists (the first frame) the Data column's width is estimated from the other columns
        if (m_code_rows_width <= 0.0f)
        {
            float type_w = 0.0f;
            for (const code_row_t& row : m_code_rows)
                type_w = std::max(type_w, tag_width(codes[row.code].type.c_str()));
            wrap_rows(std::max(
                ImGui::GetContentRegionAvail().x - type_w - copy_w - style.CellPadding.x * 6.0f - style.ScrollbarSize,
                50.0f));
        }

        constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
                                          ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##codes", 3, flags, ImVec2(0.0f, table_h)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(_("Type"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(_("Data"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##copy", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            // The header's cell is as wide as the column
            ImGui::TableSetColumnIndex(1);
            wrap_rows(ImGui::GetContentRegionAvail().x);

            for (const size_t i : shown)
            {
                const code_row_t& row   = m_code_rows[i];
                const barcode_t&  code  = codes[row.code];
                const float       row_h = std::max(ImGui::GetFrameHeight(), text_box_height(float(row.lines)));

                ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h + style.CellPadding.y * 2.0f);
                ImGui::TableSetColumnIndex(0);

                // Rows scrolled out of view only take their height
                if (!ImGui::IsRectVisible(ImVec2(1.0f, row_h)))
                    continue;

                ImGui::PushID(int(i));

                // The whole row can be clicked, under its tag, text and button. The selected one is tinted with
                // the accent, so it stands out from the row colors
                const ImVec2 cell        = ImGui::GetCursorPos();
                const bool   is_selected = std::cmp_equal(m_selected_code, i);
                ImVec4       accent      = ImGui::GetStyleColorVec4(ImGuiCol_Button);
                accent.w *= 0.35f;
                ImGui::PushStyleColor(ImGuiCol_Header, accent);
                if (ImGui::Selectable("##row",
                                      is_selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0.0f, row_h)))
                    m_selected_code = is_selected ? -1 : int(i);
                ImGui::PopStyleColor();
                if (ImGui::BeginItemTooltip())
                {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                    ImGui::TextUnformatted(code.data.c_str());
                    ImGui::Separator();
                    ImGui::TextDisabled(
                        "%s",
                        i18n::tr_fmt(
                            "At {:.0f}, {:.0f}, {:.0f}×{:.0f} pixels in the selection", code.x, code.y, code.w, code.h)
                            .c_str());
                    ImGui::TextDisabled(
                        "%s", is_selected ? _("Click to hide the whole text") : _("Click to show the whole text"));
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }

                ImGui::SetCursorPos(cell);
                draw_tag(code.type.c_str(), theme::color(AppCol::Tag), ImGui::GetFrameHeight());

                ImGui::TableSetColumnIndex(1);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.FramePadding.y);
                ImGui::TextUnformatted(row.text.c_str());  // one item, so no spacing between its lines

                ImGui::TableSetColumnIndex(2);
                push_secondary_button_colors();
                CreateCopyTextButton(code.data, copy_label, ImVec2(copy_w, 0.0f), false);
                ImGui::PopStyleColor(4);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ReadOnly, so ImGui never writes to the data
    if (selected)
        ImGui::InputTextMultiline("##code_full",
                                  const_cast<char*>(selected->data.c_str()),
                                  selected->data.size() + 1,
                                  ImVec2(-FLT_MIN, full_h),
                                  ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
}

void ScreenshotTool::DrawBarDecodeTools()
{
    const barcode_result_t& res = m_inputs.barcode_scan_result;

    ImGui::PushID("BarDecodeTools");

    if (ImGui::Button(_("Decode")))
        RequestScan(ScanType::Barcode);

    // The text box only when it says more than the table, since a plugin changed it or it can be edited
    std::string payloads;
    for (const std::string& data : res.datas)
        payloads += data + "\n\n";
    const bool show_text =
        g_config->File.allow_out_edit || (!m_inputs.barcode_text.empty() && m_inputs.barcode_text != payloads);

    DrawOutputButtons(
        m_inputs.barcode_text, m_inputs.barcode_original, m_show_barcode_original, _("Copy all"), show_text);
    ShowIfError(m_barcode_errors, BarcodeError::FailedToScan);

    std::string stats;
    if (m_inputs.barcode_scanned)
    {
        std::vector<std::pair<std::string, int>> symbologies(res.symbologies.begin(), res.symbologies.end());
        std::ranges::sort(symbologies);
        std::string kinds;
        for (const auto& [sym, count] : symbologies)
            kinds += fmt::format("{}{} ×{}", kinds.empty() ? "" : ", ", sym, count);

        stats = join_stats({ i18n::tr_n_fmt("{} code", "{} codes", long(res.datas.size()), res.datas.size()),
                             kinds,
                             duration_str(m_inputs.barcode_seconds) });
    }

    // The table takes what the text box and the stats leave
    const float text_h = text_box_height(4);
    float       below  = ImGui::GetStyle().ItemSpacing.y;
    if (show_text)
        below += text_h + ImGui::GetStyle().ItemSpacing.y;
    if (!stats.empty())
        below += stats_height(stats);
    DrawBarcodeTable(std::max(text_box_height(5), ImGui::GetContentRegionAvail().y - below));

    if (show_text)
    {
        const bool original = m_show_barcode_original;
        draw_output_text(original ? "##barcode_original" : "##" OSHOT_TEXT_BARCODE,
                         original ? m_inputs.barcode_original : m_inputs.barcode_text,
                         !original && g_config->File.allow_out_edit,
                         m_wrap_text,
                         "",
                         text_h);
    }

    if (!stats.empty())
        draw_stats(stats);

    ImGui::PopID();
}
