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
#include <filesystem>
#include <iterator>
#include <memory>
#include <string_view>
#include <thread>
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
#include "tinyfiledialogs.h"
#include "util.hpp"
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
                       (0x0096ffFF_rgba).to_abgr(),
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

        auto color = Colors::WHITE;
        if (sel.handle_hover == type || sel.dragging_handle == type)
            color.b = 0;  // yellow

        draw_list->AddRectFilled(min, max, color.to_abgr());
        draw_list->AddRect(min, max, Colors::WHITE.to_abgr(), 0.0f, 2.0f, ImDrawFlags_None);
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
            static std::atomic<size_t> estimated_size{ 0 };
            static std::atomic<bool>   size_computing{ false };
            static selection_rect_t    tracked_selection{};    // last geometry observed, any source
            static selection_rect_t    committed_selection{};  // geometry estimated_size currently reflects
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

            if (!size_computing && !same_geo(tracked_selection, committed_selection) &&
                std::chrono::steady_clock::now() - last_change_ts > 150ms)
            {
                committed_selection = tracked_selection;
                size_computing      = true;

                capture_result_t snapshot = GetFinalImage();  // render thread only, cheap crop
                ImageExt         ext      = g_config->File.image_out_type.second;

                std::thread([snapshot = std::move(snapshot), ext]() mutable {
                    std::vector<uint8_t> img = encode_to_image(snapshot, ext);
                    estimated_size.store(img.size(), std::memory_order_relaxed);
                    size_computing.store(false, std::memory_order_relaxed);
                }).detach();
            }
            byte_units_t byte_units =
                (g_config->File.image_out_size_fmt == "auto")
                    ? auto_divide_bytes(double(estimated_size.load()), ByteUnit::Kibibyte)
                    : divide_bytes(double(estimated_size.load()), g_config->File.image_out_size_fmt);

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
        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPaddingX, kPaddingY));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, (0x0F0F0FFF_rgba));
        ImGui::PushStyleColor(ImGuiCol_Border, (0xFFFFFF40_rgba));
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::WHITE);

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

void ScreenshotTool::CreateCopyTextButton(const std::string& text_copy, const std::string_view label1)
{
    ErrorContext<GeneralError>& ectx = m_general_errors;

    static bool armed = false;
    if (create_timed_button(label1, _("Copied!"), armed))
    {
        const Result<>& res = g_clipboard.CopyText(text_copy);
        if (res.ok())
        {
            ClearError(ectx, GeneralError::FailedToCopyText);
            armed = true;
        }
        else
        {
            SetError(ectx, GeneralError::FailedToCopyText, res.error_v());
        }
    }

    if (HasError(ectx, GeneralError::FailedToCopyText))
    {
        ImGui::SameLine();
        ImGui::TextColored(
            Colors::RED, _("Failed to copy text: %s"), GetError(ectx, GeneralError::FailedToCopyText).c_str());
    }
}

void ScreenshotTool::DrawDarkOverlay()
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    const float sel_x = m_main_sel.selection.get_x();
    const float sel_y = m_main_sel.selection.get_y();
    const float sel_w = m_main_sel.selection.get_width();
    const float sel_h = m_main_sel.selection.get_height();

    constexpr ImU32 dark_color = (0x00000080_rgba).to_abgr();

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

void ScreenshotTool::DrawAnnotationToolbar()
{
    auto draw_and_set_button = [&](ToolType tool, const char* id, ImTextureRef texture) -> bool {
        bool       clicked  = false;
        const bool is_tool  = is_annotation_tool(tool);
        const bool selected = is_tool && (m_current_tool == tool);

        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, (0x6699FFFF_rgba));

        if (ImGui::ImageButton(id, texture.GetTexID(), TOOL_BUTTON_SIZE2))
        {
            if (is_tool)
                m_current_tool = selected ? ToolType::kNone : tool;
            clicked = true;
        }

        if (selected)
            ImGui::PopStyleColor();

        if (!is_tool)
            return clicked;

        // Right-click popup on this item
        if (selected && ImGui::BeginPopupContextItem())
        {
            if (m_current_tool == ToolType::Text)
                m_tool_thickness[idx(m_current_tool)] = std::clamp(m_tool_thickness[idx(m_current_tool)], 8.0f, 144.0f);
            else
                m_tool_thickness[idx(m_current_tool)] = std::clamp(m_tool_thickness[idx(m_current_tool)], 1.0f, 10.0f);

            ImGuiColorEditFlags flags = g_config->File.color_picker == 0 ? ImGuiColorEditFlags_PickerHueBar
                                                                         : ImGuiColorEditFlags_PickerHueWheel;

            switch (ColorPickerAlpha(g_config->File.cpa_mode))
            {
                case ColorPickerAlpha::Disabled: flags |= ImGuiColorEditFlags_NoAlpha; break;
                case ColorPickerAlpha::Bar:      flags |= ImGuiColorEditFlags_AlphaBar; break;
                case ColorPickerAlpha::Inline:   /* no extra flag needed */ break;
            }

            ImGui::TextUnformatted(_("Annotation Settings"));
            ImGui::Separator();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);

            if (m_current_tool == ToolType::Text)
            {
                ImGui::InputFloat("##fontsize", &m_tool_thickness[idx(m_current_tool)], 2.0f, 2.0f, "%.0f px");
                ImGui::SameLine();
                ImGui::TextUnformatted(_("Font Size"));
                static constexpr auto font_filters = std::to_array({ "*.ttf", "*.otf", "*.ttc" });
                draw_input_text_file(
                    _("Font name/path"),
                    "##font_path_ann_settings",
                    font_filters,
                    [&] { m_inputs.resolved_ann_font_path = get_font_path(m_inputs.ann_font).string(); },
                    m_inputs.ann_font);
            }
            else
            {
                ImGui::SliderFloat("##thickness", &m_tool_thickness[idx(m_current_tool)], 1.0f, 10.0f, "%.2f");
                ImGui::SameLine();
                ImGui::TextUnformatted(_("Thickness"));
            }

            if (ImGui::Button(_("Pick color")))
            {
                m_current_actions.Set(CurrentAction::IsColorPicking);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            HelpMarker(_("Click anywhere on the image to pick a color"));

            ImVec4 picker = m_current_color;
            ImGui::ColorPicker4(_("Color"), reinterpret_cast<float*>(&picker), flags);

            m_current_color = rgba_t(picker);
            g_cache->SetValue(CacheEntry::AnnColor, m_current_color.to_rgba());
            ImGui::EndPopup();
        }

        return clicked;
    };

    auto set_tool_button = [&](ToolType tool, bool same_line) {
        ImGui::PushID(int(tool));
        switch (tool)
        {
            case ToolType::kNone:
            case ToolType::COUNT:
            case ToolType::Logo:  break;

            case ToolType::ToggleTextTools:
                if (!m_show_window.Has(SubWindow::MainTextTools))
                    if (draw_and_set_button(tool, "##btn", m_tool_textures[idx(tool)]))
                        m_show_window.Set(SubWindow::MainTextTools);
                break;

            case ToolType::CopyImage:
            case ToolType::SaveImage:
                if (draw_and_set_button(tool, "##btn", m_tool_textures[idx(tool)]))
                    RequestComplete(tool == ToolType::SaveImage ? SavingOp::File : SavingOp::Clipboard);
                break;

            default: draw_and_set_button(tool, "##btn", m_tool_textures[idx(tool)]);
        }
        ImGui::PopID();

        if (same_line)
            ImGui::SameLine();
    };

    const float sel_x = m_main_sel.selection.get_x();
    const float sel_y = m_main_sel.selection.get_y();
    const float sel_w = m_main_sel.selection.get_width();
    const float sel_h = m_main_sel.selection.get_height();

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2      btn   = GetToolButtonSize();

    if (m_fill_tools_complete && !g_config->File.old_toolbar_pos)
    {
        std::array<ImVec2, idx(SelectionBorder::COUNT)> first_btn_pos{};
        first_btn_pos[idx(SelectionBorder::Bottom)] = ImVec2(sel_x, sel_y + sel_h + TOOLBAR_OFFSET);
        first_btn_pos[idx(SelectionBorder::Right)]  = ImVec2(sel_x + sel_w + TOOLBAR_OFFSET, sel_y);
        first_btn_pos[idx(SelectionBorder::Top)]    = ImVec2(sel_x, sel_y - TOOLBAR_OFFSET - btn.y);
        first_btn_pos[idx(SelectionBorder::Left)]   = ImVec2(sel_x - TOOLBAR_OFFSET - btn.x, sel_y);

        for (size_t i = 0; i < m_tools_borders.size(); ++i)
        {
            if (m_tools_borders[i].empty())
                continue;

            // Kept on the stack since it's drawn every frame and is too long for std::string's inline buffer
            std::array<char, 32> name{};
            fmt::format_to_n(name.data(), name.size() - 1, "##annotation_toolbar_n{}", i);

            ImGui::SetNextWindowPos(
                ImVec2(first_btn_pos[i].x - style.WindowPadding.x, first_btn_pos[i].y - style.WindowPadding.y));
            // The overlay leaves no window padding, so the border would clip the buttons' outer pixel and rounding
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::Begin(name.data(),
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::PopStyleVar();

            for (const ToolType tool : m_tools_borders[i])
                set_tool_button(tool, i % 2 == 0);
#ifndef DISABLE_PLUGINS
            // The plugins' buttons continue after oshot's last one
            if (m_tools_borders[i].back() == ToolType::SaveImage)
                DrawPluginToolbarButtons(i % 2 == 0);
#endif

            ImGui::End();
        }
    }
    else
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;

        // One row of buttons, as big as the window will be
        size_t n_buttons = 0;
        for (size_t i = 1; i < idx(ToolType::COUNT); ++i)
            n_buttons += has_toolbar_button(toe<ToolType>(i)) ? 1 : 0;
#ifndef DISABLE_PLUGINS
        for (const auto& [id, rt] : g_plugins)
            n_buttons += std::ranges::count(rt.actions, OSHOT_PLACE_TOOLBAR, &plugin_action_t::place);
#endif
        const float toolbar_w =
            (float(n_buttons) * (btn.x + style.ItemSpacing.x)) - style.ItemSpacing.x + (style.WindowPadding.x * 2.0f);
        const float toolbar_h = btn.y + (style.WindowPadding.y * 2.0f);

        const float below_y = sel_y + sel_h + TOOLBAR_OFFSET;
        const float above_y = sel_y - TOOLBAR_OFFSET - toolbar_h;

        float toolbar_y = below_y;

        // Prefer below
        if (below_y + toolbar_h > display.y)
        {
            // Try above
            toolbar_y = above_y;

            // If above also invalid, clamp
            if (toolbar_y < 0.0f)
                toolbar_y = std::clamp(below_y, 0.0f, display.y - toolbar_h);
        }

        const float toolbar_x = std::clamp(sel_x, 0.0f, std::max(0.0f, display.x - toolbar_w));

        ImGui::SetNextWindowPos({ toolbar_x, toolbar_y });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##annotation_toolbar",
                     nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PopStyleVar();

        for (size_t i = 1; i < idx(ToolType::COUNT); ++i)
            set_tool_button(toe<ToolType>(i), true);
#ifndef DISABLE_PLUGINS
        DrawPluginToolbarButtons(true);
#endif

        ImGui::End();
    }
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

                constexpr auto filter    = std::to_array({ "*.png", "*.jpeg", "*.jpg", "*.bmp" });
                const char*    open_path = tinyfd_openFileDialog(_("Open Image"),
                                                                 "",             // default path
                                                                 filter.size(),  // number of filter patterns
                                                                 filter.data(),  // file filters
                                                                 _("Images"),    // filter description
                                                                 false           // allow multiple selections
                );

                if (hidden)
                    show_window();

                if (open_path)
                    OpenImage(open_path);
            }

            ImGui::Separator();

            if (ImGui::MenuItem(_("Save Image"), "CTRL+S"))
                RequestComplete(SavingOp::File);

            if (ImGui::MenuItem(_("Copy Image"), g_config->File.ctrl_c_copy_img ? "CTRL+C" : "CTRL+SHIFT+C"))
                RequestComplete(SavingOp::Clipboard);

            ImGui::Separator();

            if (ImGui::MenuItem(_("Quit"), "ESC"))
                Cancel();

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(_("Edit")))
        {
            if (ImGui::BeginMenu(_("Optimize OCR for...")))
            {
                if (ImGui::RadioButton(_("Automatic"), g_config->Runtime.preferred_psm == 0))
                    g_config->Runtime.preferred_psm = 0;
                ImGui::RadioButton(_("Single Word"), &g_config->Runtime.preferred_psm, tesseract::PSM_SINGLE_WORD);
                ImGui::RadioButton(_("Single Line"), &g_config->Runtime.preferred_psm, tesseract::PSM_SINGLE_LINE);
                ImGui::RadioButton(_("Paragraph"), &g_config->Runtime.preferred_psm, tesseract::PSM_SINGLE_BLOCK);
                ImGui::RadioButton(_("Automatic (Layout)"), &g_config->Runtime.preferred_psm, tesseract::PSM_AUTO);
                ImGui::EndMenu();
            }

            ImGui::Separator();
            ImGui::MenuItem(_("View Handles"), "CTRL+G", &g_config->Runtime.enable_handles);
            ImGui::MenuItem(_("Anns. in image scans"), "", &g_config->File.render_anns);
            if (ImGui::MenuItem(_("Enable vsync"), "", &g_config->File.enable_vsync))
                extern_glfwSwapInterval(int(g_config->File.enable_vsync));
            if (ImGui::MenuItem(_("Allow text edit"), "CTRL+E", &g_config->File.allow_out_edit))
                ImGui::ClearActiveID();

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

void ScreenshotTool::DrawOcrTools()
{
    ErrorContext<OcrError>& ectx = m_ocr_errors;

    std::string& ocr_path  = m_inputs.ocr_path;
    std::string& ocr_model = m_inputs.ocr_model;

    static size_t item_selected_idx = 0;

    auto push_error_style = [](bool cond) {
        if (cond)
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::RED);
    };
    auto pop_error_label = [](bool cond, const char* label) {
        if (cond)
        {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(Colors::RED, "%s", label);
        }
    };

    ImGui::PushID("OcrTools");
    ImGui::SeparatorText("OCR");

    const bool need_to_scan  = HasError(ectx, OcrError::NeedToScanDir);
    const bool invalid_path  = HasError(ectx, OcrError::InvalidPath);
    const bool invalid_model = HasError(ectx, OcrError::InvalidModel);

    // --- Path input ---
    push_error_style(invalid_path);
    push_error_style(need_to_scan);
    draw_input_text_folder(_("Path"), "##ocr_path", [&] { SetError(ectx, OcrError::NeedToScanDir); }, ocr_path);
    if (need_to_scan && ImGui::Button(_("Scan")))
        RefreshOcrModels();
    ImGui::SameLine();
    const auto& it    = std::ranges::find(m_ocr_models_list, ocr_model);
    item_selected_idx = (it != m_ocr_models_list.end()) ? std::distance(m_ocr_models_list.begin(), it) : 0;
    pop_error_label(invalid_path, _("Invalid!"));
    pop_error_label(need_to_scan, _("Need to scan new directory"));
    ImGui::SameLine();
    HelpMarker(_("Full path to the OCR models (.traineddata). Supports drag-and-drop."));

    if (invalid_path)
    {
        ImGui::Spacing();
        ImGui::TextColored((0x0AEFFFF_rgba), "%s", _("Suggestions:"));
        ImGui::Indent();

        if (g_is_nix)
        {
            constexpr const char* cmd = "nix build --no-link --print-out-paths nixpkgs#tesseract";
            ImGui::TextWrapped(
                "%s",
                _("Run the following command in your terminal, then update the OCR path in the Preferences window:"));
            ImGui::TextColored((0xFFCC33FF_rgba), cmd);
            CreateCopyTextButton(cmd, _("Copy command"));
            ImGui::Spacing();
        }

#if !OSHOT_WINDOWS
        if (!g_is_flatpak)
            ImGui::BulletText(
                "%s",
                _("Install one of the tesseract language packages (e.g. tesseract-data-eng on Arch, tesseract-ocr-eng "
                  "on Debian/Ubuntu, tesseract-langpack-eng on Fedora)."));
#endif
        ImGui::BulletText("%s", _("Download a tesseract language model via 'Tools -> Download OCR Model'."));
        ImGui::Unindent();
    }

    // --- Model combo (only shown when path is valid and isn't changed) ---
    if (!invalid_path && !need_to_scan)
    {
        push_error_style(invalid_model);

        const bool combo_open = ImGui::BeginCombo(_("Model"), ocr_model.c_str(), ImGuiComboFlags_HeightLarge);

        // Don't color the whole list red
        if (invalid_model)
            ImGui::PopStyleColor();

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

            for (size_t i = 0; i < m_ocr_models_list.size(); ++i)
            {
                if (filter.PassFilter(m_ocr_models_list[i].c_str()))
                {
                    const bool is_selected = (item_selected_idx == i);
                    if (ImGui::Selectable(m_ocr_models_list[i].c_str(), is_selected))
                    {
                        item_selected_idx = i;
                        ocr_model         = m_ocr_models_list[i];
                        ClearError(ectx, OcrError::InvalidModel);
                    }
                }
            }
            ImGui::EndCombo();
        }

        if (invalid_model)
        {
            ImGui::SameLine();
            ImGui::TextColored(Colors::RED, "%s", _("Invalid!"));
        }
    }

    // --- Extract button + result details ---
    if (!invalid_path && !invalid_model && !need_to_scan)
    {
        if (ImGui::Button(_("Extract Text")))
            RequestScan(ScanType::Ocr);

        ImGui::SameLine();

        if (HasError(ectx, OcrError::FailedToOCR))
        {
            ImGui::NewLine();  // own line, it can be wider than the panel
            ShowIfError(ectx, OcrError::FailedToOCR);
        }
        else
        {
            ImGui::SameLine();
            HelpMarker(_("If results seem off, try Edit > Optimize OCR for..."));
        }

        if (m_inputs.ocr_results.confidence > 0 && ImGui::TreeNode(_("Details")))
        {
            ImGui::BulletText("%s", _("Confidence:"));
            ImGui::SameLine();
            ImVec4 confidence_color(Colors::GREEN);
            if (m_inputs.ocr_results.confidence <= 45)
                confidence_color = Colors::RED;
            else if (m_inputs.ocr_results.confidence <= 70)
                confidence_color = Colors::YELLOW;

            ImGui::TextColored(confidence_color, "%d%%", m_inputs.ocr_results.confidence);

            ImGui::BulletText("PSM: %s", m_inputs.ocr_results.psm_str.c_str());
            ImGui::TreePop();
        }
    }

#ifndef DISABLE_PLUGINS
    DrawOriginalToggle(m_inputs.ocr_original, m_show_ocr_original);
    if (m_show_ocr_original)
    {
        ImGui::InputTextMultiline("##ocr_original",
                                  &m_inputs.ocr_original,
                                  ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                                  ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
        CreateCopyTextButton(m_inputs.ocr_original);
        ImGui::PopID();
        return;
    }
#endif
    ImGui::InputTextMultiline(
        "##" OSHOT_TEXT_OCR,
        &m_inputs.ocr_results.data,
        ImVec2(-1, ImGui::GetTextLineHeight() * 8),
        ImGuiInputTextFlags_WordWrap | (g_config->File.allow_out_edit ? 0 : ImGuiInputTextFlags_ReadOnly));

    if (!m_inputs.ocr_results.data.empty())
        CreateCopyTextButton(m_inputs.ocr_results.data);

    ImGui::PopID();
}

void ScreenshotTool::DrawBarDecodeTools()
{
    ErrorContext<BarcodeError>& ectx = m_barcode_errors;

    ImGui::PushID("BarDecodeTools");
    ImGui::SeparatorText(_("QR/Bar Decode"));

    if (ImGui::Button(_("Extract Text")))
        RequestScan(ScanType::Barcode);

    if (!ShowIfError(ectx, BarcodeError::FailedToScan) && !m_inputs.barcode_scan_result.datas.empty() &&
        ImGui::TreeNode(_("Details")))
    {
        ImGui::TextUnformatted(_("Detected barcodes:"));
        for (const auto& [sym, count] : m_inputs.barcode_scan_result.symbologies)
            ImGui::BulletText("%s (x%d)", sym.c_str(), count);
        ImGui::TreePop();
    }

#ifndef DISABLE_PLUGINS
    DrawOriginalToggle(m_inputs.barcode_original, m_show_barcode_original);
    if (m_show_barcode_original)
    {
        ImGui::InputTextMultiline("##barcode_original",
                                  &m_inputs.barcode_original,
                                  ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                                  ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
        CreateCopyTextButton(m_inputs.barcode_original);
        ImGui::PopID();
        return;
    }
#endif
    ImGui::InputTextMultiline(
        "##" OSHOT_TEXT_BARCODE,
        &m_inputs.barcode_text,
        ImVec2(-1, ImGui::GetTextLineHeight() * 8),
        ImGuiInputTextFlags_WordWrap | (g_config->File.allow_out_edit ? 0 : ImGuiInputTextFlags_ReadOnly));

    if (!m_inputs.barcode_text.empty())
        CreateCopyTextButton(m_inputs.barcode_text);

    ImGui::PopID();
}
